/*
 * Video output through SDL.
 *
 * SDL is the only presenter. Its video driver does the platform work:
 * KMSDRM on handhelds (where it sets the CRTC before flipping), X11 or Wayland
 * on desktops, and dummy/offscreen for headless runs. Keeping one
 * implementation means there is no custom DRM/fbdev code to chase across
 * vendor kernels.
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
#include "host.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;

typedef struct SDL_Rect {
    int x, y, w, h;
} SDL_Rect;

typedef struct SDL_RendererInfo {
    const char *name;
    unsigned int flags;
    unsigned int num_texture_formats;
    unsigned int texture_formats[16];
    int max_texture_width;
    int max_texture_height;
} SDL_RendererInfo;

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
    int (*update_texture)(SDL_Texture *, const SDL_Rect *, const void *, int);
    int (*render_clear)(SDL_Renderer *);
    int (*render_copy)(SDL_Renderer *, SDL_Texture *, const SDL_Rect *, const SDL_Rect *);
    void (*render_present)(SDL_Renderer *);
    int (*render_set_logical_size)(SDL_Renderer *, int, int);
    int (*get_window_size)(SDL_Window *, int *, int *);
    int (*get_renderer_info)(SDL_Renderer *, SDL_RendererInfo *);
    const char *(*get_current_video_driver)(void);
    int (*show_cursor)(int);
} SdlApi;

#define SDL_INIT_VIDEO 0x00000020u
#define SDL_WINDOW_SHOWN 0x00000004u
#define SDL_WINDOW_FULLSCREEN_DESKTOP 0x00001001u
#define SDL_RENDERER_ACCELERATED 0x00000002u
#define SDL_RENDERER_PRESENTVSYNC 0x00000004u
#define SDL_PIXELFORMAT_ABGR8888 0x16762004u
#define SDL_TEXTUREACCESS_STREAMING 1
#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000u

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
    bool stretch;
    bool windowed;
};

static void video_present(HostVideo *video, const void *rgba, int width, int height,
                          int pitch_pixels)
{
    SdlApi *api = video->api;

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
        if (!video->stretch)
            api->render_set_logical_size(video->renderer, width, height);
    }

    /* Guest pixels are A8B8G8R8, i.e. SDL_PIXELFORMAT_ABGR8888. */
    api->update_texture(video->texture, NULL, rgba, pitch_pixels * 4);
    api->render_clear(video->renderer);
    api->render_copy(video->renderer, video->texture, NULL, NULL);
    api->render_present(video->renderer);
}

static void video_destroy_sdl(HostVideo *video)
{
    SdlApi *api = video->api;

    if (!api)
        return;

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

    if (!video->windowed)
        window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    video->window = api->create_window("vita2native", SDL_WINDOWPOS_UNDEFINED,
                                       SDL_WINDOWPOS_UNDEFINED, 960, 544, window_flags);
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

    if (api->get_renderer_info) {
        SDL_RendererInfo info;

        memset(&info, 0, sizeof(info));
        if (api->get_renderer_info(video->renderer, &info) == 0)
            fprintf(stderr, "vita2native: SDL renderer '%s' flags 0x%x\n",
                    info.name ? info.name : "?", info.flags);
    }

    fprintf(stderr, "vita2native: presenting through SDL (%s video, %dx%d window)\n",
            api->get_current_video_driver ? api->get_current_video_driver() : "unknown",
            video->width, video->height);
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
        if (video->api && video->api->quit_subsystem)
            video->api->quit_subsystem(SDL_INIT_VIDEO);
        return NULL;
    }

    for (;;) {
        const uint8_t *frame;
        int width, height, pitch;

        pthread_mutex_lock(&video->lock);
        while (!video->stop && !video->has_frame)
            pthread_cond_wait(&video->cond, &video->lock);
        if (video->stop) {
            pthread_mutex_unlock(&video->lock);
            break;
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
    if (video->api && video->api->quit_subsystem)
        video->api->quit_subsystem(SDL_INIT_VIDEO);
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

#undef LOAD_SDL

    video = calloc(1, sizeof(*video));
    if (!video)
        goto fail;

    video->api = api;
    video->stretch = getenv("VITA2NATIVE_STRETCH") != NULL;
    video->windowed = getenv("VITA2NATIVE_SDL_WINDOWED") != NULL;

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
