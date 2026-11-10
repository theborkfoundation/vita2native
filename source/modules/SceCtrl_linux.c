#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <psp2/ctrl.h>

#include "host.h"
#include "log.h"
#include "module.h"
#include "modules/SceCtrl_script.h"
#include "util.h"

static SDL_GameController *g_controller;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized;
static bool g_input_debug;

static uint8_t axis_to_vita(int value)
{
    int converted;

    if (value < -32768)
        value = -32768;
    if (value > 32767)
        value = 32767;

    converted = (value + 32768) >> 8;

    return (uint8_t)converted;
}

static uint32_t controller_buttons(SDL_GameController *controller)
{
    uint32_t buttons = 0;

    if (!controller)
        return 0;

    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP))
        buttons |= SCE_CTRL_UP;
    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
        buttons |= SCE_CTRL_RIGHT;
    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
        buttons |= SCE_CTRL_DOWN;
    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
        buttons |= SCE_CTRL_LEFT;

    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A))
        buttons |= SCE_CTRL_CROSS;
    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B))
        buttons |= SCE_CTRL_CIRCLE;
    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_X))
        buttons |= SCE_CTRL_SQUARE;
    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_Y))
        buttons |= SCE_CTRL_TRIANGLE;

    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))
        buttons |= SCE_CTRL_LTRIGGER;
    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))
        buttons |= SCE_CTRL_RTRIGGER;

    if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8192)
        buttons |= SCE_CTRL_L1;
    if (SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192)
        buttons |= SCE_CTRL_R1;

    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSTICK))
        buttons |= SCE_CTRL_L3;
    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK))
        buttons |= SCE_CTRL_R3;

    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START))
        buttons |= SCE_CTRL_START;
    if (SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK))
        buttons |= SCE_CTRL_SELECT;

    return buttons;
}

static uint32_t keyboard_buttons(const Uint8 *keys)
{
    uint32_t buttons = 0;

    if (!keys)
        return 0;

    if (keys[SDL_SCANCODE_UP])
        buttons |= SCE_CTRL_UP;
    if (keys[SDL_SCANCODE_RIGHT])
        buttons |= SCE_CTRL_RIGHT;
    if (keys[SDL_SCANCODE_DOWN])
        buttons |= SCE_CTRL_DOWN;
    if (keys[SDL_SCANCODE_LEFT])
        buttons |= SCE_CTRL_LEFT;

    if (keys[SDL_SCANCODE_Z])
        buttons |= SCE_CTRL_CROSS;
    if (keys[SDL_SCANCODE_X])
        buttons |= SCE_CTRL_CIRCLE;
    if (keys[SDL_SCANCODE_A])
        buttons |= SCE_CTRL_SQUARE;
    if (keys[SDL_SCANCODE_S])
        buttons |= SCE_CTRL_TRIANGLE;

    if (keys[SDL_SCANCODE_Q])
        buttons |= SCE_CTRL_LTRIGGER;
    if (keys[SDL_SCANCODE_E])
        buttons |= SCE_CTRL_RTRIGGER;
    if (keys[SDL_SCANCODE_1])
        buttons |= SCE_CTRL_L1;
    if (keys[SDL_SCANCODE_2])
        buttons |= SCE_CTRL_R1;

    if (keys[SDL_SCANCODE_RETURN])
        buttons |= SCE_CTRL_START;
    if (keys[SDL_SCANCODE_BACKSPACE])
        buttons |= SCE_CTRL_SELECT;

    return buttons;
}

/* See SceCtrl_null.c: a peek fills and returns the whole requested count. */
#define CTRL_MAX_BUFFERED 64

EXPORT(SceCtrl, 0xA9C3CED6, int, sceCtrlPeekBufferPositive, int port, SceCtrlData *pad_data,
       int count)
{
    const Uint8 *keys;
    uint32_t buttons = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    SceCtrlData sample;

    if (port < 0 || port > 1)
        return SCE_CTRL_ERROR_NO_DEVICE;

    if (!pad_data || count <= 0)
        return -1;
    if (count > CTRL_MAX_BUFFERED)
        count = CTRL_MAX_BUFFERED;

    memset(&sample, 0, sizeof(sample));

    pthread_mutex_lock(&g_lock);

    keys = SDL_GetKeyboardState(NULL);
    buttons |= keyboard_buttons(keys);
    buttons |= SceCtrl_script_buttons();

    if (g_input_debug && buttons) {
        static uint32_t last;

        if (buttons != last)
            LOG("SceCtrl: buttons %08x (keys z=%d x=%d a=%d s=%d)", buttons, keys[SDL_SCANCODE_Z],
                keys[SDL_SCANCODE_X], keys[SDL_SCANCODE_A], keys[SDL_SCANCODE_S]);
        last = buttons;
    }

    if (g_controller) {
        buttons |= controller_buttons(g_controller);
        lx = axis_to_vita(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX));
        ly = axis_to_vita(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTY));
        rx = axis_to_vita(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTX));
        ry = axis_to_vita(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTY));
    }

    sample.buttons = buttons;
    sample.lx = lx;
    sample.ly = ly;
    sample.rx = rx;
    sample.ry = ry;
    sample.timeStamp = host_monotonic_ns() / 1000;

    pthread_mutex_unlock(&g_lock);

    memset(pad_data, 0, sizeof(*pad_data) * (size_t)count);
    for (int i = 0; i < count; i++) {
        pad_data[i] = sample;
        pad_data[i].timeStamp -= (uint64_t)i * 16667ull;
    }

    return count;
}

DECLARE_LIBRARY(SceCtrl, 0xD197E3C7);

int SceCtrl_init(void)
{
    if (g_initialized)
        return 0;

    g_input_debug = getenv("VITA2NATIVE_INPUT_DEBUG") != NULL;

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        LOG("SDL game controller init failed: %s", SDL_GetError());
        return -1;
    }

    SDL_GameControllerEventState(SDL_ENABLE);

    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i))
            continue;

        g_controller = SDL_GameControllerOpen(i);
        if (g_controller) {
            LOG("Opened game controller: %s", SDL_GameControllerName(g_controller));
            break;
        }
    }

    if (!g_controller)
        LOG("No game controller found; keyboard fallback active");

    g_initialized = true;

    return 0;
}
