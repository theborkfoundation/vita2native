/*
 * Video output through SDL.
 *
 * SDL is the fallback presenter and owns platform details on desktops: X11,
 * Wayland or the dummy driver. On KMS/DRM targets (handhelds) presentation
 * attaches EGL straight to the scanout plane and SDL only supplies input.
 * Keeping one implementation means there is no custom DRM/fbdev code to chase
 * across vendor kernels.
 *
 * SDL is loaded with dlopen, so the statically linked ARM binary keeps no SDL
 * build dependency: boards with SDL use it, and when no driver can be
 * initialised the emulator simply stays headless (BMP capture still works).
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
 * does the scaling and letterboxing. Renderer handles are opaque and the few
 * constants below are part of SDL 2.x's stable ABI.
 */
#include "display.h"
#include "gfx.h"
#include "host.h"
#include "settings.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <psp2/ctrl.h>

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef struct _SDL_GameController SDL_GameController;
typedef struct _SDL_Joystick SDL_Joystick;

typedef struct SDL_Rect {
    int x, y, w, h;
} SDL_Rect;

typedef struct SDL_RendererInfo {
    const char *name;
    uint32_t flags;
    uint32_t num_texture_formats;
    uint32_t texture_formats[16];
    int max_texture_width;
    int max_texture_height;
} SDL_RendererInfo;

/* SDL_ScaleMode, from SDL_render.h. SDL is dlopen'd, so the value is local. */
#define SDL_ScaleModeNearest 0

typedef struct {
    uint32_t type;
    uint8_t padding[60];
} SdlEvent;

typedef struct {
    uint32_t type;
    uint32_t timestamp;
    uint32_t window_id;
    uint8_t event;
    uint8_t padding[3];
    int32_t data1;
    int32_t data2;
} SdlWindowEvent;

typedef struct {
    void *library;
    int (*init_subsystem)(unsigned int flags);
    void (*quit_subsystem)(unsigned int flags);
    const char *(*get_error)(void);
    SDL_Window *(*create_window)(const char *, int, int, int, int, unsigned int);
    void (*destroy_window)(SDL_Window *);
    SDL_Renderer *(*create_renderer)(SDL_Window *, int, unsigned int);
    void (*destroy_renderer)(SDL_Renderer *);
    SDL_Texture *(*create_texture)(SDL_Renderer *, unsigned int, int, int, int);
    void (*destroy_texture)(SDL_Texture *);
    int (*set_texture_scale_mode)(SDL_Texture *, int);
    int (*update_texture)(SDL_Texture *, const SDL_Rect *, const void *, int);
    int (*render_clear)(SDL_Renderer *);
    int (*render_copy)(SDL_Renderer *, SDL_Texture *, const SDL_Rect *, const SDL_Rect *);
    void (*render_present)(SDL_Renderer *);
    int (*render_set_logical_size)(SDL_Renderer *, int, int);
    int (*get_window_size)(SDL_Window *, int *, int *);
    int (*get_renderer_info)(SDL_Renderer *, SDL_RendererInfo *);
    const char *(*get_current_video_driver)(void);
    int (*show_cursor)(int);
    void (*pump_events)(void);
    const unsigned char *(*get_keyboard_state)(int *numkeys);
    int (*num_joysticks)(void);
    int (*is_game_controller)(int joystick_index);
    SDL_GameController *(*game_controller_open)(int joystick_index);
    void (*game_controller_close)(SDL_GameController *controller);
    int (*game_controller_get_button)(SDL_GameController *, int button);
    int (*game_controller_get_axis)(SDL_GameController *, int axis);
    const char *(*game_controller_name)(SDL_GameController *);
    int (*game_controller_get_attached)(SDL_GameController *);
    SDL_Joystick *(*joystick_open)(int joystick_index);
    void (*joystick_close)(SDL_Joystick *);
    int (*joystick_get_button)(SDL_Joystick *, int button);
    int (*joystick_get_axis)(SDL_Joystick *, int axis);
    int (*joystick_get_attached)(SDL_Joystick *);
    int (*joystick_num_buttons)(SDL_Joystick *);
    int (*joystick_num_axes)(SDL_Joystick *);
    const char *(*joystick_name)(SDL_Joystick *);
    char *(*game_controller_mapping)(SDL_GameController *);
    void (*free)(void *);
    unsigned int (*get_mouse_state)(int *, int *);
    int (*game_controller_rumble)(SDL_GameController *, unsigned short low,
                                  unsigned short high, unsigned int duration_ms);
    int (*poll_event)(void *event);
} SdlApi;

#define SDL_INIT_VIDEO 0x00000020u
#define SDL_WINDOW_SHOWN 0x00000004u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u
#define SDL_RENDERER_ACCELERATED 0x00000002u
#define SDL_RENDERER_PRESENTVSYNC 0x00000004u
#define SDL_PIXELFORMAT_ABGR8888 0x16762004u
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000u

#define SDL_INIT_GAMECONTROLLER 0x00002000u

/* SDL_GameControllerButton */
#define SDL_CONTROLLER_BUTTON_A 0
#define SDL_CONTROLLER_BUTTON_B 1
#define SDL_CONTROLLER_BUTTON_X 2
#define SDL_CONTROLLER_BUTTON_Y 3
#define SDL_CONTROLLER_BUTTON_BACK 4
#define SDL_CONTROLLER_BUTTON_START 6
#define SDL_CONTROLLER_BUTTON_LEFTSTICK 7
#define SDL_CONTROLLER_BUTTON_RIGHTSTICK 8
#define SDL_CONTROLLER_BUTTON_LEFTSHOULDER 9
#define SDL_CONTROLLER_BUTTON_RIGHTSHOULDER 10
#define SDL_CONTROLLER_BUTTON_DPAD_UP 11
#define SDL_CONTROLLER_BUTTON_DPAD_DOWN 12
#define SDL_CONTROLLER_BUTTON_DPAD_LEFT 13
#define SDL_CONTROLLER_BUTTON_DPAD_RIGHT 14

/* SDL_GameControllerAxis */
#define SDL_CONTROLLER_AXIS_LEFTX 0
#define SDL_CONTROLLER_AXIS_LEFTY 1
#define SDL_CONTROLLER_AXIS_RIGHTX 2
#define SDL_CONTROLLER_AXIS_RIGHTY 3
#define SDL_CONTROLLER_AXIS_TRIGGERLEFT 4
#define SDL_CONTROLLER_AXIS_TRIGGERRIGHT 5

/* SDL_Scancode (subset) */
#define SDL_SCANCODE_A 4
#define SDL_SCANCODE_E 8
#define SDL_SCANCODE_Q 20
#define SDL_SCANCODE_S 22
#define SDL_SCANCODE_X 27
#define SDL_SCANCODE_Z 29
#define SDL_SCANCODE_1 30
#define SDL_SCANCODE_2 31
#define SDL_SCANCODE_RETURN 40
#define SDL_SCANCODE_BACKSPACE 42
#define SDL_SCANCODE_TAB 43
#define SDL_SCANCODE_SPACE 44
#define SDL_SCANCODE_RIGHT 79
#define SDL_SCANCODE_LEFT 80
#define SDL_SCANCODE_DOWN 81
#define SDL_SCANCODE_UP 82

#define SDL_QUIT 0x100u
#define SDL_WINDOWEVENT 0x200u
#define SDL_WINDOWEVENT_SIZE_CHANGED 6
#define SDL_WINDOWEVENT_CLOSE 14

/* Vita front touch panel. */
#define TOUCH_PANEL_WIDTH 1920
#define TOUCH_PANEL_HEIGHT 1088
#define SDL_BUTTON_LMASK 0x01u

struct HostVideo {
    SdlApi *api;

    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool thread_started;
    bool init_done;
    bool init_ok;
    bool stop;

    /* Frame mailbox: the guest copies into `frame`, the video thread uploads
       it. Frames that arrive while the thread is still presenting are dropped
       (latest-wins is fine when the guest itself runs at a few fps). */
    uint8_t *frame;
    size_t frame_capacity;
    int frame_width;
    int frame_height;
    int frame_pitch;
    bool has_frame;
    bool busy;

    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int texture_width;
    int texture_height;
    int width;
    int height;
    int window_width;
    int window_height;
    bool stretch;
    bool windowed;

    SDL_GameController *controller;
    SDL_Joystick *joystick;
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

    /* Direct EGL presentation to the KMS/DRM scanout plane. */
    bool direct_present;
    bool egl_attached;

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

static void video_present(HostVideo *video, const void *rgba, int width, int height,
                          int pitch_pixels)
{
    SdlApi *api = video->api;

    if (!video->renderer)
        return;

    if (!video->texture || video->texture_width != width || video->texture_height != height) {
        if (video->texture)
            api->destroy_texture(video->texture);
        video->texture = api->create_texture(video->renderer, SDL_PIXELFORMAT_ABGR8888,
                                             SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!video->texture) {
            video->texture_width = 0;
            video->texture_height = 0;
            return;
        }
        video->texture_width = width;
        video->texture_height = height;
        /*
         * SDL_RenderCopy filters linearly unless told otherwise, which blurs
         * the guest's output: a 2D title drawn at 640x368 and presented onto a
         * 640x480 panel is upscaled vertically, and an odd render scale makes
         * the horizontal ratio fractional too. Nearest keeps pixel art sharp
         * for the titles that care and costs nothing for the ones that do not.
         */
        if (api->set_texture_scale_mode)
            api->set_texture_scale_mode(video->texture, SDL_ScaleModeNearest);
        if (!video->stretch)
            api->render_set_logical_size(video->renderer, width, height);
    }

    /* Guest pixels are A8B8G8R8, i.e. SDL_PIXELFORMAT_ABGR8888. */
    api->update_texture(video->texture, NULL, rgba, pitch_pixels * 4);
    api->render_clear(video->renderer);
    api->render_copy(video->renderer, video->texture, NULL, NULL);
    api->render_present(video->renderer);
}

static void video_close_devices(HostVideo *video)
{
    if (video->joystick) {
        if (video->api->joystick_close)
            video->api->joystick_close(video->joystick);
        video->joystick = NULL;
    }
    if (video->controller) {
        if (video->api->game_controller_close)
            video->api->game_controller_close(video->controller);
        video->controller = NULL;
    }
}

static void video_destroy_sdl(HostVideo *video)
{
    SdlApi *api = video->api;

    if (!api)
        return;

    video_close_devices(video);
    if (video->texture)
        api->destroy_texture(video->texture);
    if (video->renderer)
        api->destroy_renderer(video->renderer);
    if (video->window)
        api->destroy_window(video->window);
    video->texture = NULL;
    video->renderer = NULL;
    video->window = NULL;
}

static bool video_init_sdl(HostVideo *video)
{
    SdlApi *api = video->api;
    unsigned int window_flags = SDL_WINDOW_SHOWN;
    int panel_width = 0, panel_height = 0;

    if (api->init_subsystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "vita2native: SDL video init failed: %s\n", api->get_error());
        return false;
    }

    /* Controllers are optional: keyboard input and video work without them. */
    if (api->game_controller_open &&
        api->init_subsystem(SDL_INIT_GAMECONTROLLER) == 0) {
        video->controller_ready = true;
    } else {
        fprintf(stderr, "vita2native: SDL controller init failed: %s\n", api->get_error());
    }

    if (host_display_uses_kms()) {
        /* KMS presentation draws straight to the scanout buffers; SDL only
           provides evdev input, so no window (and no extra GBM/EGL state) is
           created here. */
        video->direct_present = true;
        video->width = 960;
        video->height = 544;
        fprintf(stderr, "vita2native: direct KMS presentation enabled (SDL %s input only)\n",
                api->get_current_video_driver ? api->get_current_video_driver() : "unknown");
        video->controller_scan_at_ns = video_now_ns();
        return true;
    }

    if (!video->windowed)
        window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    video->window = api->create_window("vita2native", SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, video->window_width,
                                       video->window_height, window_flags);
    if (!video->window) {
        fprintf(stderr, "vita2native: SDL window creation failed: %s\n", api->get_error());
        return false;
    }

    video->renderer = api->create_renderer(
        video->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!video->renderer)
        video->renderer = api->create_renderer(video->window, -1, 0);
    if (!video->renderer) {
        fprintf(stderr, "vita2native: SDL renderer creation failed: %s\n", api->get_error());
        return false;
    }

    api->show_cursor(0);
    api->get_window_size(video->window, &panel_width, &panel_height);
    video->width = panel_width > 0 ? panel_width : 960;
    video->height = panel_height > 0 ? panel_height : 544;

    if (video->renderer && api->get_renderer_info) {
        SDL_RendererInfo info;

        memset(&info, 0, sizeof(info));
        if (api->get_renderer_info(video->renderer, &info) == 0)
            fprintf(stderr, "vita2native: SDL renderer '%s' flags 0x%x\n",
                    info.name ? info.name : "?", info.flags);
    }

    fprintf(stderr, "vita2native: presenting through SDL (%s video, %dx%d window)\n",
            api->get_current_video_driver ? api->get_current_video_driver() : "unknown",
            video->width, video->height);

    video->controller_scan_at_ns = video_now_ns();
    return true;
}

/* ------------------------------------------------------------------------- */
/* SDL input                                                                  */
/* ------------------------------------------------------------------------- */

static SDL_GameController *video_open_controller(HostVideo *video)
{
    SdlApi *api = video->api;
    int count;

    if (!video->controller_ready || !api->num_joysticks || !api->game_controller_open)
        return NULL;

    count = api->num_joysticks();
    for (int i = 0; i < count; i++) {
        SDL_GameController *controller;
        const char *name;

        if (api->is_game_controller && !api->is_game_controller(i))
            continue;

        controller = api->game_controller_open(i);
        if (!controller)
            continue;

        name = api->game_controller_name ? api->game_controller_name(controller) : NULL;
        fprintf(stderr, "vita2native: SDL opened controller %d%s%s\n", i, name ? ": " : "",
                name ? name : "");
        if (vita2native_env_flag("VITA2NATIVE_INPUT_DEBUG") && api->game_controller_mapping) {
            char *mapping = api->game_controller_mapping(controller);

            if (mapping) {
                fprintf(stderr, "vita2native: SDL mapping %s\n", mapping);
                if (api->free)
                    api->free(mapping);
            }
        }
        return controller;
    }

    return NULL;
}

/* Fallback for pads SDL has no game controller mapping for: read raw joystick
   buttons and axes positionally. Generic vendor pads (including the R36S pad)
   show up this way. */
static SDL_Joystick *video_open_joystick(HostVideo *video)
{
    SdlApi *api = video->api;
    int count;

    if (!video->controller_ready || !api->num_joysticks || !api->joystick_open)
        return NULL;

    count = api->num_joysticks();
    for (int i = 0; i < count; i++) {
        SDL_Joystick *joystick = api->joystick_open(i);
        const char *name;
        int buttons, axes;

        if (!joystick)
            continue;

        name = api->joystick_name ? api->joystick_name(joystick) : NULL;
        buttons = api->joystick_num_buttons ? api->joystick_num_buttons(joystick) : 0;
        axes = api->joystick_num_axes ? api->joystick_num_axes(joystick) : 0;
        fprintf(stderr, "vita2native: SDL raw joystick %d%s%s (%d buttons, %d axes)\n", i,
                name ? ": " : "", name ? name : "", buttons, axes);
        return joystick;
    }

    return NULL;
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
    SdlApi *api = video->api;
    SdlEvent event;

    if (api->poll_event) {
        while (api->poll_event(&event)) {
            if (event.type == SDL_QUIT) {
                if (vita2native_env_flag("VITA2NATIVE_INPUT_DEBUG")) {
                    fprintf(stderr, "vita2native: SDL quit event\n");
                    fflush(stderr);
                }
                video->window_closed = true;
            } else if (event.type == SDL_WINDOWEVENT) {
                const SdlWindowEvent *window_event = (const SdlWindowEvent *)&event;

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
    } else if (api->pump_events) {
        api->pump_events();
    }
}

static void video_sample_touch(HostVideo *video)
{
    SdlApi *api = video->api;
    int mouse_x = 0, mouse_y = 0;
    unsigned int buttons;
    bool down;
    bool mapped = false;
    int panel_x = 0, panel_y = 0;

    if (!api || !api->get_mouse_state)
        return;

    buttons = api->get_mouse_state(&mouse_x, &mouse_y);
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

        /* Mirror SDL_RenderSetLogicalSize's letterbox, so the black bars are
           not touchable and the visible frame maps onto the full panel. */
        if (!video->stretch && video->texture_width > 0 && video->texture_height > 0) {
            int scale_x = video->width * 1000 / video->texture_width;
            int scale_y = video->height * 1000 / video->texture_height;
            int scale = scale_x < scale_y ? scale_x : scale_y;

            content_w = video->texture_width * scale / 1000;
            content_h = video->texture_height * scale / 1000;
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
    SdlApi *api = video->api;
    unsigned int buttons = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    bool active = false;
    bool rumble = false;
    uint16_t rumble_low = 0, rumble_high = 0;

    if (!api)
        return;

    video_poll_events(video);
    video_sample_touch(video);

    if (api->get_keyboard_state) {
        int numkeys = 0;
        const unsigned char *keys = api->get_keyboard_state(&numkeys);

        if (keys && numkeys > 0) {
            buttons |= keyboard_button_mask(keys, numkeys);

            /* A keyboard has no analog stick, but several titles only aim with
               one (Angry Birds' slingshot menu). Drive the left stick from the
               arrow keys when no physical pad is attached; d-pad titles still
               get the button mask above. */
            if (!video->controller && !video->joystick) {
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

    if (video->controller && api->game_controller_get_attached &&
        !api->game_controller_get_attached(video->controller)) {
        fprintf(stderr, "vita2native: SDL controller disconnected\n");
        video_close_devices(video);
    }
    if (video->joystick && api->joystick_get_attached &&
        !api->joystick_get_attached(video->joystick)) {
        fprintf(stderr, "vita2native: SDL joystick disconnected\n");
        video_close_devices(video);
    }

    if (!video->controller && !video->joystick) {
        uint64_t now = video_now_ns();

        if (now >= video->controller_scan_at_ns) {
            video->controller = video_open_controller(video);
            if (!video->controller)
                video->joystick = video_open_joystick(video);
            /* Hotplug detection costs a joystick enumeration, so look once a
               second rather than on every 16 ms poll. */
            video->controller_scan_at_ns = now + 1000000000ull;
        }
    }

    if (video->controller) {
        active = true;
        for (int button = 0; button <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT; button++) {
            if (api->game_controller_get_button(video->controller, button))
                buttons |= controller_button_mask(video, button);
        }
        if (api->game_controller_get_axis(video->controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >
            8000)
            buttons |= SCE_CTRL_L2;
        if (api->game_controller_get_axis(video->controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >
            8000)
            buttons |= SCE_CTRL_R2;

        lx = sdl_axis_to_vita(
            api->game_controller_get_axis(video->controller, SDL_CONTROLLER_AXIS_LEFTX));
        ly = sdl_axis_to_vita(
            api->game_controller_get_axis(video->controller, SDL_CONTROLLER_AXIS_LEFTY));
        rx = sdl_axis_to_vita(
            api->game_controller_get_axis(video->controller, SDL_CONTROLLER_AXIS_RIGHTX));
        ry = sdl_axis_to_vita(
            api->game_controller_get_axis(video->controller, SDL_CONTROLLER_AXIS_RIGHTY));
    } else if (video->joystick && api->joystick_get_button && api->joystick_get_axis) {
        active = true;
        for (int button = 0; button <= 13; button++) {
            if (api->joystick_get_button(video->joystick, button))
                buttons |= raw_button_mask(video, button);
        }
        /* Linux gamepad axes are X, Y, RX, RY in this order. Triggers are
           buttons on generic pads, so they need no axis handling here. */
        lx = sdl_axis_to_vita(api->joystick_get_axis(video->joystick, 0));
        ly = sdl_axis_to_vita(api->joystick_get_axis(video->joystick, 1));
        rx = sdl_axis_to_vita(api->joystick_get_axis(video->joystick, 2));
        ry = sdl_axis_to_vita(api->joystick_get_axis(video->joystick, 3));
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
    if (video->controller && video->rumble_pending) {
        rumble = true;
        rumble_low = video->rumble_low;
        rumble_high = video->rumble_high;
        video->rumble_pending = false;
    }
    pthread_mutex_unlock(&video->lock);

    if (rumble && api->game_controller_rumble)
        api->game_controller_rumble(video->controller, rumble_low, rumble_high, 0xFFFFFFFFu);
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
        if (video->api && video->api->quit_subsystem) {
            if (video->controller_ready)
                video->api->quit_subsystem(SDL_INIT_GAMECONTROLLER);
            video->api->quit_subsystem(SDL_INIT_VIDEO);
        }
        return NULL;
    }

    for (;;) {
        const uint8_t *frame;
        int width, height, pitch;
        struct timespec wait_until;

        /* Poll input even when no frame is waiting, so a desktop keyboard or
           controller stays responsive while the guest is busy. */
        video_sample_input(video);

        if (video->window_closed) {
            fprintf(stderr, "vita2native: window closed, exiting\n");
            fflush(stderr);
            _exit(0);
        }

        clock_gettime(CLOCK_REALTIME, &wait_until);
        wait_until.tv_nsec += 16 * 1000000L;
        if (wait_until.tv_nsec >= 1000000000L) {
            wait_until.tv_sec++;
            wait_until.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&video->lock);
        while (!video->stop && !video->has_frame) {
            if (pthread_cond_timedwait(&video->cond, &video->lock, &wait_until) == ETIMEDOUT)
                break;
        }
        if (video->stop) {
            pthread_mutex_unlock(&video->lock);
            break;
        }
        if (!video->has_frame) {
            pthread_mutex_unlock(&video->lock);
            continue;
        }

        video->has_frame = false;
        video->busy = true;
        frame = video->frame;
        width = video->frame_width;
        height = video->frame_height;
        pitch = video->frame_pitch;
        pthread_mutex_unlock(&video->lock);

        video_present(video, frame, width, height, pitch);

        pthread_mutex_lock(&video->lock);
        video->busy = false;
        pthread_cond_broadcast(&video->cond);
        pthread_mutex_unlock(&video->lock);
    }

    video_destroy_sdl(video);
    if (video->api && video->api->quit_subsystem) {
        if (video->controller_ready)
            video->api->quit_subsystem(SDL_INIT_GAMECONTROLLER);
        video->api->quit_subsystem(SDL_INIT_VIDEO);
    }
    return NULL;
}

HostVideo *host_video_open(void)
{
    static const char *const library_names[] = {
        "libSDL2-2.0.so.0", "libSDL2-2.0.so", "libSDL2.so", NULL,
    };
    HostVideo *video;
    SdlApi *api;

    api = calloc(1, sizeof(*api));
    if (!api)
        return NULL;

    for (int i = 0; library_names[i] && !api->library; i++)
        api->library = dlopen(library_names[i], RTLD_NOW | RTLD_LOCAL);
    if (!api->library) {
        fprintf(stderr, "vita2native: SDL is not available; video output disabled\n");
        free(api);
        return NULL;
    }

#define LOAD_SDL(field, name)                                                                      \
    do {                                                                                           \
        *(void **)(&api->field) = dlsym(api->library, name);                                       \
        if (!api->field) {                                                                         \
            fprintf(stderr, "vita2native: SDL is missing %s\n", name);                             \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

    LOAD_SDL(init_subsystem, "SDL_InitSubSystem");
    LOAD_SDL(quit_subsystem, "SDL_QuitSubSystem");
    LOAD_SDL(get_error, "SDL_GetError");
    LOAD_SDL(create_window, "SDL_CreateWindow");
    LOAD_SDL(destroy_window, "SDL_DestroyWindow");
    LOAD_SDL(create_renderer, "SDL_CreateRenderer");
    LOAD_SDL(destroy_renderer, "SDL_DestroyRenderer");
    LOAD_SDL(create_texture, "SDL_CreateTexture");
    LOAD_SDL(destroy_texture, "SDL_DestroyTexture");
    LOAD_SDL(update_texture, "SDL_UpdateTexture");
    LOAD_SDL(render_clear, "SDL_RenderClear");
    LOAD_SDL(render_copy, "SDL_RenderCopy");
    LOAD_SDL(render_present, "SDL_RenderPresent");
    LOAD_SDL(render_set_logical_size, "SDL_RenderSetLogicalSize");
    LOAD_SDL(get_window_size, "SDL_GetWindowSize");
    LOAD_SDL(get_renderer_info, "SDL_GetRendererInfo");
    LOAD_SDL(show_cursor, "SDL_ShowCursor");
    *(void **)(&api->get_current_video_driver) = dlsym(api->library, "SDL_GetCurrentVideoDriver");
    *(void **)(&api->set_texture_scale_mode) =
        dlsym(api->library, "SDL_SetTextureScaleMode");
    /*
     * SDL_RenderSetLogicalSize does the letterbox scaling itself, and it reads
     * this hint rather than the texture's scale mode, so both are set. The
     * hint must be in place before any texture or renderer exists.
     */
    {
        int (*set_hint)(const char *, const char *) =
            (int (*)(const char *, const char *))dlsym(api->library, "SDL_SetHint");

        if (set_hint)
            set_hint("SDL_RENDER_SCALE_QUALITY", "0"); /* nearest */
    }

    /* Input is optional: every symbol below may be absent on a minimal SDL
       build, and missing video symbols are the only hard failures. */
    *(void **)(&api->pump_events) = dlsym(api->library, "SDL_PumpEvents");
    *(void **)(&api->get_keyboard_state) = dlsym(api->library, "SDL_GetKeyboardState");
    *(void **)(&api->num_joysticks) = dlsym(api->library, "SDL_NumJoysticks");
    *(void **)(&api->is_game_controller) = dlsym(api->library, "SDL_IsGameController");
    *(void **)(&api->game_controller_open) = dlsym(api->library, "SDL_GameControllerOpen");
    *(void **)(&api->game_controller_close) = dlsym(api->library, "SDL_GameControllerClose");
    *(void **)(&api->game_controller_get_button) =
        dlsym(api->library, "SDL_GameControllerGetButton");
    *(void **)(&api->game_controller_get_axis) = dlsym(api->library, "SDL_GameControllerGetAxis");
    *(void **)(&api->game_controller_name) = dlsym(api->library, "SDL_GameControllerName");
    *(void **)(&api->game_controller_get_attached) =
        dlsym(api->library, "SDL_GameControllerGetAttached");
    *(void **)(&api->joystick_open) = dlsym(api->library, "SDL_JoystickOpen");
    *(void **)(&api->joystick_close) = dlsym(api->library, "SDL_JoystickClose");
    *(void **)(&api->joystick_get_button) = dlsym(api->library, "SDL_JoystickGetButton");
    *(void **)(&api->joystick_get_axis) = dlsym(api->library, "SDL_JoystickGetAxis");
    *(void **)(&api->joystick_get_attached) = dlsym(api->library, "SDL_JoystickGetAttached");
    *(void **)(&api->joystick_num_buttons) = dlsym(api->library, "SDL_JoystickNumButtons");
    *(void **)(&api->joystick_num_axes) = dlsym(api->library, "SDL_JoystickNumAxes");
    *(void **)(&api->joystick_name) = dlsym(api->library, "SDL_JoystickName");
    *(void **)(&api->game_controller_mapping) = dlsym(api->library, "SDL_GameControllerMapping");
    *(void **)(&api->free) = dlsym(api->library, "SDL_free");
    *(void **)(&api->get_mouse_state) = dlsym(api->library, "SDL_GetMouseState");
    *(void **)(&api->game_controller_rumble) = dlsym(api->library, "SDL_GameControllerRumble");
    *(void **)(&api->poll_event) = dlsym(api->library, "SDL_PollEvent");

#undef LOAD_SDL

    video = calloc(1, sizeof(*video));
    if (!video)
        goto fail;

    video->api = api;
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
    if (api->library)
        dlclose(api->library);
    free(api);
    return NULL;
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
        video->has_frame = false;
        pthread_cond_broadcast(&video->cond);
        pthread_mutex_unlock(&video->lock);
        pthread_join(video->thread, NULL);
    }

    pthread_mutex_destroy(&video->lock);
    pthread_cond_destroy(&video->cond);
    free(video->frame);
    if (video->api) {
        if (video->api->library)
            dlclose(video->api->library);
        free(video->api);
    }
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

void host_video_present(HostVideo *video, const void *rgba, int width, int height,
                        int pitch_pixels)
{
    size_t needed;

    if (!video || !rgba || width <= 0 || height <= 0 || pitch_pixels < width)
        return;

    needed = (size_t)pitch_pixels * (size_t)height * 4;
    pthread_mutex_lock(&video->lock);

    /* The consumer is uploading the previous frame; drop this one rather than
       blocking the guest or overwriting pixels it is reading. */
    if (video->busy) {
        pthread_mutex_unlock(&video->lock);
        return;
    }

    if (video->frame_capacity < needed) {
        uint8_t *grown = realloc(video->frame, needed);

        if (!grown) {
            pthread_mutex_unlock(&video->lock);
            return;
        }
        video->frame = grown;
        video->frame_capacity = needed;
    }

    memcpy(video->frame, rgba, needed);
    video->frame_width = width;
    video->frame_height = height;
    video->frame_pitch = pitch_pixels;
    video->has_frame = true;
    pthread_cond_signal(&video->cond);
    pthread_mutex_unlock(&video->lock);
}

bool host_video_present_texture(HostVideo *video, unsigned int texture, int width, int height)
{
    HostGpuDevice device;

    if (!video || !video->direct_present || !texture || width <= 0 || height <= 0)
        return false;

    device = host_gpu_current();
    if (!device)
        return false;

    if (!video->egl_attached) {
        if (host_gpu_attach_kms(device, -1, 0, 0) != 0) {
            fprintf(stderr, "vita2native: direct EGL attach failed\n");
            return false;
        }
        video->egl_attached = true;
    }

    host_gpu_set_overlay(device, video->overlay_pixels, video->overlay_width,
                         video->overlay_height);
    host_gpu_present_texture(device, texture, width, height);
    return true;
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

__attribute__((weak)) int host_gpu_attach_window(HostGpuDevice device, void *native_window)
{
    (void)device;
    (void)native_window;
    return -1;
}

__attribute__((weak)) int host_gpu_attach_kms(HostGpuDevice device, int drm_fd, int width,
                                               int height)
{
    (void)device;
    (void)drm_fd;
    (void)width;
    (void)height;
    return -1;
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
