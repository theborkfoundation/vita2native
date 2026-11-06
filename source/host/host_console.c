/*
 * Framebuffer presentation.
 *
 * Two backends:
 *
 *   - DRM/KMS: allocate our own dumb buffers, map them, and point the CRTC at
 *     them. This is the only reliable path on vendor kernels whose fbdev
 *     emulation is not wired to the active scanout. The R36S is such a case:
 *     its VOP is enabled and scanning a null buffer, while /dev/fb0 points at a
 *     GEM buffer nobody ever displays.
 *
 *   - fbdev: fallback for simpler setups where /dev/fb0 really is the panel.
 *
 * Neither backend renders through the GPU: the headless GXM backend has already
 * produced a sorted-out guest framebuffer, so this file only converts and
 * scales it onto the scanout buffer.
 */
#ifndef _FILE_OFFSET_BITS
/* The DRM dumb-buffer mmap offset does not fit in a 32-bit off_t. */
#define _FILE_OFFSET_BITS 64
#endif

#include "host.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CONSOLE_MAX_BUFFERS 2

/* libdrm exposes this as an enum; the kernel UAPI only documents the value. */
#ifndef DRM_MODE_CONNECTED
#define DRM_MODE_CONNECTED 1
#endif

/*
 * SDL is loaded with dlopen (the headless ARM build has no SDL at link time),
 * using the renderer API only: its handles are opaque and the few constants
 * below are part of SDL's stable ABI. The core guest framebuffer is
 * A8B8G8R8, which is exactly SDL_PIXELFORMAT_ABGR8888 on little-endian hosts,
 * so frames are uploaded without a CPU conversion.
 */
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;

typedef struct SDL_Rect {
    int x, y, w, h;
} SDL_Rect;

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

enum {
    CONSOLE_BACKEND_DRM = 0,
    CONSOLE_BACKEND_FBDEV = 1,
    CONSOLE_BACKEND_SDL = 2,
};

typedef struct {
    uint32_t handle;
    uint32_t fb_id;
    uint8_t *map;
    size_t size;
    uint32_t pitch;
} ConsoleBuffer;

struct HostConsole {
    int fd;
    int backend;
    int width;
    int height;
    bool stretch;

    /* 8-bit source channel -> destination word, one table per channel. */
    uint32_t lut[3][256];
    /* Precomputed opaque alpha bits, zero for formats without alpha. */
    uint32_t alpha;

    /* fbdev */
    uint8_t *memory;
    size_t size;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;

    /* DRM/KMS */
    uint32_t crtc_id;
    uint32_t connector_id;
    ConsoleBuffer buffers[CONSOLE_MAX_BUFFERS];
    int buffer_count;
    int front;
    int back;
    bool flip_pending;
    bool use_flip;

    /* SDL */
    SdlApi *sdl;
    SDL_Window *sdl_window;
    SDL_Renderer *sdl_renderer;
    SDL_Texture *sdl_texture;
    int sdl_texture_width;
    int sdl_texture_height;
};

static bool console_debug;

static void build_lut(uint32_t lut[256], unsigned int offset, unsigned int length)
{
    uint32_t maximum;

    if (!length)
        return;
    if (length > 8)
        length = 8;
    maximum = (1u << length) - 1u;
    for (unsigned int i = 0; i < 256; i++)
        lut[i] = ((uint32_t)i * maximum / 255u) << offset;
}


/*
 * Nearest-neighbour scale plus channel conversion. Letterboxes to preserve the
 * guest aspect ratio unless stretching was requested; the bars are cleared to
 * black so a stale frame never leaks around the edges.
 */
static void console_blit(HostConsole *console, uint8_t *dst, size_t dst_pitch, int dst_width,
                         int dst_height, const uint8_t *src, int width, int height,
                         int pitch_pixels)
{
    uint32_t draw_width = (uint32_t)dst_width;
    uint32_t draw_height = (uint32_t)dst_height;
    uint32_t x_offset = 0;
    uint32_t y_offset = 0;

    if (width <= 0 || height <= 0 || pitch_pixels < width)
        return;

    if (!console->stretch) {
        double scale_x = (double)dst_width / (double)width;
        double scale_y = (double)dst_height / (double)height;

        if (scale_x < scale_y) {
            draw_width = (uint32_t)dst_width;
            draw_height = (uint32_t)((double)height * scale_x + 0.5);
        } else {
            draw_height = (uint32_t)dst_height;
            draw_width = (uint32_t)((double)width * scale_y + 0.5);
        }
        if (!draw_width)
            draw_width = 1;
        if (!draw_height)
            draw_height = 1;
        x_offset = ((uint32_t)dst_width - draw_width) / 2;
        y_offset = ((uint32_t)dst_height - draw_height) / 2;
    }

    memset(dst, 0, dst_pitch * (size_t)dst_height);

    for (uint32_t dy = 0; dy < draw_height; dy++) {
        uint32_t sy = (uint32_t)((uint64_t)dy * (uint32_t)height / draw_height);
        const uint8_t *src_row = src + (size_t)sy * (size_t)pitch_pixels * 4;
        uint32_t *dst_row =
            (uint32_t *)(dst + (size_t)(y_offset + dy) * dst_pitch) + x_offset;
        uint32_t step = ((uint32_t)width << 16) / draw_width;
        uint32_t sx = 0;

        for (uint32_t dx = 0; dx < draw_width; dx++, sx += step) {
            const uint8_t *p = src_row + (size_t)(sx >> 16) * 4;

            /* Guest pixels are A8B8G8R8, so bytes are R, G, B, A. */
            dst_row[dx] = console->lut[0][p[0]] | console->lut[1][p[1]] |
                          console->lut[2][p[2]] | console->alpha;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* fbdev                                                                      */
/* ------------------------------------------------------------------------- */

static HostConsole *console_open_fbdev(const char *path)
{
    HostConsole *console;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    int fd;

    if (!path || !path[0])
        return NULL;

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return NULL;

    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0 || var.bits_per_pixel != 32 || !var.xres ||
        !var.yres || !fix.line_length) {
        close(fd);
        return NULL;
    }

    console = calloc(1, sizeof(*console));
    if (!console) {
        close(fd);
        return NULL;
    }

    console->size = (size_t)fix.line_length * var.yres;
    console->memory = mmap(NULL, console->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (console->memory == MAP_FAILED) {
        free(console);
        close(fd);
        return NULL;
    }

    console->backend = CONSOLE_BACKEND_FBDEV;
    console->fd = fd;
    console->var = var;
    console->fix = fix;
    console->width = (int)var.xres;
    console->height = (int)var.yres;
    console->stretch = getenv("VITA2NATIVE_STRETCH") != NULL;

    build_lut(console->lut[0], var.red.offset, var.red.length);
    build_lut(console->lut[1], var.green.offset, var.green.length);
    build_lut(console->lut[2], var.blue.offset, var.blue.length);
    if (var.transp.length) {
        uint32_t maximum = (1u << var.transp.length) - 1u;

        console->alpha = maximum << var.transp.offset;
    }
    return console;
}

/* ------------------------------------------------------------------------- */
/* DRM/KMS                                                                    */
/* ------------------------------------------------------------------------- */

static int drm_ioctl(int fd, unsigned long request, void *arg)
{
    int result;

    do {
        result = ioctl(fd, request, arg);
    } while (result < 0 && errno == EINTR);
    return result;
}

static bool drm_wait_flip(HostConsole *console, int timeout_ms)
{
    struct pollfd pfd = {
        .fd = console->fd,
        .events = POLLIN,
    };
    uint8_t buffer[256];

    while (poll(&pfd, 1, timeout_ms) > 0) {
        ssize_t length = read(console->fd, buffer, sizeof(buffer));

        if (length <= 0)
            return false;

        for (ssize_t offset = 0; offset + (ssize_t)sizeof(struct drm_event) <= length;) {
            const struct drm_event *event = (const struct drm_event *)(buffer + offset);

            if (!event->length || offset + (ssize_t)event->length > length)
                return false;
            if (event->type == DRM_EVENT_FLIP_COMPLETE)
                return true;
            offset += event->length;
        }

        /* Only flips are requested on this fd; anything else is unexpected but
           not fatal, so keep waiting for the completion event. */
        timeout_ms = 100;
    }

    return false;
}

static bool drm_dirty(HostConsole *console, const ConsoleBuffer *buffer)
{
    struct drm_mode_fb_dirty_cmd dirty = {
        .fb_id = buffer->fb_id,
    };

    /* num_clips == 0 means "the whole framebuffer". Some drivers have no dirty
       op at all, so a failure here is harmless. */
    return drm_ioctl(console->fd, DRM_IOCTL_MODE_DIRTYFB, &dirty) == 0;
}

static void drm_wait_vblank(HostConsole *console)
{
    union drm_wait_vblank vblank;

    memset(&vblank, 0, sizeof(vblank));
    vblank.request.type = _DRM_VBLANK_RELATIVE;
    vblank.request.sequence = 1;
    /* Returns an error immediately if the CRTC is not scanning, so this cannot
       wedge a headless run. */
    drm_ioctl(console->fd, DRM_IOCTL_WAIT_VBLANK, &vblank);
}

static bool drm_flip(HostConsole *console, int target)
{
    struct drm_mode_crtc_page_flip flip = {
        .crtc_id = console->crtc_id,
        .fb_id = console->buffers[target].fb_id,
        .flags = DRM_MODE_PAGE_FLIP_EVENT,
    };

    if (drm_ioctl(console->fd, DRM_IOCTL_MODE_PAGE_FLIP, &flip) < 0) {
        if (console_debug)
            fprintf(stderr, "vita2native: page flip failed: %s\n", strerror(errno));
        return false;
    }

    return drm_wait_flip(console, 100);
}

static void drm_close_buffers(HostConsole *console)
{
    for (int i = 0; i < CONSOLE_MAX_BUFFERS; i++) {
        ConsoleBuffer *buffer = &console->buffers[i];

        if (buffer->map && buffer->map != MAP_FAILED)
            munmap(buffer->map, buffer->size);
        if (buffer->fb_id)
            drm_ioctl(console->fd, DRM_IOCTL_MODE_RMFB, &buffer->fb_id);
        if (buffer->handle) {
            struct drm_mode_destroy_dumb destroy = {
                .handle = buffer->handle,
            };

            drm_ioctl(console->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
        }
        buffer->map = NULL;
        buffer->fb_id = 0;
        buffer->handle = 0;
    }
    console->buffer_count = 0;
}

static bool drm_create_buffer(HostConsole *console, ConsoleBuffer *buffer)
{
    struct drm_mode_create_dumb create = {
        .width = (uint32_t)console->width,
        .height = (uint32_t)console->height,
        .bpp = 32,
    };
    struct drm_mode_map_dumb map;
    struct drm_mode_fb_cmd framebuffer;
    void *memory;

    if (drm_ioctl(console->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
        fprintf(stderr, "vita2native: DRM dumb buffer creation failed: %s\n", strerror(errno));
        return false;
    }

    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (drm_ioctl(console->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        fprintf(stderr, "vita2native: DRM dumb buffer mapping failed: %s\n", strerror(errno));
        return false;
    }

    memory = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, console->fd, map.offset);
    if (memory == MAP_FAILED) {
        fprintf(stderr,
                "vita2native: cannot map DRM dumb buffer (size %llu, offset %llu, off_t %u): "
                "%s\n",
                (unsigned long long)create.size, (unsigned long long)map.offset,
                (unsigned int)sizeof(off_t), strerror(errno));
        return false;
    }

    memset(&framebuffer, 0, sizeof(framebuffer));
    framebuffer.width = create.width;
    framebuffer.height = create.height;
    framebuffer.pitch = create.pitch;
    framebuffer.bpp = 32;
    framebuffer.depth = 24;
    framebuffer.handle = create.handle;
    if (drm_ioctl(console->fd, DRM_IOCTL_MODE_ADDFB, &framebuffer) < 0) {
        fprintf(stderr, "vita2native: DRM framebuffer creation failed: %s\n", strerror(errno));
        munmap(memory, create.size);
        return false;
    }

    buffer->handle = create.handle;
    buffer->fb_id = framebuffer.fb_id;
    buffer->map = memory;
    buffer->size = create.size;
    buffer->pitch = create.pitch;
    return true;
}

typedef struct {
    uint32_t connector_id;
    uint32_t encoder_id;
    uint32_t crtc_id;
    struct drm_mode_modeinfo mode;
} DrmTarget;

static bool drm_pick_connector(int fd, const uint32_t *connectors, uint32_t count,
                               DrmTarget *target)
{
    struct drm_mode_modeinfo *modes = NULL;
    uint32_t *encoders = NULL;
    uint32_t mode_capacity = 0;
    uint32_t encoder_capacity = 0;
    bool found = false;

    for (uint32_t i = 0; i < count && !found; i++) {
        struct drm_mode_get_connector connector;

        memset(&connector, 0, sizeof(connector));
        connector.connector_id = connectors[i];
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) < 0)
            continue;
        if (connector.connection != DRM_MODE_CONNECTED || !connector.count_modes)
            continue;

        if (connector.count_modes > mode_capacity) {
            struct drm_mode_modeinfo *grown =
                realloc(modes, connector.count_modes * sizeof(*modes));

            if (!grown)
                break;
            modes = grown;
            mode_capacity = connector.count_modes;
        }
        if (connector.count_encoders > encoder_capacity) {
            uint32_t *grown = realloc(encoders, connector.count_encoders * sizeof(*encoders));

            if (!grown)
                break;
            encoders = grown;
            encoder_capacity = connector.count_encoders;
        }

        memset(&connector, 0, sizeof(connector));
        connector.connector_id = connectors[i];
        connector.modes_ptr = (uint64_t)(uintptr_t)modes;
        connector.count_modes = mode_capacity;
        connector.encoders_ptr = (uint64_t)(uintptr_t)encoders;
        connector.count_encoders = encoder_capacity;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) < 0)
            continue;
        if (connector.connection != DRM_MODE_CONNECTED || !connector.count_modes)
            continue;

        target->connector_id = connector.connector_id;
        target->encoder_id = connector.encoder_id;
        if (!target->encoder_id && connector.count_encoders)
            target->encoder_id = encoders[0];
        target->mode = modes[0];
        found = true;
    }

    free(modes);
    free(encoders);
    return found && target->encoder_id;
}

static bool drm_pick_crtc(int fd, const DrmTarget *target, const uint32_t *crtcs,
                          uint32_t count, uint32_t *crtc_id)
{
    struct drm_mode_get_encoder encoder;

    memset(&encoder, 0, sizeof(encoder));
    encoder.encoder_id = target->encoder_id;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) < 0)
        return false;

    if (encoder.crtc_id) {
        *crtc_id = encoder.crtc_id;
        return true;
    }

    /* Fall back to the first CRTC this encoder can drive. */
    for (uint32_t bit = 0; bit < 32; bit++) {
        if (!(encoder.possible_crtcs & (1u << bit)))
            continue;
        if (bit < count) {
            *crtc_id = crtcs[bit];
            return true;
        }
    }

    if (count) {
        *crtc_id = crtcs[0];
        return true;
    }

    return false;
}

static HostConsole *console_open_drm(const char *path)
{
    struct drm_mode_card_res resources;
    uint32_t *crtcs = NULL;
    uint32_t *connectors = NULL;
    uint32_t *encoders = NULL;
    uint32_t crtc_count, connector_count, encoder_count;
    DrmTarget target;
    HostConsole *console = NULL;
    struct drm_mode_crtc crtc;
    bool master = false;
    int fd;

    if (!path || !path[0])
        return NULL;

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return NULL;

    /* Without master SETCRTC is refused, so this is mandatory. An explicit
       drop happens at close; the kernel also drops it on close. */
    if (drm_ioctl(fd, DRM_IOCTL_SET_MASTER, NULL) == 0)
        master = true;

    memset(&resources, 0, sizeof(resources));
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0)
        goto fail;

    /* The first call only sizes the arrays; the kernel requires the same
       capacities to be passed back on the second call. */
    crtc_count = resources.count_crtcs;
    connector_count = resources.count_connectors;
    encoder_count = resources.count_encoders;
    if (!crtc_count || !connector_count)
        goto fail;

    crtcs = calloc(crtc_count, sizeof(*crtcs));
    connectors = calloc(connector_count, sizeof(*connectors));
    encoders = calloc(encoder_count ? encoder_count : 1, sizeof(*encoders));
    if (!crtcs || !connectors || !encoders)
        goto fail;

    memset(&resources, 0, sizeof(resources));
    resources.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    resources.count_crtcs = crtc_count;
    resources.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
    resources.count_connectors = connector_count;
    resources.encoder_id_ptr = (uint64_t)(uintptr_t)encoders;
    resources.count_encoders = encoder_count;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &resources) < 0)
        goto fail;

    memset(&target, 0, sizeof(target));
    if (!drm_pick_connector(fd, connectors, resources.count_connectors, &target)) {
        if (console_debug)
            fprintf(stderr, "vita2native: %s has no connected connector\n", path);
        goto fail;
    }
    if (!drm_pick_crtc(fd, &target, crtcs, resources.count_crtcs, &target.crtc_id))
        goto fail;

    /* Prefer the timing the CRTC is already running so a working panel setup
       is never replaced by identical-but-different timings. */
    memset(&crtc, 0, sizeof(crtc));
    crtc.crtc_id = target.crtc_id;
    if (drm_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) == 0 && crtc.mode_valid)
        target.mode = crtc.mode;

    if (!target.mode.hdisplay || !target.mode.vdisplay)
        goto fail;

    console = calloc(1, sizeof(*console));
    if (!console)
        goto fail;

    console->fd = fd;
    console->backend = CONSOLE_BACKEND_DRM;
    console->crtc_id = target.crtc_id;
    console->connector_id = target.connector_id;
    console->width = (int)target.mode.hdisplay;
    console->height = (int)target.mode.vdisplay;
    console->stretch = getenv("VITA2NATIVE_STRETCH") != NULL;
    /* The Rockchip vendor kernel accepts PAGE_FLIP and sends completion events
       but keeps scanning the original buffer, so a flip never reaches the
       panel. Writing into the SETCRTC buffer always works; double buffering is
       opt-in for kernels where flips are known to be wired up. */
    console->use_flip = getenv("VITA2NATIVE_PAGE_FLIP") != NULL;
    console->front = 0;
    console->back = console->use_flip ? 1 : 0;

    build_lut(console->lut[0], 16, 8);
    build_lut(console->lut[1], 8, 8);
    build_lut(console->lut[2], 0, 8);

    for (int i = 0; i < CONSOLE_MAX_BUFFERS; i++) {
        if (!drm_create_buffer(console, &console->buffers[i]))
            goto fail_console;
        console->buffer_count++;
    }

    {
        struct drm_mode_crtc set;

        memset(&set, 0, sizeof(set));
        set.crtc_id = console->crtc_id;
        set.fb_id = console->buffers[0].fb_id;
        set.set_connectors_ptr = (uint64_t)(uintptr_t)&console->connector_id;
        set.count_connectors = 1;
        set.mode = target.mode;
        set.mode_valid = 1;
        if (drm_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &set) < 0) {
            fprintf(stderr, "vita2native: DRM modeset on %s failed: %s\n", path, strerror(errno));
            goto fail_console;
        }
    }

    fprintf(stderr,
            "vita2native: presenting to %s (%dx%d, DRM crtc %u connector %u%s)\n", path,
            console->width, console->height, console->crtc_id, console->connector_id,
            master ? "" : ", no master");
    free(crtcs);
    free(connectors);
    free(encoders);
    return console;

fail_console:
    drm_close_buffers(console);
    if (console->fd >= 0) {
        close(console->fd);
        fd = -1;
    }
    free(console);
    console = NULL;
fail:
    if (!console && console_debug)
        fprintf(stderr, "vita2native: cannot present through %s: %s\n", path, strerror(errno));
    if (fd >= 0)
        close(fd);
    free(crtcs);
    free(connectors);
    free(encoders);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* SDL                                                                        */
/* ------------------------------------------------------------------------- */

static HostConsole *console_open_sdl(void)
{
    static const char *const library_names[] = {
        "libSDL2-2.0.so.0", "libSDL2-2.0.so", "libSDL2.so", NULL,
    };
    SdlApi *api;
    HostConsole *console = NULL;
    const char *video_driver = getenv("VITA2NATIVE_SDL_DRIVER");
    unsigned int window_flags = SDL_WINDOW_SHOWN;
    int panel_width = 0, panel_height = 0;

    api = calloc(1, sizeof(*api));
    if (!api)
        return NULL;

    for (int i = 0; library_names[i] && !api->library; i++)
        api->library = dlopen(library_names[i], RTLD_NOW | RTLD_LOCAL);
    if (!api->library) {
        if (console_debug)
            fprintf(stderr, "vita2native: SDL is not available\n");
        goto fail;
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
    LOAD_SDL(show_cursor, "SDL_ShowCursor");
    *(void **)(&api->get_current_video_driver) = dlsym(api->library, "SDL_GetCurrentVideoDriver");

#undef LOAD_SDL

    if (video_driver && video_driver[0])
        setenv("SDL_VIDEODRIVER", video_driver, 1);

    if (api->init_subsystem(SDL_INIT_VIDEO) != 0) {
        if (console_debug)
            fprintf(stderr, "vita2native: SDL video init failed: %s\n", api->get_error());
        goto fail;
    }

    console = calloc(1, sizeof(*console));
    if (!console)
        goto fail;

    console->sdl = api;
    console->backend = CONSOLE_BACKEND_SDL;
    console->stretch = getenv("VITA2NATIVE_STRETCH") != NULL;

    if (!getenv("VITA2NATIVE_SDL_WINDOWED"))
        window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    console->sdl_window = api->create_window("vita2native", SDL_WINDOWPOS_UNDEFINED,
                                             SDL_WINDOWPOS_UNDEFINED, 960, 544, window_flags);
    if (!console->sdl_window) {
        fprintf(stderr, "vita2native: SDL window creation failed: %s\n", api->get_error());
        goto fail_console;
    }

    console->sdl_renderer = api->create_renderer(
        console->sdl_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!console->sdl_renderer)
        console->sdl_renderer = api->create_renderer(console->sdl_window, -1, 0);
    if (!console->sdl_renderer) {
        fprintf(stderr, "vita2native: SDL renderer creation failed: %s\n", api->get_error());
        goto fail_console;
    }

    api->show_cursor(0);
    api->get_window_size(console->sdl_window, &panel_width, &panel_height);
    console->width = panel_width > 0 ? panel_width : 960;
    console->height = panel_height > 0 ? panel_height : 544;

    fprintf(stderr, "vita2native: presenting through SDL (%s video, %dx%d window)\n",
            api->get_current_video_driver ? api->get_current_video_driver() : "unknown",
            console->width, console->height);
    return console;

fail_console:
    if (console->sdl_renderer)
        api->destroy_renderer(console->sdl_renderer);
    if (console->sdl_window)
        api->destroy_window(console->sdl_window);
    api->quit_subsystem(SDL_INIT_VIDEO);
    free(console);
    console = NULL;
fail:
    if (api->library)
        dlclose(api->library);
    free(api);
    return NULL;
}

static void console_present_sdl(HostConsole *console, const void *rgba, int width, int height,
                                int pitch_pixels)
{
    SdlApi *api = console->sdl;

    if (!api || !console->sdl_renderer)
        return;

    if (!console->sdl_texture || console->sdl_texture_width != width ||
        console->sdl_texture_height != height) {
        if (console->sdl_texture)
            api->destroy_texture(console->sdl_texture);
        console->sdl_texture = api->create_texture(console->sdl_renderer,
                                                   SDL_PIXELFORMAT_ABGR8888,
                                                   SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!console->sdl_texture) {
            console->sdl_texture_width = 0;
            console->sdl_texture_height = 0;
            return;
        }
        console->sdl_texture_width = width;
        console->sdl_texture_height = height;
        if (!console->stretch)
            api->render_set_logical_size(console->sdl_renderer, width, height);
    }

    /* Guest pixels are A8B8G8R8, which is SDL_PIXELFORMAT_ABGR8888 on a
       little-endian host: no per-pixel conversion is needed. */
    api->update_texture(console->sdl_texture, NULL, rgba, pitch_pixels * 4);
    api->render_clear(console->sdl_renderer);
    api->render_copy(console->sdl_renderer, console->sdl_texture, NULL, NULL);
    api->render_present(console->sdl_renderer);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

HostConsole *host_console_open(const char *path)
{
    const char *preference = getenv("VITA2NATIVE_VIDEO");
    const char *forced_fbdev = getenv("VITA2NATIVE_FBDEV");
    const char *forced_drm = getenv("VITA2NATIVE_DRM");

    console_debug = getenv("VITA2NATIVE_PRESENT_DEBUG") != NULL ||
                    getenv("VITA2NATIVE_DRM_DEBUG") != NULL ||
                    getenv("VITA2NATIVE_VIDEO_DEBUG") != NULL;

    /* An explicit path selects the backend by its device node. */
    if (path && path[0] && strcmp(path, "auto") != 0) {
        if (strstr(path, "/fb"))
            return console_open_fbdev(path);
        return console_open_drm(path);
    }

    if (getenv("VITA2NATIVE_NO_PRESENT"))
        return NULL;
    if (preference && strcmp(preference, "none") == 0)
        return NULL;

    /* Device-node overrides keep the previous behaviour. */
    if (forced_fbdev && forced_fbdev[0])
        return console_open_fbdev(forced_fbdev);
    if (forced_drm && forced_drm[0])
        return console_open_drm(forced_drm);

    /* SDL first: it covers KMSDRM, X11 and Wayland, which is what handheld
       frontends and desktops already use. */
    if (!preference || strcmp(preference, "auto") == 0 || strcmp(preference, "sdl") == 0) {
        HostConsole *console = console_open_sdl();

        if (console)
            return console;
        if (preference && strcmp(preference, "sdl") == 0)
            return NULL;
    }

    if (!preference || strcmp(preference, "auto") == 0 || strcmp(preference, "drm") == 0) {
        HostConsole *console = console_open_drm("/dev/dri/card0");

        if (console)
            return console;
        if (preference && strcmp(preference, "drm") == 0)
            return NULL;
    }

    if (!preference || strcmp(preference, "auto") == 0 || strcmp(preference, "fbdev") == 0)
        return console_open_fbdev("/dev/fb0");

    return NULL;
}

void host_console_close(HostConsole *console)
{
    if (!console)
        return;

    if (console->backend == CONSOLE_BACKEND_DRM) {
        if (console->flip_pending)
            drm_wait_flip(console, 100);
        drm_close_buffers(console);
        drm_ioctl(console->fd, DRM_IOCTL_DROP_MASTER, NULL);
    } else if (console->backend == CONSOLE_BACKEND_SDL) {
        SdlApi *api = console->sdl;

        if (api) {
            if (console->sdl_texture)
                api->destroy_texture(console->sdl_texture);
            if (console->sdl_renderer)
                api->destroy_renderer(console->sdl_renderer);
            if (console->sdl_window)
                api->destroy_window(console->sdl_window);
            api->quit_subsystem(SDL_INIT_VIDEO);
            if (api->library)
                dlclose(api->library);
            free(api);
            console->sdl = NULL;
        }
    } else {
        if (console->memory && console->memory != MAP_FAILED)
            munmap(console->memory, console->size);
    }

    if (console->fd >= 0)
        close(console->fd);
    free(console);
}

int host_console_width(const HostConsole *console)
{
    return console ? console->width : 0;
}

int host_console_height(const HostConsole *console)
{
    return console ? console->height : 0;
}

const char *host_console_backend(const HostConsole *console)
{
    if (!console)
        return "none";

    switch (console->backend) {
    case CONSOLE_BACKEND_DRM:
        return "drm";
    case CONSOLE_BACKEND_SDL:
        return "sdl";
    default:
        return "fbdev";
    }
}

void host_console_present(HostConsole *console, const void *rgba, int width, int height,
                          int pitch_pixels)
{
    if (!console || !rgba || width <= 0 || height <= 0 || pitch_pixels < width)
        return;

    if (console->backend == CONSOLE_BACKEND_SDL) {
        console_present_sdl(console, rgba, width, height, pitch_pixels);
        return;
    }

    if (console->backend == CONSOLE_BACKEND_DRM) {
        int target;

        if (console->flip_pending) {
            if (!drm_wait_flip(console, 100))
                console->use_flip = false;
            console->flip_pending = false;
        }

        target = console->use_flip ? console->back : 0;
        if (console->use_flip) {
            console_blit(console, console->buffers[target].map, console->buffers[target].pitch,
                         console->width, console->height, rgba, width, height, pitch_pixels);
        } else {
            /* buffer 0 is being scanned out, so never compose into it directly:
               a refresh would catch the cleared frame. Compose into buffer 1 and
               copy the finished image in one pass. */
            ConsoleBuffer *scanout = &console->buffers[0];
            ConsoleBuffer *stage = &console->buffers[1];

            console_blit(console, stage->map, stage->pitch, console->width, console->height, rgba,
                         width, height, pitch_pixels);

            /* Start the copy at the top of a frame and finish it before the
               beam gets far: the copy is much faster than the scanout, so the
               panel never sees a half-updated buffer. */
            drm_wait_vblank(console);
            memcpy(scanout->map, stage->map, (size_t)stage->pitch * (size_t)console->height);
            target = 0;
        }

        if (console->use_flip) {
            if (drm_flip(console, target)) {
                console->front = target;
                console->back = 1 - target;
                console->flip_pending = true;
            } else {
                /* Keep drawing into buffer 0 from now on; it is the one the
                   CRTC was pointed at by SETCRTC. */
                console->use_flip = false;
                drm_dirty(console, &console->buffers[0]);
            }
        } else {
            drm_dirty(console, &console->buffers[0]);
        }
        return;
    }

    console_blit(console, console->memory, console->fix.line_length, console->width,
                 console->height, rgba, width, height, pitch_pixels);
}
