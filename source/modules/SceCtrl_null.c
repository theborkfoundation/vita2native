/*
 * Controller backend.
 *
 * Input comes from two places and the states are ORed:
 *
 *   SDL      the video layer samples keyboards and game controllers on its own
 *            thread (see host_video_sdl.c). This is the desktop/X11 path and
 *            also covers any pad SDL can open when the video window exists.
 *   evdev    every /dev/input/event* node is opened below, which is the
 *            reliable path on the R36S where SDL may have no controller
 *            mapping and the pad is exposed as a plain gpio-keys device:
 *
 *   BTN_SOUTH/EAST/NORTH/WEST   face buttons (A/B/X/Y)
 *   BTN_TL/TR and BTN_TL2/TR2   shoulders
 *   BTN_DPAD_UP/DOWN/LEFT/RIGHT
 *   BTN_SELECT/START            present on some device trees
 *   BTN_TRIGGER_HAPPY1..5       the R36S F1..F5 buttons
 *   ABS_X/Y and ABS_RX/RY       sticks, roughly -1800..1800
 *
 * Keyboard arrow/ZXAS/QE/Enter/Backspace are mapped too, so the x86 builder VM
 * can drive a title from a serial console.
 *
 * Setting VITA2NATIVE_CTRL_SCRIPT installs a deterministic script on top,
 * keyed to the number of presented display frames:
 *
 *   VITA2NATIVE_CTRL_SCRIPT="2:down,4:,6:down,8:,10:cross,12:"
 *
 * Each entry is `<frame>:<button>[+<button>...]`; the state applies from that
 * frame until the next entry. An empty button list releases everything. The
 * frame counter comes from the display backend, so timing is unaffected by how
 * slow a software-rendered frame is on the host. Scripted buttons are ORed with
 * the physical pad, which keeps automated runs working on machines that also
 * have a controller attached.
 */
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <psp2/ctrl.h>

#include "host.h"
#include "module.h"
#include "modules/SceCtrl_script.h"
#include "util.h"

#define MAX_INPUT_DEVICES 12

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static bool g_thread_started;
static int g_fds[MAX_INPUT_DEVICES];
static int g_fd_count;
static bool g_debug;
static bool g_have_pad;
static bool g_swap_ab;

static unsigned int g_pad_buttons;
/* Presses that happened since the last peek. The guest may poll the pad as
   rarely as once per rendered frame (VitaShell does, at ~1.3 fps on the R36S),
   so a human tap would otherwise be released before it was ever sampled. */
static unsigned int g_pending_presses;
static uint8_t g_lx = 128;
static uint8_t g_ly = 128;
static uint8_t g_rx = 128;
static uint8_t g_ry = 128;
static unsigned long long g_peek_calls;
static unsigned int g_peek_logged_buttons;


/* ------------------------------------------------------------------------- */
/* evdev                                                                      */
/* ------------------------------------------------------------------------- */

static unsigned int key_to_button(unsigned int code)
{
    switch (code) {
    case BTN_SOUTH:
        /* Positional Vita layout: south is cross. Some shells label the east
           button A, so VITA2NATIVE_CTRL_SWAP_AB makes that one confirm. */
        return g_swap_ab ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
    case BTN_EAST:
        return g_swap_ab ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
    case BTN_NORTH:
        return SCE_CTRL_TRIANGLE;
    case BTN_WEST:
        return SCE_CTRL_SQUARE;
    case BTN_TL:
        return SCE_CTRL_L1;
    case BTN_TR:
        return SCE_CTRL_R1;
    case BTN_TL2:
        return SCE_CTRL_L2;
    case BTN_TR2:
        return SCE_CTRL_R2;
    case BTN_SELECT:
        return SCE_CTRL_SELECT;
    case BTN_START:
        return SCE_CTRL_START;
    case BTN_THUMBL:
        return SCE_CTRL_L3;
    case BTN_THUMBR:
        return SCE_CTRL_R3;
    case BTN_DPAD_UP:
        return SCE_CTRL_UP;
    case BTN_DPAD_DOWN:
        return SCE_CTRL_DOWN;
    case BTN_DPAD_LEFT:
        return SCE_CTRL_LEFT;
    case BTN_DPAD_RIGHT:
        return SCE_CTRL_RIGHT;

    /* The R36S has no BTN_SELECT/BTN_START on some device trees; its Select
       and Start buttons are wired as F1..F5 instead. Map the whole F range so
       either pair works, and leave FN/F5 alongside Start. */
    case BTN_TRIGGER_HAPPY1:
        return SCE_CTRL_SELECT;
    case BTN_TRIGGER_HAPPY2:
        return SCE_CTRL_START;
    case BTN_TRIGGER_HAPPY3:
        return SCE_CTRL_SELECT;
    case BTN_TRIGGER_HAPPY4:
        return SCE_CTRL_START;
    case BTN_TRIGGER_HAPPY5:
        return SCE_CTRL_START;

    /* Keyboard, for the builder VM and headless consoles. */
    case KEY_UP:
        return SCE_CTRL_UP;
    case KEY_DOWN:
        return SCE_CTRL_DOWN;
    case KEY_LEFT:
        return SCE_CTRL_LEFT;
    case KEY_RIGHT:
        return SCE_CTRL_RIGHT;
    case KEY_Z:
    case KEY_ENTER:
        return SCE_CTRL_CROSS;
    case KEY_X:
    case KEY_BACKSPACE:
        return SCE_CTRL_CIRCLE;
    case KEY_A:
        return SCE_CTRL_SQUARE;
    case KEY_S:
        return SCE_CTRL_TRIANGLE;
    case KEY_Q:
        return SCE_CTRL_L1;
    case KEY_E:
        return SCE_CTRL_R1;
    case KEY_TAB:
        return SCE_CTRL_SELECT;
    case KEY_SPACE:
        return SCE_CTRL_START;
    default:
        return 0;
    }
}

static uint8_t axis_to_vita(int value, const struct input_absinfo *range, bool invert)
{
    int span = range->maximum - range->minimum;
    int converted;

    if (span <= 0)
        return 128;
    if (value < range->minimum)
        value = range->minimum;
    if (value > range->maximum)
        value = range->maximum;

    converted = (int)(((long long)(value - range->minimum) * 255 + span / 2) / span);
    if (invert)
        converted = 255 - converted;

    return (uint8_t)converted;
}

static void update_axis(unsigned int code, const struct input_absinfo *range, int value)
{
    pthread_mutex_lock(&g_lock);
    switch (code) {
    case ABS_X:
        g_lx = axis_to_vita(value, range, false);
        break;
    case ABS_Y:
        g_ly = axis_to_vita(value, range, true);
        break;
    case ABS_RX:
        g_rx = axis_to_vita(value, range, false);
        break;
    case ABS_RY:
        g_ry = axis_to_vita(value, range, true);
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&g_lock);
}

static void *input_thread(void *arg)
{
    (void)arg;

    for (;;) {
        struct pollfd pollfds[MAX_INPUT_DEVICES];
        int count = g_fd_count;

        if (count <= 0)
            return NULL;

        for (int i = 0; i < count; i++) {
            pollfds[i].fd = g_fds[i];
            pollfds[i].events = POLLIN;
            pollfds[i].revents = 0;
        }

        if (poll(pollfds, (nfds_t)count, 250) <= 0)
            continue;

        for (int i = 0; i < count; i++) {
            struct input_event event;
            struct input_absinfo range;

            if (g_fds[i] < 0)
                continue;

            /* A uinput pad can disappear while we are reading it. */
            if (pollfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                close(g_fds[i]);
                g_fds[i] = -1;
                continue;
            }
            if (!(pollfds[i].revents & POLLIN))
                continue;

            while (read(g_fds[i], &event, sizeof(event)) == (ssize_t)sizeof(event)) {
                if (event.type == EV_KEY) {
                    unsigned int mask = key_to_button(event.code);

                    if (!mask)
                        continue;
                    if (g_debug)
                        fprintf(stderr, "vita2native: SceCtrl key 0x%x (%s) -> 0x%x\n",
                                event.code, event.value ? "down" : "up", mask);

                    pthread_mutex_lock(&g_lock);
                    if (event.value) {
                        g_pad_buttons |= mask;
                        /* value 2 is autorepeat, not a new press. */
                        if (event.value == 1)
                            g_pending_presses |= mask;
                    } else {
                        g_pad_buttons &= ~mask;
                    }
                    pthread_mutex_unlock(&g_lock);
                } else if (event.type == EV_ABS) {
                    memset(&range, 0, sizeof(range));
                    if (ioctl(g_fds[i], EVIOCGABS(event.code), &range) < 0 ||
                        range.minimum == range.maximum) {
                        range.minimum = -1800;
                        range.maximum = 1800;
                    }
                    update_axis(event.code, &range, event.value);
                }
            }
        }
    }
}

static bool input_open_devices(void)
{
    DIR *directory = opendir("/dev/input");
    struct dirent *entry;

    if (!directory)
        return false;

    while ((entry = readdir(directory)) != NULL && g_fd_count < MAX_INPUT_DEVICES) {
        char path[128];
        char name[128] = "";
        int fd;

        if (strncmp(entry->d_name, "event", 5) != 0)
            continue;

        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
        if (strstr(name, "Gamepad") || strstr(name, "joypad") || strstr(name, "Pad"))
            g_have_pad = true;
        if (g_debug)
            fprintf(stderr, "vita2native: SceCtrl opened %s (%s)\n", path, name);
        g_fds[g_fd_count++] = fd;
    }

    closedir(directory);
    return g_fd_count > 0;
}

EXPORT(SceCtrl, 0xA9C3CED6, int, sceCtrlPeekBufferPositive, int port, SceCtrlData *pad_data,
       int count)
{
    HostPadState sdl_pad;
    bool have_sdl_pad;
    unsigned int script_buttons;

    (void)port;
    (void)count;

    if (!pad_data)
        return -1;

    script_buttons = SceCtrl_script_buttons();

    /* Sampled by the SDL video thread (keyboard and game controllers). Call it
       before taking g_lock so the two locks are never taken in both orders. */
    have_sdl_pad = host_video_pad_state(&sdl_pad);

    pthread_mutex_lock(&g_lock);
    pad_data->buttons = g_pad_buttons | g_pending_presses | script_buttons;
    g_pending_presses = 0;
    pad_data->lx = g_lx;
    pad_data->ly = g_ly;
    pad_data->rx = g_rx;
    pad_data->ry = g_ry;

    if (have_sdl_pad) {
        pad_data->buttons |= sdl_pad.buttons | sdl_pad.pressed;
        /* Prefer SDL axes only when a controller is actually attached; a
           keyboard-only session must not overwrite the evdev stick values. */
        if (sdl_pad.active) {
            pad_data->lx = sdl_pad.lx;
            pad_data->ly = sdl_pad.ly;
            pad_data->rx = sdl_pad.rx;
            pad_data->ry = sdl_pad.ry;
        }
    }
    pthread_mutex_unlock(&g_lock);

    if (g_debug) {
        /* Rate-limited so a 60 Hz guest does not flood the log: always report a
           state change, otherwise a heartbeat every ~600 polls. */
        g_peek_calls++;
        if (pad_data->buttons != g_peek_logged_buttons ||
            (g_peek_calls % 600) == 0) {
            fprintf(stderr, "vita2native: SceCtrl peek #%llu buttons=0x%x\n",
                    g_peek_calls, pad_data->buttons);
            g_peek_logged_buttons = pad_data->buttons;
        }
    }

    pad_data->timeStamp = host_monotonic_ns() / 1000;

    return 0;
}

DECLARE_LIBRARY(SceCtrl, 0xD197E3C7);

int SceCtrl_init(void)
{
    g_debug = getenv("VITA2NATIVE_INPUT_DEBUG") != NULL;
    g_swap_ab = getenv("VITA2NATIVE_CTRL_SWAP_AB") != NULL;

    if (!input_open_devices()) {
        if (g_debug)
            fprintf(stderr, "vita2native: SceCtrl found no evdev devices under /dev/input\n");
        return 0;
    }

    if (!g_have_pad && g_debug)
        fprintf(stderr, "vita2native: SceCtrl found no gamepad by name, using all %d event \
"
                        "devices\n",
                g_fd_count);

    if (pthread_create(&g_thread, NULL, input_thread, NULL) == 0)
        g_thread_started = true;
    else
        fprintf(stderr, "vita2native: SceCtrl cannot start the input thread\n");

    return 0;
}
