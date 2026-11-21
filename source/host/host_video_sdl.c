/*
 * Video output through SDL.
 *
 * SDL owns platform details on desktops: X11, Wayland or the dummy driver. On
 * KMS/DRM targets (handhelds) presentation attaches EGL straight to the
 * scanout plane and SDL only supplies input. Keeping one implementation means
 * there is no custom DRM/fbdev code to chase across vendor kernels.
 *
 * When no video driver can be initialised the emulator stays without output
 * (BMP capture still works).
 *
 * The guest's GLES context and SDL's GL renderer cannot share a thread: SDL
 * makes its own EGL context current, and on Mesa that leaks GL errors into the
 * guest's context (EGL tracks the current context per display, so neither
 * eglGetCurrentContext nor a cached binding can detect it). The window,
 * renderer and swapchain therefore live on a dedicated video thread; the guest
 * only copies finished frames into a staging buffer.
 *
 * The guest framebuffer is A8B8G8R8, which is SDL_PIXELFORMAT_ABGR8888 on a
 * little-endian host, so frames are uploaded without a conversion. SDL also
 * does the scaling and letterboxing.
 */
#include "gfx.h"
#include "host.h"
#include "settings.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_syswm.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <psp2/ctrl.h>

/* Vita front touch panel. */
#define TOUCH_PANEL_WIDTH 1920
#define TOUCH_PANEL_HEIGHT 1088
#define SDL_BUTTON_LMASK 0x01u

/* Built-in pad plus whatever is plugged in: a handheld can have both at once. */
#define MAX_SDL_PADS 4

struct HostVideo {

    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool thread_started;
    bool init_done;
    bool init_ok;
    bool stop;

    SDL_Window *window;
    /* Guest frame size, for mapping the pointer through the letterbox. */
    int guest_width;
    int guest_height;
    int width;
    int height;
    int window_width;
    int window_height;
    bool stretch;
    bool windowed;

    /*
     * Every connected pad, not just the first: a handheld can have its built-in
     * pad and a USB/Bluetooth/virtual one attached at the same time, and both
     * should drive the guest.
     */
    struct {
        SDL_GameController *controller;
        SDL_Joystick *joystick;
        int device_index;
    } pads[MAX_SDL_PADS];
    int pad_count;
    bool controller_ready;
    uint64_t controller_scan_at_ns;
    bool swap_ab;
    HostPadState pad;
    HostTouchState touch;
    bool touch_latched;
    bool rumble_pending;
    uint16_t rumble_low;
    uint16_t rumble_high;
    bool window_closed;

    /* Set once EGL has a window surface for it (see video_ensure_egl). */
    bool egl_attached;

    /* The window SDL created, presented to through EGL. */
    bool gl_window;
    void *native_window;
    /* Re-read on the video thread: SDL can replace the X window after first
       reporting it, and attaching to the old id fails with BadWindow. */
    bool native_window_ok;
    uint64_t window_probe_at_ns;

    /* Debug overlay drawn by the direct-present GPU pass. */
    const void *overlay_pixels;
    int overlay_width;
    int overlay_height;
};

static HostVideo *g_active_video;

static uint64_t video_now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void video_close_devices(HostVideo *video)
{
    for (int i = 0; i < video->pad_count; i++) {
        if (video->pads[i].joystick && SDL_JoystickClose)
            SDL_JoystickClose(video->pads[i].joystick);
        if (video->pads[i].controller && SDL_GameControllerClose)
            SDL_GameControllerClose(video->pads[i].controller);
        memset(&video->pads[i], 0, sizeof(video->pads[i]));
    }
    video->pad_count = 0;
}

static void video_destroy_sdl(HostVideo *video)
{
    video_close_devices(video);
    if (video->window)
        SDL_DestroyWindow(video->window);
    video->window = NULL;
}

/*
 * Brings SDL's video subsystem up.
 *
 * SDL picks the driver. It probes X11, Wayland, KMSDRM, offscreen and dummy in
 * its own order and honours an explicit SDL_VIDEODRIVER from the caller.
 * Overriding it here only ever fought SDL.
 */
static bool video_init_video_subsystem(HostVideo *video)
{
    (void)video;

    return SDL_InitSubSystem(SDL_INIT_VIDEO) == 0;
}

/*
 * Re-reads the window's native handle. Runs on the video thread, where SDL's
 * video calls belong.
 */
static void video_capture_window_id(HostVideo *video)
{
    SDL_SysWMinfo info;

    video->native_window = NULL;
    video->native_window_ok = false;
    if (!video->window)
        return;

    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(video->window, &info))
        return;

    if (info.subsystem == SDL_SYSWM_X11)
        video->native_window = (void *)(uintptr_t)info.info.x11.window;
    else if (info.subsystem == SDL_SYSWM_WAYLAND)
        /*
         * The window is SDL's wl_surface. The wl_egl_window an EGL surface
         * needs is an EGL-side object, so the GL layer creates it; SDL only
         * makes one itself when asked for an SDL_GLContext, which is not how
         * this port works.
         */
        video->native_window = info.info.wl.surface;
    else
        return;

    video->native_window_ok = video->native_window != NULL;
    if (video->native_window_ok &&
        vita2native_env_flag("VITA2NATIVE_DIRECT_PRESENT_DEBUG"))
        fprintf(stderr, "vita2native: window handle 0x%lx\n",
                (unsigned long)(uintptr_t)video->native_window);
}

/*
 * Reports the window handle EGL can attach to.
 *
 * The device does not exist yet when the window is created - the GL layer is
 * built on this handle, not the other way round - so this only records what SDL
 * gave us. Whether EGL can actually present to it is decided later, when
 * host_video_present_texture() runs with a device.
 */
static bool video_try_gl_window(HostVideo *video)
{
    const char *driver;

    if (!video->window)
        return false;

    driver = SDL_GetCurrentVideoDriver ? SDL_GetCurrentVideoDriver() : "";
    if (!driver || (strcmp(driver, "x11") != 0 && strcmp(driver, "wayland") != 0 &&
                    strcmp(driver, "kmsdrm") != 0))
        return false;

    video_capture_window_id(video);
    if (!video->native_window_ok)
        return false;

    video->gl_window = true;
    fprintf(stderr, "vita2native: window handed to EGL (%s video)\n", driver);
    return true;
}

static bool video_init_sdl(HostVideo *video)
{
    unsigned int window_flags = SDL_WINDOW_SHOWN;
    int panel_width = 0, panel_height = 0;

    if (!video_init_video_subsystem(video)) {
        fprintf(stderr, "vita2native: SDL video init failed: %s\n", SDL_GetError());
        return false;
    }

    /* Controllers are optional: keyboard input and video work without them. */
    if (SDL_GameControllerOpen &&
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
        video->controller_ready = true;
    } else {
        fprintf(stderr, "vita2native: SDL controller init failed: %s\n", SDL_GetError());
    }

    if (!video->windowed)
        window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    video->window = SDL_CreateWindow("vita2native", SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, video->window_width,
                                       video->window_height, window_flags);
    if (!video->window) {
        fprintf(stderr, "vita2native: SDL window creation failed: %s\n", SDL_GetError());
        return false;
    }

    /*
     * EGL is the only output path: it draws straight into the window, so both
     * the guest's render target and any CPU frame go through it. No
     * SDL_Renderer is created - a second GL context behind our back is exactly
     * the interference this design avoids.
     */
    if (!video_try_gl_window(video)) {
        fprintf(stderr, "vita2native: no EGL window for this video driver (%s)\n",
                SDL_GetCurrentVideoDriver ? SDL_GetCurrentVideoDriver() : "none");
        return false;
    }

    SDL_ShowCursor(0);
    SDL_GetWindowSize(video->window, &panel_width, &panel_height);
    video->width = panel_width > 0 ? panel_width : 960;
    video->height = panel_height > 0 ? panel_height : 544;

    fprintf(stderr, "vita2native: presenting through EGL (%s video, %dx%d window)\n",
            SDL_GetCurrentVideoDriver ? SDL_GetCurrentVideoDriver() : "unknown", video->width,
            video->height);

    video->controller_scan_at_ns = video_now_ns();
    return true;
}

/* ------------------------------------------------------------------------- */
/* SDL input                                                                  */
/* ------------------------------------------------------------------------- */

static bool video_pad_open(const HostVideo *video, int device_index)
{
    for (int i = 0; i < video->pad_count; i++)
        if (video->pads[i].device_index == device_index)
            return true;
    return false;
}

static void video_close_pad(HostVideo *video, int slot)
{

    if (video->pads[slot].joystick && SDL_JoystickClose)
        SDL_JoystickClose(video->pads[slot].joystick);
    if (video->pads[slot].controller && SDL_GameControllerClose)
        SDL_GameControllerClose(video->pads[slot].controller);

    for (int i = slot; i + 1 < video->pad_count; i++)
        video->pads[i] = video->pads[i + 1];
    video->pad_count--;
    memset(&video->pads[video->pad_count], 0, sizeof(video->pads[0]));
}

/*
 * Opens every pad SDL knows about and drops the ones that went away. Pads SDL
 * has no mapping for are read as raw joysticks, positionally. Runs about once a
 * second: enumeration is not free and hot-plug does not need to be faster.
 */
static void video_scan_pads(HostVideo *video)
{
    int count;

    if (!video->controller_ready || !SDL_NumJoysticks)
        return;

    for (int i = video->pad_count - 1; i >= 0; i--) {
        if (video->pads[i].controller && SDL_GameControllerGetAttached &&
            !SDL_GameControllerGetAttached(video->pads[i].controller)) {
            fprintf(stderr, "vita2native: SDL controller %d disconnected\n",
                    video->pads[i].device_index);
            video_close_pad(video, i);
        } else if (video->pads[i].joystick && SDL_JoystickGetAttached &&
                   !SDL_JoystickGetAttached(video->pads[i].joystick)) {
            fprintf(stderr, "vita2native: SDL joystick %d disconnected\n",
                    video->pads[i].device_index);
            video_close_pad(video, i);
        }
    }

    count = SDL_NumJoysticks();
    for (int i = 0; i < count && video->pad_count < MAX_SDL_PADS; i++) {
        SDL_GameController *controller = NULL;
        SDL_Joystick *joystick = NULL;
        const char *name = NULL;

        if (video_pad_open(video, i))
            continue;

        if (SDL_GameControllerOpen && (!SDL_IsGameController || SDL_IsGameController(i)))
            controller = SDL_GameControllerOpen(i);
        if (controller) {
            name = SDL_GameControllerName ? SDL_GameControllerName(controller) : NULL;
            fprintf(stderr, "vita2native: SDL opened controller %d%s%s\n", i, name ? ": " : "",
                    name ? name : "");
            if (vita2native_env_flag("VITA2NATIVE_INPUT_DEBUG") &&
                SDL_GameControllerMapping) {
                char *mapping = SDL_GameControllerMapping(controller);

                if (mapping) {
                    fprintf(stderr, "vita2native: SDL mapping %s\n", mapping);
                    if (SDL_free)
                        SDL_free(mapping);
                }
            }
        } else if (SDL_JoystickOpen) {
            joystick = SDL_JoystickOpen(i);
            if (joystick) {
                name = SDL_JoystickName ? SDL_JoystickName(joystick) : NULL;
                fprintf(stderr, "vita2native: SDL raw joystick %d%s%s (%d buttons, %d axes)\n", i,
                        name ? ": " : "", name ? name : "",
                        SDL_JoystickNumButtons ? SDL_JoystickNumButtons(joystick) : 0,
                        SDL_JoystickNumAxes ? SDL_JoystickNumAxes(joystick) : 0);
            }
        }

        if (!controller && !joystick)
            continue;

        video->pads[video->pad_count].controller = controller;
        video->pads[video->pad_count].joystick = joystick;
        video->pads[video->pad_count].device_index = i;
        video->pad_count++;
    }
}

static unsigned int raw_button_mask(const HostVideo *video, int button)
{
    switch (button) {
    case 0: /* BTN_SOUTH */
        return video->swap_ab ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
    case 1: /* BTN_EAST */
        return video->swap_ab ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
    case 2: /* BTN_NORTH */
        return SCE_CTRL_TRIANGLE;
    case 3: /* BTN_WEST */
        return SCE_CTRL_SQUARE;
    case 4: /* BTN_TL */
        return SCE_CTRL_L1;
    case 5: /* BTN_TR */
        return SCE_CTRL_R1;
    case 6: /* BTN_TL2 */
        return SCE_CTRL_L2;
    case 7: /* BTN_TR2 */
        return SCE_CTRL_R2;
    case 8: /* BTN_SELECT */
        return SCE_CTRL_SELECT;
    case 9: /* BTN_START */
        return SCE_CTRL_START;
    case 10: /* BTN_DPAD_UP */
        return SCE_CTRL_UP;
    case 11: /* BTN_DPAD_DOWN */
        return SCE_CTRL_DOWN;
    case 12: /* BTN_DPAD_LEFT */
        return SCE_CTRL_LEFT;
    case 13: /* BTN_DPAD_RIGHT */
        return SCE_CTRL_RIGHT;
    default:
        return 0;
    }
}

static unsigned int controller_button_mask(const HostVideo *video, int button)
{
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:
        return video->swap_ab ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
    case SDL_CONTROLLER_BUTTON_B:
        return video->swap_ab ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
    case SDL_CONTROLLER_BUTTON_X:
        return SCE_CTRL_SQUARE;
    case SDL_CONTROLLER_BUTTON_Y:
        return SCE_CTRL_TRIANGLE;
    case SDL_CONTROLLER_BUTTON_BACK:
        return SCE_CTRL_SELECT;
    case SDL_CONTROLLER_BUTTON_START:
        return SCE_CTRL_START;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:
        return SCE_CTRL_L3;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        return SCE_CTRL_R3;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        return SCE_CTRL_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        return SCE_CTRL_R1;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        return SCE_CTRL_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        return SCE_CTRL_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        return SCE_CTRL_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        return SCE_CTRL_RIGHT;
    default:
        return 0;
    }
}

static unsigned int keyboard_button_mask(const unsigned char *keys, int numkeys)
{
    unsigned int mask = 0;

#define KEY(scancode, button)                                                                      \
    do {                                                                                           \
        if ((scancode) < numkeys && keys[(scancode)])                                              \
            mask |= (button);                                                                      \
    } while (0)

    KEY(SDL_SCANCODE_UP, SCE_CTRL_UP);
    KEY(SDL_SCANCODE_DOWN, SCE_CTRL_DOWN);
    KEY(SDL_SCANCODE_LEFT, SCE_CTRL_LEFT);
    KEY(SDL_SCANCODE_RIGHT, SCE_CTRL_RIGHT);
    KEY(SDL_SCANCODE_Z, SCE_CTRL_CROSS);
    KEY(SDL_SCANCODE_RETURN, SCE_CTRL_CROSS);
    KEY(SDL_SCANCODE_X, SCE_CTRL_CIRCLE);
    KEY(SDL_SCANCODE_BACKSPACE, SCE_CTRL_CIRCLE);
    KEY(SDL_SCANCODE_A, SCE_CTRL_SQUARE);
    KEY(SDL_SCANCODE_S, SCE_CTRL_TRIANGLE);
    KEY(SDL_SCANCODE_Q, SCE_CTRL_L1);
    KEY(SDL_SCANCODE_E, SCE_CTRL_R1);
    KEY(SDL_SCANCODE_1, SCE_CTRL_L2);
    KEY(SDL_SCANCODE_2, SCE_CTRL_R2);
    KEY(SDL_SCANCODE_TAB, SCE_CTRL_SELECT);
    KEY(SDL_SCANCODE_SPACE, SCE_CTRL_START);

#undef KEY
    return mask;
}

static uint8_t sdl_axis_to_vita(int value)
{
    int converted;

    /* SDL sticks are -32768..32767 with negative = up/left; Vita axes are
       0..255 with 0 = up/left and 128 = center. */
    converted = (int)(((long long)(value + 32768) * 255 + 32767) / 65535);
    if (converted < 0)
        converted = 0;
    if (converted > 255)
        converted = 255;
    return (uint8_t)converted;
}

/* Runs on the video thread: SDL event/video calls must stay on the thread that
   initialised the subsystem. */
static void video_poll_events(HostVideo *video)
{
    SDL_Event event;

    if (SDL_PollEvent) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                if (vita2native_env_flag("VITA2NATIVE_INPUT_DEBUG")) {
                    fprintf(stderr, "vita2native: SDL quit event\n");
                    fflush(stderr);
                }
                video->window_closed = true;
            } else if (event.type == SDL_WINDOWEVENT) {
                const SDL_WindowEvent *window_event = (const SDL_WindowEvent *)&event;

                if (vita2native_env_flag("VITA2NATIVE_INPUT_DEBUG")) {
                    fprintf(stderr, "vita2native: SDL window event %u (%dx%d)\n",
                            window_event->event, window_event->data1, window_event->data2);
                    fflush(stderr);
                }
                if (window_event->event == SDL_WINDOWEVENT_CLOSE) {
                    video->window_closed = true;
                } else if (window_event->event == SDL_WINDOWEVENT_SIZE_CHANGED &&
                           window_event->data1 > 0 && window_event->data2 > 0) {
                    pthread_mutex_lock(&video->lock);
                    video->width = window_event->data1;
                    video->height = window_event->data2;
                    pthread_mutex_unlock(&video->lock);
                }
            }
        }
    } else if (SDL_PumpEvents) {
        SDL_PumpEvents();
    }
}

static void video_sample_touch(HostVideo *video)
{
    int mouse_x = 0, mouse_y = 0;
    unsigned int buttons;
    bool down;
    bool mapped = false;
    int panel_x = 0, panel_y = 0;


    buttons = SDL_GetMouseState(&mouse_x, &mouse_y);
    down = (buttons & SDL_BUTTON_LMASK) != 0;

    if (vita2native_env_flag("VITA2NATIVE_INPUT_DEBUG")) {
        static int last_x = -1, last_y = -1;
        static unsigned int last_buttons;

        if (mouse_x != last_x || mouse_y != last_y || buttons != last_buttons) {
            fprintf(stderr, "vita2native: mouse %d,%d buttons=0x%x\n", mouse_x, mouse_y,
                    buttons);
            fflush(stderr);
            last_x = mouse_x;
            last_y = mouse_y;
            last_buttons = buttons;
        }
    }

    if (video->width > 0 && video->height > 0) {
        int content_x = 0, content_y = 0;
        int content_w = video->width, content_h = video->height;

        /* Mirror the presenter's letterbox (host_gfx_gl_present_texture), so the
           black bars are not touchable and the visible frame maps onto the full
           panel. The guest frame is the Vita's 960x544. */
        if (!video->stretch && video->guest_width > 0 && video->guest_height > 0) {
            int scale_x = video->width * 1000 / video->guest_width;
            int scale_y = video->height * 1000 / video->guest_height;
            int scale = scale_x < scale_y ? scale_x : scale_y;

            content_w = video->guest_width * scale / 1000;
            content_h = video->guest_height * scale / 1000;
            content_x = (video->width - content_w) / 2;
            content_y = (video->height - content_h) / 2;
        }

        if (content_w > 0 && content_h > 0 && mouse_x >= content_x &&
            mouse_y >= content_y && mouse_x < content_x + content_w &&
            mouse_y < content_y + content_h) {
            panel_x = (mouse_x - content_x) * TOUCH_PANEL_WIDTH / content_w;
            panel_y = (mouse_y - content_y) * TOUCH_PANEL_HEIGHT / content_h;
            if (panel_x < 0)
                panel_x = 0;
            if (panel_x >= TOUCH_PANEL_WIDTH)
                panel_x = TOUCH_PANEL_WIDTH - 1;
            if (panel_y < 0)
                panel_y = 0;
            if (panel_y >= TOUCH_PANEL_HEIGHT)
                panel_y = TOUCH_PANEL_HEIGHT - 1;
            mapped = true;
        }
    }

    pthread_mutex_lock(&video->lock);
    video->touch.active = true;
    if (mapped && down) {
        /* Keep the press coordinates for the latch: a tap may be released
           before the guest polls, and it should still report where it was. */
        video->touch.x = (uint16_t)panel_x;
        video->touch.y = (uint16_t)panel_y;
        if (!video->touch.down)
            video->touch_latched = true;
        video->touch.down = true;
    } else {
        video->touch.down = false;
    }
    pthread_mutex_unlock(&video->lock);
}

static void video_sample_input(HostVideo *video)
{
    unsigned int buttons = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    bool active = false;
    bool rumble = false;
    uint16_t rumble_low = 0, rumble_high = 0;

    video_poll_events(video);
    video_sample_touch(video);

    if (SDL_GetKeyboardState) {
        int numkeys = 0;
        const unsigned char *keys = SDL_GetKeyboardState(&numkeys);

        if (keys && numkeys > 0) {
            buttons |= keyboard_button_mask(keys, numkeys);

            /* A keyboard has no analog stick, but several titles only aim with
               one (Angry Birds' slingshot menu). Drive the left stick from the
               arrow keys when no physical pad is attached; d-pad titles still
               get the button mask above. */
            if (video->pad_count == 0) {
                int x = (keys[SDL_SCANCODE_RIGHT] ? 1 : 0) - (keys[SDL_SCANCODE_LEFT] ? 1 : 0);
                int y = (keys[SDL_SCANCODE_UP] ? 1 : 0) - (keys[SDL_SCANCODE_DOWN] ? 1 : 0);

                /* Make the keyboard the axis source: without a pad the SDL
                   axes are only honoured when the sample is marked active. */
                active = true;
                if (x)
                    lx = x > 0 ? 255 : 0;
                if (y)
                    ly = y > 0 ? 0 : 255;
            }
        }
    }

    {
        uint64_t now = video_now_ns();

        if (now >= video->controller_scan_at_ns) {
            video_scan_pads(video);
            /* Hotplug detection costs a joystick enumeration, so look once a
               second rather than on every 16 ms poll. */
            video->controller_scan_at_ns = now + 1000000000ull;
        }
    }

    /* Buttons are the union of every pad. The axes come from whichever pad is
       being used: the built-in pad sits centred while an attached one plays. */
    {
        int best_score = -1;

        for (int p = 0; p < video->pad_count; p++) {
            uint8_t plx = 128, ply = 128, prx = 128, pry = 128;
            unsigned int pad_buttons = 0;
            int score;

            if (video->pads[p].controller && SDL_GameControllerGetButton) {
                for (int button = 0; button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT; button++)
                    if (SDL_GameControllerGetButton(video->pads[p].controller, button))
                        pad_buttons |= controller_button_mask(video, button);
                if (SDL_GameControllerGetAxis) {
                    if (SDL_GameControllerGetAxis(video->pads[p].controller,
                                                      SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000)
                        pad_buttons |= SCE_CTRL_L2;
                    if (SDL_GameControllerGetAxis(video->pads[p].controller,
                                                      SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000)
                        pad_buttons |= SCE_CTRL_R2;

                    plx = sdl_axis_to_vita(SDL_GameControllerGetAxis(
                        video->pads[p].controller, SDL_CONTROLLER_AXIS_LEFTX));
                    ply = sdl_axis_to_vita(SDL_GameControllerGetAxis(
                        video->pads[p].controller, SDL_CONTROLLER_AXIS_LEFTY));
                    prx = sdl_axis_to_vita(SDL_GameControllerGetAxis(
                        video->pads[p].controller, SDL_CONTROLLER_AXIS_RIGHTX));
                    pry = sdl_axis_to_vita(SDL_GameControllerGetAxis(
                        video->pads[p].controller, SDL_CONTROLLER_AXIS_RIGHTY));
                }
            } else if (video->pads[p].joystick && SDL_JoystickGetButton &&
                       SDL_JoystickGetAxis) {
                for (int button = 0; button <= 13; button++)
                    if (SDL_JoystickGetButton(video->pads[p].joystick, button))
                        pad_buttons |= raw_button_mask(video, button);
                /* Linux gamepad axes are X, Y, RX, RY in this order. Triggers
                   are buttons on generic pads, so they need no axis handling. */
                plx = sdl_axis_to_vita(SDL_JoystickGetAxis(video->pads[p].joystick, 0));
                ply = sdl_axis_to_vita(SDL_JoystickGetAxis(video->pads[p].joystick, 1));
                prx = sdl_axis_to_vita(SDL_JoystickGetAxis(video->pads[p].joystick, 2));
                pry = sdl_axis_to_vita(SDL_JoystickGetAxis(video->pads[p].joystick, 3));
            } else {
                continue;
            }

            active = true;
            buttons |= pad_buttons;

            score = pad_buttons ? 1000 : 0;
            score += (plx > 128 ? plx - 128 : 128 - plx) + (ply > 128 ? ply - 128 : 128 - ply) +
                     (prx > 128 ? prx - 128 : 128 - prx) + (pry > 128 ? pry - 128 : 128 - pry);
            if (score > best_score) {
                best_score = score;
                lx = plx;
                ly = ply;
                rx = prx;
                ry = pry;
            }
        }
    }

    if (vita2native_env_flag("VITA2NATIVE_INPUT_DEBUG")) {
        static unsigned int sampled;
        static unsigned int last_buttons;

        if (buttons != last_buttons || (sampled++ % 120) == 0) {
            fprintf(stderr, "vita2native: SDL sample buttons=0x%x lx=%u ly=%u active=%d pads=%d\n",
                    buttons, lx, ly, (int)active, video->pad_count);
            last_buttons = buttons;
        }
    }

    pthread_mutex_lock(&video->lock);
    /* Latch new presses until the guest peeks: titles poll the pad once per
       rendered frame, which can be slower than a human tap. */
    video->pad.pressed |= buttons & ~video->pad.buttons;
    video->pad.buttons = buttons;
    video->pad.lx = lx;
    video->pad.ly = ly;
    video->pad.rx = rx;
    video->pad.ry = ry;
    video->pad.active = active;
    if (video->pad_count > 0 && video->rumble_pending) {
        rumble = true;
        rumble_low = video->rumble_low;
        rumble_high = video->rumble_high;
        video->rumble_pending = false;
    }
    pthread_mutex_unlock(&video->lock);

    if (rumble && SDL_GameControllerRumble) {
        for (int p = 0; p < video->pad_count; p++)
            if (video->pads[p].controller)
                SDL_GameControllerRumble(video->pads[p].controller, rumble_low, rumble_high,
                                            0xFFFFFFFFu);
    }
}

void host_video_rumble(uint8_t small, uint8_t large)
{
    HostVideo *video = g_active_video;

    if (!video)
        return;

    pthread_mutex_lock(&video->lock);
    video->rumble_low = (uint16_t)large * 257u;
    video->rumble_high = (uint16_t)small * 257u;
    video->rumble_pending = true;
    pthread_mutex_unlock(&video->lock);
}

bool host_video_pad_state(HostPadState *state)
{
    HostVideo *video = g_active_video;

    if (!video || !state)
        return false;

    pthread_mutex_lock(&video->lock);
    *state = video->pad;
    video->pad.pressed = 0;
    pthread_mutex_unlock(&video->lock);
    return true;
}

bool host_video_touch_state(HostTouchState *state)
{
    HostVideo *video = g_active_video;

    if (!video || !state)
        return false;

    pthread_mutex_lock(&video->lock);
    *state = video->touch;
    state->down = video->touch.down || video->touch_latched;
    video->touch_latched = false;
    pthread_mutex_unlock(&video->lock);
    return true;
}

static void *video_thread(void *arg)
{
    HostVideo *video = arg;
    bool ok = video_init_sdl(video);

    pthread_mutex_lock(&video->lock);
    video->init_ok = ok;
    video->init_done = true;
    pthread_cond_broadcast(&video->cond);
    pthread_mutex_unlock(&video->lock);

    if (!ok) {
        video_destroy_sdl(video);
        if (video->controller_ready)
            SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return NULL;
    }

    for (;;) {
        struct timespec wait_until;

        /* Poll input even when no frame is waiting, so a desktop keyboard or
           controller stays responsive while the guest is busy. */
        video_sample_input(video);

        /*
         * An attach that has not succeeded yet needs a current window id: SDL
         * can replace the X window, and the presenting thread cannot ask SDL.
         * Once a second is enough.
         */
        if (video->gl_window && !video->native_window_ok) {
            uint64_t now = video_now_ns();

            if (now >= video->window_probe_at_ns) {
                video_capture_window_id(video);
                video->window_probe_at_ns = now + 1000000000ull;
            }
        }

        if (video->window_closed) {
            fprintf(stderr, "vita2native: window closed, exiting\n");
            fflush(stderr);
            _exit(0);
        }

        /*
         * This thread owns the window and the input devices, and nothing else:
         * frames are presented on the thread that renders them (see
         * host_video_present), so there is no mailbox to drain here.
         */
        clock_gettime(CLOCK_REALTIME, &wait_until);
        wait_until.tv_nsec += 16 * 1000000L;
        if (wait_until.tv_nsec >= 1000000000L) {
            wait_until.tv_sec++;
            wait_until.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&video->lock);
        if (!video->stop)
            pthread_cond_timedwait(&video->cond, &video->lock, &wait_until);
        if (video->stop) {
            pthread_mutex_unlock(&video->lock);
            break;
        }
        pthread_mutex_unlock(&video->lock);
    }

    video_destroy_sdl(video);
    if (video->controller_ready)
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return NULL;
}

HostVideo *host_video_open(void)
{
    HostVideo *video;

    /*
     * Letterbox scaling is SDL_RenderSetLogicalSize's job, and it reads this
     * hint rather than the texture's scale mode. Nearest keeps pixel art sharp;
     * the hint has to be set before any texture or renderer exists.
     */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");


    video = calloc(1, sizeof(*video));
    if (!video)
        goto fail;

    video->stretch = vita2native_env_flag("VITA2NATIVE_STRETCH");
    video->windowed = vita2native_env_flag("VITA2NATIVE_SDL_WINDOWED");
    video->swap_ab = vita2native_env_flag("VITA2NATIVE_CTRL_SWAP_AB");
    video->pad.lx = 128;
    video->pad.ly = 128;
    video->pad.rx = 128;
    video->pad.ry = 128;
    video->window_width = 960;
    video->window_height = 544;

    /* VITA2NATIVE_SDL_SIZE=480x272 makes the window smaller (X11 forwarding
       and remote testing); the guest frame is still scaled/letterboxed. */
    {
        const char *size = vita2native_env_str("VITA2NATIVE_SDL_SIZE");

        if (size && sscanf(size, "%dx%d", &video->window_width, &video->window_height) != 2) {
            video->window_width = 960;
            video->window_height = 544;
        }
        if (video->window_width < 64 || video->window_height < 64) {
            video->window_width = 960;
            video->window_height = 544;
        }
    }

    pthread_mutex_init(&video->lock, NULL);
    pthread_cond_init(&video->cond, NULL);

    if (pthread_create(&video->thread, NULL, video_thread, video) != 0) {
        fprintf(stderr, "vita2native: cannot start the video thread\n");
        pthread_mutex_destroy(&video->lock);
        pthread_cond_destroy(&video->cond);
        free(video);
        goto fail;
    }
    video->thread_started = true;

    pthread_mutex_lock(&video->lock);
    while (!video->init_done)
        pthread_cond_wait(&video->cond, &video->lock);
    pthread_mutex_unlock(&video->lock);

    if (!video->init_ok) {
        pthread_join(video->thread, NULL);
        video->thread_started = false;
        pthread_mutex_destroy(&video->lock);
        pthread_cond_destroy(&video->cond);
        free(video);
        goto fail;
    }

    g_active_video = video;
    return video;

fail:
    return NULL;
}

bool host_video_window_info(HostWindowInfo *info)
{
    HostVideo *video = g_active_video;
    SDL_SysWMinfo wm;

    if (!info)
        return false;

    memset(info, 0, sizeof(*info));
    if (!video || !video->window)
        return false;

    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(video->window, &wm))
        return false;

    switch (wm.subsystem) {
    case SDL_SYSWM_X11:
        info->type = HOST_WINDOW_X11;
        info->display = (void *)wm.info.x11.display;
        info->window = (void *)(uintptr_t)wm.info.x11.window;
        break;
    case SDL_SYSWM_WAYLAND:
        info->type = HOST_WINDOW_WAYLAND;
        info->display = wm.info.wl.display;
        /* The window is SDL's wl_surface; EGL builds its wl_egl_window on it. */
        info->window = wm.info.wl.surface;
        break;
#if SDL_VERSION_ATLEAST(2, 0, 12)
    case SDL_SYSWM_KMSDRM:
        info->type = HOST_WINDOW_KMSDRM;
        info->gbm_device = wm.info.kmsdrm.gbm_dev;
        info->drm_fd = wm.info.kmsdrm.drm_fd;
        /* The gbm_surface is not in SDL_SysWMinfo; the GL layer creates its own
           on this gbm_device and blits into it. */
        break;
#endif
    default:
        return false;
    }

    info->width = video->width > 0 ? video->width : video->window_width;
    info->height = video->height > 0 ? video->height : video->window_height;
    return info->type != HOST_WINDOW_NONE;
}

void host_video_close(HostVideo *video)
{
    if (!video)
        return;

    if (g_active_video == video)
        g_active_video = NULL;

    if (video->thread_started) {
        pthread_mutex_lock(&video->lock);
        video->stop = true;
        pthread_cond_broadcast(&video->cond);
        pthread_mutex_unlock(&video->lock);
        pthread_join(video->thread, NULL);
    }

    pthread_mutex_destroy(&video->lock);
    pthread_cond_destroy(&video->cond);
    free(video);
}

int host_video_width(const HostVideo *video)
{
    return video ? video->width : 0;
}

int host_video_height(const HostVideo *video)
{
    return video ? video->height : 0;
}

/*
 * Ensures the GL backend has an EGL window surface for SDL's window.
 *
 * Called on the presenting thread, which is where the GL context is current.
 * Returns the device, or NULL when there is no window to present to.
 */
static HostGpuDevice video_ensure_egl(HostVideo *video)
{
    HostGpuDevice device;
    HostWindowInfo info;

    if (!video || !video->gl_window)
        return NULL;

    device = host_gpu_current();
    if (!device)
        return NULL;

    if (video->egl_attached)
        return device;

    if (!host_video_window_info(&info)) {
        video->native_window_ok = false;
        return NULL;
    }

    if (host_gpu_attach_window(device, &info) != 0) {
        fprintf(stderr, "vita2native: EGL window attach failed\n");
        /* SDL may have replaced the window; the video thread re-reads it. */
        video->native_window_ok = false;
        return NULL;
    }

    video->egl_attached = true;
    return device;
}

void host_video_present(HostVideo *video, const void *rgba, int width, int height,
                        int pitch_pixels)
{
    HostGpuDevice device;

    if (!video || !rgba || width <= 0 || height <= 0 || pitch_pixels < width)
        return;

    /*
     * EGL is the only output path, so a CPU frame is presented right here
     * rather than handed to the video thread: the window surface belongs to the
     * thread whose GL context is current, and that is this one.
     */
    device = video_ensure_egl(video);
    if (device) {
        video->guest_width = width;
        video->guest_height = height;
        host_gpu_set_overlay(device, video->overlay_pixels, video->overlay_width,
                             video->overlay_height);
        host_gpu_present_framebuffer(device, rgba, width, height, pitch_pixels);
    }
}

bool host_video_present_texture(HostVideo *video, unsigned int texture, int width, int height)
{
    HostGpuDevice device;

    if (!video || !texture || width <= 0 || height <= 0)
        return false;

    device = video_ensure_egl(video);
    if (!device)
        return false;

    video->guest_width = width;
    video->guest_height = height;
    host_gpu_set_overlay(device, video->overlay_pixels, video->overlay_width,
                         video->overlay_height);
    host_gpu_present_texture(device, texture, width, height);
    return true;
}

__attribute__((weak)) bool host_gpu_can_present_window(HostGpuDevice device)
{
    (void)device;
    return false;
}

bool host_video_presents_texture(void)
{
    HostVideo *video = g_active_video;

    return video && video->gl_window;
}

void host_video_set_overlay(HostVideo *video, const void *rgba, int width, int height)
{
    if (!video)
        return;

    video->overlay_pixels = rgba;
    video->overlay_width = rgba ? width : 0;
    video->overlay_height = rgba ? height : 0;
}

/*
 * Standalone test binaries link this file without the GL backend. The real
 * implementations live in host_gfx.c in the emulator and override these.
 */
#if defined(__GNUC__)
__attribute__((weak)) HostGpuDevice host_gpu_current(void)
{
    return NULL;
}

__attribute__((weak)) int host_gpu_attach_window(HostGpuDevice device,
                                                 const HostWindowInfo *window)
{
    (void)device;
    (void)window;
    return -1;
}

__attribute__((weak)) void host_gpu_present_framebuffer(HostGpuDevice device, const void *rgba,
                                                        int width, int height, int stride)
{
    (void)device;
    (void)rgba;
    (void)width;
    (void)height;
    (void)stride;
}

__attribute__((weak)) void host_gpu_present_texture(HostGpuDevice device, unsigned int texture,
                                                    int width, int height)
{
    (void)device;
    (void)texture;
    (void)width;
    (void)height;
}

__attribute__((weak)) void host_gpu_set_overlay(HostGpuDevice device, const void *rgba, int width,
                                                int height)
{
    (void)device;
    (void)rgba;
    (void)width;
    (void)height;
}
#endif
