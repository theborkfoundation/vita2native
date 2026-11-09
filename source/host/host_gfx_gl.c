/*
 * GLES 3.x backend.
 *
 * EGL and GLESv2 are loaded with dlopen() so vita2native links and runs on
 * systems without graphics libraries (the null backend takes over). When the
 * libraries are present, an offscreen GLES 3.x context is created at the Vita
 * resolution. SceGxm renders into this context and reads frames back into
 * guest memory.
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host_gfx_gl.h"
#include "host_gfx_internal.h"

/*
 * EGL contexts are bound to the thread that made them current. Guest threads
 * are host threads, so every guest thread that renders needs its own context;
 * contexts created here share all objects with the primary context.
 */
typedef struct {
    HostGles *gles;
    EGLContext context;
    EGLSurface surface;  /* this thread's own pbuffer */
    EGLSurface bound;    /* surface currently made current, if any */
    bool window;         /* true when this thread owns the window surface */
} GlThreadState;

static _Thread_local GlThreadState g_thread_state;
static pthread_t g_window_thread;
static bool g_window_thread_valid;
static pthread_mutex_t g_thread_contexts_lock = PTHREAD_MUTEX_INITIALIZER;

static const EGLint g_thread_context_attribs[] = {
    EGL_CONTEXT_CLIENT_VERSION, 3,
    EGL_NONE,
};

static const EGLint g_thread_pbuffer_attribs[] = {
    EGL_WIDTH,  1,
    EGL_HEIGHT, 1,
    EGL_NONE,
};

static void gl_remember_thread_context(HostGles *g, EGLContext context, EGLSurface surface)
{
    EGLContext *contexts;
    EGLSurface *surfaces;

    pthread_mutex_lock(&g_thread_contexts_lock);
    contexts = realloc(g->thread_contexts,
                       (g->thread_context_count + 1) * sizeof(*g->thread_contexts));
    surfaces = realloc(g->thread_surfaces,
                       (g->thread_context_count + 1) * sizeof(*g->thread_surfaces));
    if (contexts && surfaces) {
        g->thread_contexts = contexts;
        g->thread_surfaces = surfaces;
        g->thread_contexts[g->thread_context_count] = context;
        g->thread_surfaces[g->thread_context_count] = surface;
        g->thread_context_count++;
    } else {
        free(contexts);
        free(surfaces);
    }
    pthread_mutex_unlock(&g_thread_contexts_lock);
}

static EGLContext gl_thread_context(HostGles *g)
{
    EGLContext context;
    EGLSurface surface;

    if (g_thread_state.gles == g && g_thread_state.context != EGL_NO_CONTEXT)
        return g_thread_state.context;

    context = g->eglCreateContext(g->display, g->egl_config, g->context,
                                  g_thread_context_attribs);
    if (context == EGL_NO_CONTEXT) {
        fprintf(stderr, "vita2native: eglCreateContext(thread) failed: 0x%x\n",
                g->eglGetError());
        return EGL_NO_CONTEXT;
    }

    /*
     * Every context needs a drawing surface of its own: a pbuffer can only be
     * current on one context at a time, so the primary pbuffer cannot be
     * reused by other threads.
     */
    surface = g->eglCreatePbufferSurface(g->display, g->egl_config, g_thread_pbuffer_attribs);
    if (surface == EGL_NO_SURFACE) {
        fprintf(stderr, "vita2native: eglCreatePbufferSurface(thread) failed: 0x%x\n",
                g->eglGetError());
        g->eglDestroyContext(g->display, context);
        return EGL_NO_CONTEXT;
    }

    gl_remember_thread_context(g, context, surface);
    g_thread_state.gles = g;
    g_thread_state.context = context;
    g_thread_state.surface = surface;
    g_thread_state.bound = EGL_NO_SURFACE;
    g_thread_state.window = false;

    return context;
}

static int gl_thread_bind(HostGles *g, EGLSurface surface)
{
    EGLContext context = gl_thread_context(g);

    if (context == EGL_NO_CONTEXT)
        return -1;
    if (g_thread_state.bound == surface)
        return 0;
    if (!g->eglMakeCurrent(g->display, surface, surface, context)) {
        fprintf(stderr, "vita2native: eglMakeCurrent(thread) failed: 0x%x\n", g->eglGetError());
        return -1;
    }

    g_thread_state.bound = surface;
    return 0;
}

static void *open_first(const char *const *names)
{
    for (size_t i = 0; names[i]; i++) {
        void *handle = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (handle)
            return handle;
    }

    return NULL;
}

/*
 * SDL and the handheld launchers select the vendor EGL with
 * SDL_VIDEO_EGL_DRIVER (ArkOS sets it to the Mali blob). Honor the same hint so
 * our EGL and GLES libraries come from the same vendor; mixing Mesa EGL with a
 * vendor GLES library yields a context glGetString() cannot query.
 */
static void *open_egl(const char *const *names)
{
    const char *forced = getenv("SDL_VIDEO_EGL_DRIVER");

    if (forced && forced[0]) {
        void *handle = dlopen(forced, RTLD_NOW | RTLD_LOCAL);

        if (handle)
            return handle;
        fprintf(stderr, "vita2native: cannot load SDL_VIDEO_EGL_DRIVER='%s': %s\n", forced,
                dlerror());
    }

    return open_first(names);
}

#define LOAD_EGL(g, name)                                                                          \
    do {                                                                                           \
        *(void **)(&(g)->name) = dlsym((g)->egl_lib, #name);                                       \
        if (!(g)->name) {                                                                          \
            fprintf(stderr, "vita2native: EGL is missing %s\n", #name);                            \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

/* True when presentation should go straight to KMS instead of an X11 window. */
static bool gles_kms_requested(void)
{
    static int cached = -1;
    const char *driver;

    if (cached >= 0)
        return cached != 0;

    cached = 0;
    if (getenv("VITA2NATIVE_DIRECT_PRESENT")) {
        driver = getenv("SDL_VIDEODRIVER");
        if (getenv("VITA2NATIVE_KMS_PRESENT") || (driver && strstr(driver, "kmsdrm")))
            cached = 1;
        else if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY"))
            cached = 1;
    }

    return cached != 0;
}

#define LOAD_GL(g, type, name)                                                                     \
    do {                                                                                           \
        *(void **)(&(g)->name) = dlsym((g)->gles_lib, #name);                                      \
        if (!(g)->name) {                                                                          \
            fprintf(stderr, "vita2native: GLESv2 is missing %s\n", #name);                         \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

/*
 * Some handhelds ship a vendor GLES blob (ARM Bifrost) behind EGL 1.4 with no
 * surfaceless platform and no DRM render driver, so Mesa cannot create a
 * screen. The blob does accept a GBM device, so create one when libgbm is
 * available and offer it as a display candidate.
 */
static bool gles_open_gbm(HostGles *g)
{
    if (g->gbm_lib)
        return true;

    g->gbm_lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g->gbm_lib)
        g->gbm_lib = dlopen("libgbm.so", RTLD_NOW | RTLD_LOCAL);

    return g->gbm_lib != NULL;
}

static void *gxm_gbm_device_create(HostGles *g)
{
    typedef void *(*gbm_create_device_fn)(int fd);
    static const char *const drm_paths[] = {
        "/dev/dri/card0",
        "/dev/dri/renderD128",
    };
    gbm_create_device_fn create_device;

    if (g->gbm_device)
        return g->gbm_device;

    if (!gles_open_gbm(g))
        return NULL;

    create_device = (gbm_create_device_fn)dlsym(g->gbm_lib, "gbm_create_device");
    if (!create_device)
        return NULL;

    for (size_t i = 0; i < sizeof(drm_paths) / sizeof(drm_paths[0]); i++) {
        int fd = open(drm_paths[i], O_RDWR | O_CLOEXEC);

        if (fd < 0)
            continue;
        g->gbm_device = create_device(fd);
        if (g->gbm_device) {
            g->drm_fd = fd;
            g->owns_gbm_device = true;
            g->owns_drm_fd = true;
            return g->gbm_device;
        }
        close(fd);
    }

    return NULL;
}

/* ---------------------------------------------------------- KMS / GBM ---- */

/*
 * Direct presentation on KMS/DRM. libdrm and libgbm are loaded at runtime, so
 * the emulator still builds and runs on systems without either library. The
 * structures below mirror libdrm's public ABI; only the fields we use are
 * declared, in their original order (pointer fields follow the target ABI).
 */

#define V2N_DRM_MODE_CONNECTED 1
#define V2N_DRM_MODE_TYPE_PREFERRED (1u << 3)
#define V2N_DRM_MODE_PAGE_FLIP_EVENT 0x01u
#define V2N_DRM_EVENT_CONTEXT_VERSION 2
#define V2N_GBM_FORMAT_ARGB8888 0x34325241u
#define V2N_GBM_BO_USE_SCANOUT (1u << 0)
#define V2N_GBM_BO_USE_RENDERING (1u << 2)

typedef struct {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
} KmsModeInfo;

typedef struct {
    uint32_t crtc_id;
    uint32_t buffer_id;
    uint32_t x, y;
    uint32_t width, height;
    int mode_valid;
    KmsModeInfo mode;
    int gamma_size;
} KmsCrtc;

typedef struct {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
} KmsEncoder;

typedef struct {
    uint32_t connector_id;
    uint32_t encoder_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mmWidth, mmHeight;
    uint32_t subpixel;
    int count_modes;
    KmsModeInfo *modes;
    int count_props;
    uint32_t *props;
    uint64_t *prop_values;
    int count_encoders;
    uint32_t *encoders;
} KmsConnector;

typedef struct {
    int count_fbs;
    uint32_t *fbs;
    int count_crtcs;
    uint32_t *crtcs;
    int count_connectors;
    uint32_t *connectors;
    int count_encoders;
    uint32_t *encoders;
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
} KmsResources;

typedef struct {
    int version;
    void (*vblank_handler)(int, unsigned int, unsigned int, unsigned int, void *);
    void (*page_flip_handler)(int, unsigned int, unsigned int, unsigned int, void *);
    void (*page_flip_handler2)(int, unsigned int, unsigned int, unsigned int, unsigned int, void *);
    void (*sequence_handler)(int, uint64_t, uint64_t, uint64_t);
} KmsEventContext;

typedef union {
    void *ptr;
    int32_t s32;
    uint32_t u32;
    int64_t s64;
    uint64_t u64;
} KmsBoHandle;

typedef struct {
    void *lib;

    KmsResources *(*get_resources)(int fd);
    void (*free_resources)(KmsResources *resources);
    KmsConnector *(*get_connector)(int fd, uint32_t connector_id);
    void (*free_connector)(KmsConnector *connector);
    KmsEncoder *(*get_encoder)(int fd, uint32_t encoder_id);
    void (*free_encoder)(KmsEncoder *encoder);
    KmsCrtc *(*get_crtc)(int fd, uint32_t crtc_id);
    void (*free_crtc)(KmsCrtc *crtc);
    int (*set_crtc)(int fd, uint32_t crtc_id, uint32_t fb_id, uint32_t x, uint32_t y,
                    uint32_t *connectors, int count, KmsModeInfo *mode);
    int (*page_flip)(int fd, uint32_t crtc_id, uint32_t fb_id, uint32_t flags, void *user_data);
    int (*add_fb)(int fd, uint32_t width, uint32_t height, uint32_t depth, uint32_t bpp,
                  uint32_t pitch, uint32_t handle, uint32_t *fb_id);
    int (*rm_fb)(int fd, uint32_t fb_id);
    int (*handle_event)(int fd, KmsEventContext *event);
    int (*set_master)(int fd);
    int (*drop_master)(int fd);

    void *(*surface_create)(void *gbm, uint32_t width, uint32_t height, uint32_t format,
                            uint32_t flags);
    void (*surface_destroy)(void *surface);
    void *(*surface_lock_front_buffer)(void *surface);
    void (*surface_release_buffer)(void *surface, void *bo);
    uint32_t (*bo_get_width)(void *bo);
    uint32_t (*bo_get_height)(void *bo);
    uint32_t (*bo_get_stride)(void *bo);
    KmsBoHandle (*bo_get_handle)(void *bo);

    KmsCrtc *saved_crtc;
    KmsModeInfo mode;
    uint32_t connector_id;
    uint32_t crtc_id;
    bool mode_set;
    bool flip_pending;
    void *scanout_bo;
    uint32_t scanout_fb;
    void *retired_bo;
    uint32_t retired_fb;
} KmsState;

static void gles_kms_flip_handler(int fd, unsigned int frame, unsigned int sec,
                                  unsigned int usec, void *user_data)
{
    KmsState *k = user_data;

    (void)fd;
    (void)frame;
    (void)sec;
    (void)usec;
    if (k)
        k->flip_pending = false;
}

static int gles_kms_wait_flip(HostGles *g, KmsState *k)
{
    while (k->flip_pending) {
        struct pollfd pfd;
        KmsEventContext event;
        int ready;

        pfd.fd = g->drm_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        ready = poll(&pfd, 1, 2000);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (ready == 0)
            return -1;
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
            return -1;
        if (!(pfd.revents & POLLIN))
            continue;

        memset(&event, 0, sizeof(event));
        event.version = V2N_DRM_EVENT_CONTEXT_VERSION;
        event.page_flip_handler = gles_kms_flip_handler;
        k->handle_event(g->drm_fd, &event);
    }

    return 0;
}

static void gles_kms_release_retired(HostGles *g, KmsState *k)
{
    if (!k->retired_bo)
        return;

    k->rm_fb(g->drm_fd, k->retired_fb);
    k->surface_release_buffer(g->gbm_surface, k->retired_bo);
    k->retired_bo = NULL;
    k->retired_fb = 0;
}

static bool gles_kms_load(HostGles *g)
{
    KmsState *k;
    void *lib;

    if (g->kms_state)
        return true;

    k = calloc(1, sizeof(*k));
    if (!k)
        return false;

    lib = dlopen("libdrm.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        lib = dlopen("libdrm.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "vita2native: KMS: libdrm is not available\n");
        free(k);
        return false;
    }
    k->lib = lib;

#define LOAD_DRM(field, name)                                                                      \
    do {                                                                                           \
        *(void **)(&k->field) = dlsym(lib, name);                                                  \
        if (!k->field) {                                                                           \
            fprintf(stderr, "vita2native: KMS: libdrm is missing %s\n", name);                   \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

    LOAD_DRM(get_resources, "drmModeGetResources");
    LOAD_DRM(free_resources, "drmModeFreeResources");
    LOAD_DRM(get_connector, "drmModeGetConnector");
    LOAD_DRM(free_connector, "drmModeFreeConnector");
    LOAD_DRM(get_encoder, "drmModeGetEncoder");
    LOAD_DRM(free_encoder, "drmModeFreeEncoder");
    LOAD_DRM(get_crtc, "drmModeGetCrtc");
    LOAD_DRM(free_crtc, "drmModeFreeCrtc");
    LOAD_DRM(set_crtc, "drmModeSetCrtc");
    LOAD_DRM(page_flip, "drmModePageFlip");
    LOAD_DRM(add_fb, "drmModeAddFB");
    LOAD_DRM(rm_fb, "drmModeRmFB");
    LOAD_DRM(handle_event, "drmHandleEvent");
#undef LOAD_DRM

    /* Not fatal: on a KMSDRM desktop SDL may already hold master on another
       fd, in which case modesetting will report the real error later. */
    *(void **)(&k->set_master) = dlsym(lib, "drmSetMaster");
    *(void **)(&k->drop_master) = dlsym(lib, "drmDropMaster");

    if (!gles_open_gbm(g))
        goto fail;

#define LOAD_GBM(field, name)                                                                      \
    do {                                                                                           \
        *(void **)(&k->field) = dlsym(g->gbm_lib, name);                                           \
        if (!k->field) {                                                                           \
            fprintf(stderr, "vita2native: KMS: libgbm is missing %s\n", name);                    \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

    LOAD_GBM(surface_create, "gbm_surface_create");
    LOAD_GBM(surface_destroy, "gbm_surface_destroy");
    LOAD_GBM(surface_lock_front_buffer, "gbm_surface_lock_front_buffer");
    LOAD_GBM(surface_release_buffer, "gbm_surface_release_buffer");
    LOAD_GBM(bo_get_width, "gbm_bo_get_width");
    LOAD_GBM(bo_get_height, "gbm_bo_get_height");
    LOAD_GBM(bo_get_stride, "gbm_bo_get_stride");
    LOAD_GBM(bo_get_handle, "gbm_bo_get_handle");
#undef LOAD_GBM

    g->kms_state = k;
    return true;

fail:
    dlclose(lib);
    free(k);
    return false;
}

static bool gles_kms_setup_output(HostGles *g, KmsState *k, int want_width, int want_height)
{
    KmsResources *resources;
    KmsConnector *connector = NULL;
    KmsEncoder *encoder = NULL;
    KmsCrtc *crtc = NULL;
    uint32_t crtc_id = 0;
    int mode_index = -1;
    int i, j;

    resources = k->get_resources(g->drm_fd);
    if (!resources) {
        fprintf(stderr, "vita2native: KMS: drmModeGetResources failed\n");
        return false;
    }

    for (i = 0; i < resources->count_connectors && !connector; i++) {
        KmsConnector *candidate = k->get_connector(g->drm_fd, resources->connectors[i]);

        if (!candidate)
            continue;
        if (candidate->connection == V2N_DRM_MODE_CONNECTED && candidate->count_modes > 0)
            connector = candidate;
        else
            k->free_connector(candidate);
    }
    if (!connector) {
        fprintf(stderr, "vita2native: KMS: no connected display\n");
        k->free_resources(resources);
        return false;
    }

    if (connector->encoder_id)
        encoder = k->get_encoder(g->drm_fd, connector->encoder_id);

    if (!encoder) {
        for (i = 0; i < resources->count_encoders && !encoder; i++) {
            KmsEncoder *candidate = k->get_encoder(g->drm_fd, resources->encoders[i]);

            if (!candidate)
                continue;
            for (j = 0; j < connector->count_encoders; j++) {
                if (connector->encoders[j] == candidate->encoder_id)
                    break;
            }
            if (j < connector->count_encoders)
                encoder = candidate;
            else
                k->free_encoder(candidate);
        }
    }
    if (!encoder) {
        fprintf(stderr, "vita2native: KMS: no encoder for the connected display\n");
        k->free_connector(connector);
        k->free_resources(resources);
        return false;
    }

    if (encoder->crtc_id) {
        crtc = k->get_crtc(g->drm_fd, encoder->crtc_id);
        crtc_id = encoder->crtc_id;
    }
    if (!crtc) {
        for (i = 0; i < resources->count_crtcs && !crtc; i++) {
            if (encoder->possible_crtcs & (1u << i)) {
                crtc = k->get_crtc(g->drm_fd, resources->crtcs[i]);
                crtc_id = resources->crtcs[i];
            }
        }
    }
    if (!crtc) {
        fprintf(stderr, "vita2native: KMS: no CRTC for the connected display\n");
        k->free_encoder(encoder);
        k->free_connector(connector);
        k->free_resources(resources);
        return false;
    }

    /* An explicit request wins, then keep the current mode, then the panel's
       preferred mode, then the largest one. */
    if (want_width > 0 && want_height > 0) {
        for (i = 0; i < connector->count_modes; i++) {
            if (connector->modes[i].hdisplay == want_width &&
                connector->modes[i].vdisplay == want_height) {
                mode_index = i;
                break;
            }
        }
    }
    if (mode_index < 0 && crtc->mode_valid) {
        for (i = 0; i < connector->count_modes; i++) {
            if (memcmp(&connector->modes[i], &crtc->mode, sizeof(crtc->mode)) == 0) {
                mode_index = i;
                break;
            }
        }
    }
    if (mode_index < 0) {
        for (i = 0; i < connector->count_modes; i++) {
            if (connector->modes[i].type & V2N_DRM_MODE_TYPE_PREFERRED) {
                mode_index = i;
                break;
            }
        }
    }
    if (mode_index < 0) {
        int best_area = 0;

        for (i = 0; i < connector->count_modes; i++) {
            int area = (int)connector->modes[i].hdisplay * (int)connector->modes[i].vdisplay;

            if (area > best_area) {
                best_area = area;
                mode_index = i;
            }
        }
    }
    if (mode_index < 0) {
        fprintf(stderr, "vita2native: KMS: no usable display mode\n");
        k->free_crtc(crtc);
        k->free_encoder(encoder);
        k->free_connector(connector);
        k->free_resources(resources);
        return false;
    }

    k->mode = connector->modes[mode_index];
    k->crtc_id = crtc_id;
    k->connector_id = connector->connector_id;
    k->saved_crtc = crtc;
    g->kms_width = (int)k->mode.hdisplay;
    g->kms_height = (int)k->mode.vdisplay;
    g->kms_crtc_id = crtc_id;
    g->kms_drm_fd = g->drm_fd;

    fprintf(stderr, "vita2native: KMS display %s %ux%u@%u\n", k->mode.name, k->mode.hdisplay,
            k->mode.vdisplay, k->mode.vrefresh);

    k->free_encoder(encoder);
    k->free_connector(connector);
    k->free_resources(resources);
    return true;
}

static void gles_kms_shutdown(HostGles *g)
{
    KmsState *k = g->kms_state;

    if (!k)
        return;

    if (k->flip_pending)
        gles_kms_wait_flip(g, k);

    gles_kms_release_retired(g, k);

    if (k->saved_crtc) {
        if (g->drm_fd >= 0 && k->saved_crtc->mode_valid)
            (void)k->set_crtc(g->drm_fd, k->saved_crtc->crtc_id, k->saved_crtc->buffer_id,
                              k->saved_crtc->x, k->saved_crtc->y, &k->connector_id, 1,
                              &k->saved_crtc->mode);
        k->free_crtc(k->saved_crtc);
        k->saved_crtc = NULL;
    }

    /* The CRTC no longer scans this buffer out, so it can be released. */
    if (g->gbm_surface && k->scanout_bo) {
        k->rm_fb(g->drm_fd, k->scanout_fb);
        k->surface_release_buffer(g->gbm_surface, k->scanout_bo);
        k->scanout_bo = NULL;
        k->scanout_fb = 0;
    }

    if (g->gbm_surface) {
        k->surface_destroy(g->gbm_surface);
        g->gbm_surface = NULL;
    }

    if (k->drop_master && g->drm_fd >= 0)
        (void)k->drop_master(g->drm_fd);

    dlclose(k->lib);
    free(k);
    g->kms_state = NULL;
}

static void gles_destroy_impl(HostGles *g)
{
    if (!g)
        return;

    if (g->display != EGL_NO_DISPLAY) {
        if (g->glDeleteFramebuffers)
            g->glDeleteFramebuffers(1, &g->framebuffer);
        if (g->glDeleteTextures) {
            g->glDeleteTextures(1, &g->color_texture);
            g->glDeleteTextures(1, &g->present_texture);
            g->glDeleteTextures(1, &g->overlay_texture);
        }
        if (g->glDeleteProgram)
            g->glDeleteProgram(g->present_program);
        if (g->glDeleteVertexArrays)
            g->glDeleteVertexArrays(1, &g->present_vao);

        g->eglMakeCurrent(g->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        for (unsigned int i = 0; i < g->thread_context_count; i++) {
            if (g->thread_surfaces[i] != EGL_NO_SURFACE)
                g->eglDestroySurface(g->display, g->thread_surfaces[i]);
            if (g->thread_contexts[i] != EGL_NO_CONTEXT)
                g->eglDestroyContext(g->display, g->thread_contexts[i]);
        }
        free(g->thread_contexts);
        free(g->thread_surfaces);
        if (g->window_surface != EGL_NO_SURFACE)
            g->eglDestroySurface(g->display, g->window_surface);
        if (g->kms_surface != EGL_NO_SURFACE)
            g->eglDestroySurface(g->display, g->kms_surface);
        if (g->surface != EGL_NO_SURFACE)
            g->eglDestroySurface(g->display, g->surface);
        if (g->context != EGL_NO_CONTEXT)
            g->eglDestroyContext(g->display, g->context);
        if (g->eglReleaseThread)
            g->eglReleaseThread();
        g->eglTerminate(g->display);
    }

    /* Releases GBM buffers, restores the CRTC and unloads libdrm. */
    gles_kms_shutdown(g);

    if (g->gbm_device && g->owns_gbm_device) {
        typedef void (*gbm_device_destroy_fn)(void *device);
        gbm_device_destroy_fn destroy_device =
            g->gbm_lib ? (gbm_device_destroy_fn)dlsym(g->gbm_lib, "gbm_device_destroy") : NULL;

        if (destroy_device)
            destroy_device(g->gbm_device);
    }
    if (g->drm_fd >= 0 && g->owns_drm_fd)
        close(g->drm_fd);
    if (g->gbm_lib)
        dlclose(g->gbm_lib);

    if (g->egl_lib)
        dlclose(g->egl_lib);
    if (g->gles_lib)
        dlclose(g->gles_lib);

    free(g);
}

HostGpuDevice host_gfx_gl_create(const HostGpuConfig *config)
{
    static const char *const egl_names[] = {
        "libEGL.so.1", "libEGL.so", "libEGL.dylib", NULL,
    };
    static const char *const gles_names[] = {
        "libGLESv2.so.2", "libGLESv2.so", "libGLESv2.dylib", NULL,
    };
    static const EGLint config_attribs_window[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE,
    };
    static const EGLint config_attribs_pbuffer[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE,
    };
    static const EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE,
    };
    static const EGLint pbuffer_attribs[] = {
        EGL_WIDTH,  1,
        EGL_HEIGHT, 1,
        EGL_NONE,
    };
    HostGles *g = calloc(1, sizeof(*g));
    HostGpuDevice device = NULL;
    EGLConfig egl_config = NULL;
    EGLint num_configs = 0;
    EGLint egl_major = 0, egl_minor = 0;
    const char *version;
    const char *renderer;

    if (!g)
        return NULL;

    g->display = EGL_NO_DISPLAY;
    g->context = EGL_NO_CONTEXT;
    g->surface = EGL_NO_SURFACE;
    g->window_surface = EGL_NO_SURFACE;
    g->width = config->width > 0 ? config->width : 1;
    g->height = config->height > 0 ? config->height : 1;
    g->drm_fd = -1;

    g->egl_lib = open_egl(egl_names);
    if (!g->egl_lib)
        goto fail;

    LOAD_EGL(g, eglGetDisplay);
    *(void **)(&g->eglGetPlatformDisplay) = dlsym(g->egl_lib, "eglGetPlatformDisplay");
    LOAD_EGL(g, eglGetProcAddress);
    g->eglGetPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)g->eglGetProcAddress("eglGetPlatformDisplayEXT");
    LOAD_EGL(g, eglInitialize);
    LOAD_EGL(g, eglTerminate);
    LOAD_EGL(g, eglGetError);
    LOAD_EGL(g, eglBindAPI);
    LOAD_EGL(g, eglChooseConfig);
    LOAD_EGL(g, eglCreateContext);
    LOAD_EGL(g, eglDestroyContext);
    LOAD_EGL(g, eglCreatePbufferSurface);
    LOAD_EGL(g, eglCreateWindowSurface);
    LOAD_EGL(g, eglSwapBuffers);
    LOAD_EGL(g, eglQuerySurface);
    LOAD_EGL(g, eglDestroySurface);
    LOAD_EGL(g, eglMakeCurrent);
    LOAD_EGL(g, eglReleaseThread);
    *(void **)(&g->eglSwapInterval) = dlsym(g->egl_lib, "eglSwapInterval");

    /*
     * A window build needs a platform display that can create window surfaces,
     * so try the default (X11/Wayland) display first. Headless builds prefer
     * the surfaceless platform, which needs no display server. In both cases
     * the other options are fallbacks. KMS direct presentation needs the GBM
     * platform display first, because the scanout surface and the GXM textures
     * must live on the same EGL display.
     */
    {
        EGLDisplay candidates[8];
        bool candidate_is_gbm[8] = { false };
        int num_candidates = 0;
        bool initialized = false;
        bool prefer_gbm = gles_kms_requested();

#define GXM_ADD_CANDIDATE(display, is_gbm)                                                         \
    do {                                                                                           \
        candidates[num_candidates] = (display);                                                    \
        candidate_is_gbm[num_candidates] = (is_gbm);                                               \
        num_candidates++;                                                                          \
    } while (0)

#define GXM_ADD_SURFACELESS()                                                                      \
    do {                                                                                           \
        if (g->eglGetPlatformDisplay)                                                              \
            GXM_ADD_CANDIDATE(g->eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA,              \
                                                        EGL_DEFAULT_DISPLAY, NULL),                \
                              false);                                                              \
        if (g->eglGetPlatformDisplayEXT)                                                           \
            GXM_ADD_CANDIDATE(g->eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,            \
                                                           EGL_DEFAULT_DISPLAY, NULL),             \
                              false);                                                              \
    } while (0)

#define GXM_ADD_GBM()                                                                              \
    do {                                                                                           \
        void *gbm = gxm_gbm_device_create(g);                                                      \
        if (gbm) {                                                                                 \
            if (g->eglGetPlatformDisplayEXT)                                                       \
                GXM_ADD_CANDIDATE(g->eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR,                \
                                                               (EGLNativeDisplayType)gbm, NULL),   \
                                  true);                                                       \
            GXM_ADD_CANDIDATE(g->eglGetDisplay((EGLNativeDisplayType)gbm), true);                  \
        }                                                                                          \
    } while (0)

        /* Vendor blobs on handhelds need a GBM device; Mesa ignores this when
           one of the other platforms already worked. */
        if (prefer_gbm)
            GXM_ADD_GBM();

        if (config->headless) {
            GXM_ADD_SURFACELESS();
            GXM_ADD_CANDIDATE(g->eglGetDisplay(EGL_DEFAULT_DISPLAY), false);
        } else {
            GXM_ADD_CANDIDATE(g->eglGetDisplay(EGL_DEFAULT_DISPLAY), false);
            GXM_ADD_SURFACELESS();
        }

        if (!prefer_gbm)
            GXM_ADD_GBM();

#undef GXM_ADD_GBM
#undef GXM_ADD_SURFACELESS
#undef GXM_ADD_CANDIDATE

        for (int i = 0; i < num_candidates && !initialized; i++) {
            if (candidates[i] == EGL_NO_DISPLAY)
                continue;

            if (g->eglInitialize(candidates[i], &egl_major, &egl_minor)) {
                g->display = candidates[i];
                g->display_is_gbm = candidate_is_gbm[i];
                initialized = true;
            }
        }

        if (!initialized) {
            fprintf(stderr, "vita2native: no usable EGL display: 0x%x\n", g->eglGetError());
            goto fail;
        }
    }

    if (!g->eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "vita2native: eglBindAPI(EGL_OPENGL_ES_API) failed\n");
        goto fail;
    }

    if (!config->headless &&
        g->eglChooseConfig(g->display, config_attribs_window, &egl_config, 1, &num_configs) &&
        num_configs >= 1) {
        g->window_capable = true;
    } else if (!g->eglChooseConfig(g->display, config_attribs_pbuffer, &egl_config, 1,
                                   &num_configs) ||
               num_configs < 1) {
        fprintf(stderr, "vita2native: no EGL config with GLES 3.x\n");
        goto fail;
    }

    g->egl_config = egl_config;

    g->context = g->eglCreateContext(g->display, egl_config, EGL_NO_CONTEXT, context_attribs);
    if (g->context == EGL_NO_CONTEXT) {
        fprintf(stderr, "vita2native: eglCreateContext failed: 0x%x\n", g->eglGetError());
        goto fail;
    }

    g->surface = g->eglCreatePbufferSurface(g->display, egl_config, pbuffer_attribs);
    if (g->surface == EGL_NO_SURFACE) {
        fprintf(stderr, "vita2native: eglCreatePbufferSurface failed: 0x%x\n", g->eglGetError());
        goto fail;
    }

    if (!g->eglMakeCurrent(g->display, g->surface, g->surface, g->context)) {
        fprintf(stderr, "vita2native: eglMakeCurrent failed: 0x%x\n", g->eglGetError());
        goto fail;
    }

    g_thread_state.gles = g;
    g_thread_state.context = g->context;
    g_thread_state.surface = g->surface;
    g_thread_state.bound = g->surface;
    g_thread_state.window = false;

    g->gles_lib = open_first(gles_names);
    if (!g->gles_lib)
        goto fail;

#define HOST_GL_LOAD(type, name) LOAD_GL(g, type, name);
    HOST_GL_FUNCTIONS(HOST_GL_LOAD)
#undef HOST_GL_LOAD

    version = (const char *)g->glGetString(GL_VERSION);
    renderer = (const char *)g->glGetString(GL_RENDERER);

    if (getenv("VITA2NATIVE_GL_INFO")) {
        const char *extensions = (const char *)g->glGetString(GL_EXTENSIONS);

        fprintf(stderr, "vita2native: GL extensions: %s\n", extensions ? extensions : "(none)");
    }
    {
        const char *number = version ? strstr(version, "OpenGL ES ") : NULL;
        number = number ? number + strlen("OpenGL ES ") : version;
        if (!number || atoi(number) < 3) {
            fprintf(stderr, "vita2native: GLES 3.x required, got '%s'\n",
                    version ? version : "?");
            goto fail;
        }

        /* Offscreen color target used by the GXM backend. */
        g->glGenTextures(1, &g->color_texture);
        g->glBindTexture(GL_TEXTURE_2D, g->color_texture);
        g->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g->width, g->height, 0, GL_RGBA,
                        GL_UNSIGNED_BYTE, NULL);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        g->glBindTexture(GL_TEXTURE_2D, 0);

        g->glGenFramebuffers(1, &g->framebuffer);
        g->glBindFramebuffer(GL_FRAMEBUFFER, g->framebuffer);
        g->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                  g->color_texture, 0);
        if (g->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "vita2native: incomplete GLES framebuffer\n");
            goto fail;
        }

        g->glViewport(0, 0, g->width, g->height);

        device = calloc(1, sizeof(*device));
        if (!device)
            goto fail;

        device->backend = HOST_GPU_BACKEND_GLES;
        device->impl = g;
        snprintf(device->description, sizeof(device->description), "GLES %s (%s, EGL %d.%d)",
                 number, renderer ? renderer : "unknown renderer", egl_major, egl_minor);
    }

    return device;

fail:
    gles_destroy_impl(g);
    return NULL;
}

HostGles *host_gpu_get_gles(HostGpuDevice device)
{
    if (!device || device->backend != HOST_GPU_BACKEND_GLES)
        return NULL;

    return device->impl;
}

void host_gfx_gl_destroy(HostGpuDevice device)
{
    gles_destroy_impl(device->impl);
    free(device);
}

void host_gfx_gl_begin_frame(HostGpuDevice device)
{
    HostGles *gl = device->impl;

    gl->glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);
    gl->glViewport(0, 0, gl->width, gl->height);
}

void host_gfx_gl_end_frame(HostGpuDevice device)
{
    HostGles *gl = device->impl;

    gl->glFinish();
}

void host_gfx_gl_clear(HostGpuDevice device, float r, float g, float b, float a)
{
    HostGles *gl = device->impl;

    gl->glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);
    gl->glClearColor(r, g, b, a);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glFinish();
}

int host_gfx_gl_read_pixels(HostGpuDevice device, int x, int y, int width, int height, void *rgba,
                            size_t size)
{
    HostGles *gl = device->impl;
    size_t needed;

    if (!rgba || width <= 0 || height <= 0 || x < 0 || y < 0)
        return -1;
    if (x + width > gl->width || y + height > gl->height)
        return -1;

    needed = (size_t)width * (size_t)height * 4;
    if (size < needed)
        return -1;

    gl->glBindFramebuffer(GL_FRAMEBUFFER, gl->framebuffer);

    while (gl->glGetError() != GL_NO_ERROR) {
        /* Drain stale errors so the readback result is meaningful. */
    }

    gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
    gl->glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    gl->glFinish();

    return gl->glGetError() == GL_NO_ERROR ? 0 : -1;
}

void host_gfx_gl_finish(HostGpuDevice device)
{
    HostGles *gl = device->impl;

    gl->glFinish();
}

/* ------------------------------------------------------------- presentation */

static GLuint gles_compile_present_shader(HostGles *g, GLenum stage, const char *source)
{
    GLuint shader = g->glCreateShader(stage);
    GLint ok = 0;

    g->glShaderSource(shader, 1, &source, NULL);
    g->glCompileShader(shader);
    g->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];

        g->glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "vita2native: present %s shader failed: %s\n",
                stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        g->glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static bool gles_create_present_program(HostGles *g)
{
    static const char vertex_source[] =
        "#version 300 es\n"
        "out vec2 v_uv;\n"
        "void main() {\n"
        "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "    v_uv = p;\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char fragment_source[] =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "uniform float u_flip;\n"
        "out vec4 o_color;\n"
        "void main() {\n"
        "    vec2 uv = v_uv;\n"
        "    if (u_flip > 0.5) uv.y = 1.0 - uv.y;\n"
        "    o_color = texture(u_tex, uv);\n"
        "}\n";
    GLuint vertex = gles_compile_present_shader(g, GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = 0;
    GLuint program = 0;
    GLint ok = 0;

    if (!vertex)
        return false;

    fragment = gles_compile_present_shader(g, GL_FRAGMENT_SHADER, fragment_source);
    if (!fragment) {
        g->glDeleteShader(vertex);
        return false;
    }

    program = g->glCreateProgram();
    g->glAttachShader(program, vertex);
    g->glAttachShader(program, fragment);
    g->glLinkProgram(program);
    g->glGetProgramiv(program, GL_LINK_STATUS, &ok);
    g->glDeleteShader(vertex);
    g->glDeleteShader(fragment);
    if (!ok) {
        char log[512];

        g->glGetProgramInfoLog(program, sizeof(log), NULL, log);
        fprintf(stderr, "vita2native: present program link failed: %s\n", log);
        g->glDeleteProgram(program);
        return false;
    }

    g->glUseProgram(program);
    g->glUniform1i(g->glGetUniformLocation(program, "u_tex"), 0);
    g->present_flip_uniform = g->glGetUniformLocation(program, "u_flip");
    g->glUniform1f(g->present_flip_uniform, 1.0f);
    g->glUseProgram(0);

    if (!g->present_vao)
        g->glGenVertexArrays(1, &g->present_vao);

    g->present_program = program;
    return true;
}

static void gles_kms_recycle(HostGles *g)
{
    KmsState *k = g->kms_state;

    if (!k)
        return;

    if (k->flip_pending && gles_kms_wait_flip(g, k) != 0) {
        fprintf(stderr, "vita2native: KMS: page flip wait failed\n");
        k->flip_pending = false;
    }

    /* The previous front buffer is no longer scanned out and can go back to
       the GBM swap chain. The buffer being scanned out right now must stay
       locked (and its fb must stay registered) until a newer flip completes. */
    gles_kms_release_retired(g, k);
}

static int gles_kms_queue_flip(HostGles *g)
{
    KmsState *k = g->kms_state;
    void *bo;
    uint32_t fb_id = 0;
    uint32_t handle;
    uint32_t stride;
    int ret;

    if (!k || !g->gbm_surface || g->drm_fd < 0)
        return -1;

    bo = k->surface_lock_front_buffer(g->gbm_surface);
    if (!bo) {
        fprintf(stderr, "vita2native: KMS: gbm_surface_lock_front_buffer failed\n");
        return -1;
    }

    handle = k->bo_get_handle(bo).u32;
    stride = k->bo_get_stride(bo);
    ret = k->add_fb(g->drm_fd, k->bo_get_width(bo), k->bo_get_height(bo), 24, 32, stride, handle,
                    &fb_id);
    if (ret != 0) {
        fprintf(stderr, "vita2native: KMS: drmModeAddFB failed (%d)\n", ret);
        k->surface_release_buffer(g->gbm_surface, bo);
        return -1;
    }

    if (!k->mode_set) {
        /* A page flip needs a CRTC that is already configured. */
        ret = k->set_crtc(g->drm_fd, k->crtc_id, fb_id, 0, 0, &k->connector_id, 1, &k->mode);
        if (ret != 0) {
            fprintf(stderr, "vita2native: KMS: drmModeSetCrtc failed (%d)\n", ret);
            k->rm_fb(g->drm_fd, fb_id);
            k->surface_release_buffer(g->gbm_surface, bo);
            return -1;
        }
        k->mode_set = true;
    } else {
        ret = k->page_flip(g->drm_fd, k->crtc_id, fb_id, V2N_DRM_MODE_PAGE_FLIP_EVENT, k);
        if (ret != 0) {
            fprintf(stderr, "vita2native: KMS: drmModePageFlip failed (%d)\n", ret);
            k->rm_fb(g->drm_fd, fb_id);
            k->surface_release_buffer(g->gbm_surface, bo);
            return -1;
        }
        k->flip_pending = true;
    }

    k->retired_bo = k->scanout_bo;
    k->retired_fb = k->scanout_fb;
    k->scanout_bo = bo;
    k->scanout_fb = fb_id;
    return 0;
}

int host_gfx_gl_attach_kms(HostGpuDevice device, int drm_fd, int width, int height)
{
    HostGles *g = device->impl;
    KmsState *k;
    EGLSurface surface;

    (void)drm_fd;

    if (!g || !g->gbm_device || !g->display_is_gbm || !g->eglCreateWindowSurface)
        return -1;

    if (!g->window_capable) {
        fprintf(stderr, "vita2native: KMS: the EGL display has no window config\n");
        return -1;
    }

    if (g->kms_surface != EGL_NO_SURFACE)
        return (g_window_thread_valid && !pthread_equal(pthread_self(), g_window_thread)) ? -1 : 0;

    if (!gles_kms_load(g))
        return -1;
    k = g->kms_state;

    if (k->crtc_id == 0 && !gles_kms_setup_output(g, k, width, height))
        return -1;

    if (k->set_master) {
        int ret = k->set_master(g->drm_fd);

        if (ret != 0) {
            fprintf(stderr, "vita2native: KMS: drmSetMaster failed (%d): another DRM master?\n",
                    ret);
            return -1;
        }
    }

    if (!g->gbm_surface) {
        g->gbm_surface =
            k->surface_create(g->gbm_device, (uint32_t)g->kms_width, (uint32_t)g->kms_height,
                              V2N_GBM_FORMAT_ARGB8888, V2N_GBM_BO_USE_SCANOUT |
                                                       V2N_GBM_BO_USE_RENDERING);
        if (!g->gbm_surface) {
            fprintf(stderr, "vita2native: KMS: gbm_surface_create(%dx%d) failed\n", g->kms_width,
                    g->kms_height);
            return -1;
        }
    }

    while (g->glGetError() != GL_NO_ERROR) {
    }

    surface = g->eglCreateWindowSurface(g->display, g->egl_config,
                                        (EGLNativeWindowType)g->gbm_surface, NULL);
    if (surface == EGL_NO_SURFACE) {
        fprintf(stderr, "vita2native: KMS: eglCreateWindowSurface failed: 0x%x\n",
                g->eglGetError());
        k->surface_destroy(g->gbm_surface);
        g->gbm_surface = NULL;
        return -1;
    }

    if (gl_thread_bind(g, surface) != 0) {
        fprintf(stderr, "vita2native: KMS: eglMakeCurrent failed: 0x%x\n", g->eglGetError());
        g->eglDestroySurface(g->display, surface);
        k->surface_destroy(g->gbm_surface);
        g->gbm_surface = NULL;
        return -1;
    }

    /* Page flips provide the pacing; blocking in eglSwapBuffers would delay
       the guest thread that hands frames over. */
    if (g->eglSwapInterval)
        g->eglSwapInterval(g->display, 0);

    g_thread_state.window = true;
    g->kms_surface = surface;
    g_window_thread = pthread_self();
    g_window_thread_valid = true;

    fprintf(stderr, "vita2native: KMS presentation enabled (%dx%d, crtc %u, connector %u)\n",
            g->kms_width, g->kms_height, k->crtc_id, k->connector_id);
    return 0;
}

int host_gfx_gl_attach_window(HostGpuDevice device, void *native_window)
{
    HostGles *g = device->impl;
    EGLSurface surface;

    if (!g || !native_window || !g->window_capable || !g->eglCreateWindowSurface)
        return -1;

    /* An EGL surface can only be current on one context at a time. */
    if (g->window_surface != EGL_NO_SURFACE && g_window_thread_valid)
        return pthread_equal(pthread_self(), g_window_thread) ? 0 : -1;

    while (g->glGetError() != GL_NO_ERROR) {
    }

    surface = g->eglCreateWindowSurface(g->display, g->egl_config,
                                        (EGLNativeWindowType)native_window, NULL);
    if (surface == EGL_NO_SURFACE) {
        fprintf(stderr, "vita2native: eglCreateWindowSurface failed: 0x%x\n", g->eglGetError());
        return -1;
    }

    if (gl_thread_bind(g, surface) != 0) {
        fprintf(stderr, "vita2native: eglMakeCurrent(window) failed: 0x%x\n", g->eglGetError());
        g->eglDestroySurface(g->display, surface);
        return -1;
    }

    g_thread_state.window = true;

    if (g->window_surface != EGL_NO_SURFACE)
        g->eglDestroySurface(g->display, g->window_surface);
    g->window_surface = surface;
    g_window_thread = pthread_self();
    g_window_thread_valid = true;

    if (g->eglSwapInterval)
        g->eglSwapInterval(g->display, 1);

    return 0;
}

void host_gfx_gl_thread_enter(HostGpuDevice device)
{
    HostGles *g = device ? device->impl : NULL;

    if (!g || g->display == EGL_NO_DISPLAY)
        return;

    if (gl_thread_context(g) == EGL_NO_CONTEXT)
        return;

    if (g_thread_state.window && g->window_surface != EGL_NO_SURFACE)
        gl_thread_bind(g, g->window_surface);
    else if (g_thread_state.window && g->kms_surface != EGL_NO_SURFACE)
        gl_thread_bind(g, g->kms_surface);
    else
        gl_thread_bind(g, g_thread_state.surface);
}

void host_gfx_gl_detach_window(HostGpuDevice device)
{
    HostGles *g = device->impl;

    if (!g)
        return;

    if (g->kms_surface == EGL_NO_SURFACE && g->window_surface == EGL_NO_SURFACE)
        return;

    if (g_window_thread_valid && !pthread_equal(pthread_self(), g_window_thread))
        return;

    gl_thread_bind(g, g_thread_state.surface);
    g_thread_state.window = false;

    if (g->kms_surface != EGL_NO_SURFACE) {
        g->eglDestroySurface(g->display, g->kms_surface);
        g->kms_surface = EGL_NO_SURFACE;
        gles_kms_shutdown(g);
    }

    if (g->window_surface != EGL_NO_SURFACE) {
        g->eglDestroySurface(g->display, g->window_surface);
        g->window_surface = EGL_NO_SURFACE;
    }

    g_window_thread_valid = false;
}

void host_gfx_gl_present_framebuffer(HostGpuDevice device, const void *rgba, int width, int height,
                                     int stride_in_pixels)
{
    HostGles *g = device->impl;
    EGLint window_width = 0, window_height = 0;
    GLint viewport_x, viewport_y, viewport_width, viewport_height;

    if (!g || g->window_surface == EGL_NO_SURFACE)
        return;

    if (!g->eglQuerySurface(g->display, g->window_surface, EGL_WIDTH, &window_width) ||
        !g->eglQuerySurface(g->display, g->window_surface, EGL_HEIGHT, &window_height) ||
        window_width <= 0 || window_height <= 0)
        return;

    if (!g->present_program && !gles_create_present_program(g))
        return;

    g->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g->glViewport(0, 0, window_width, window_height);
    g->glDisable(GL_SCISSOR_TEST);
    g->glDisable(GL_DEPTH_TEST);
    g->glDisable(GL_STENCIL_TEST);
    g->glDisable(GL_BLEND);
    g->glDisable(GL_CULL_FACE);
    g->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    g->glClear(GL_COLOR_BUFFER_BIT);

    if (rgba && width > 0 && height > 0 && stride_in_pixels >= width) {
        if (!g->present_texture)
            g->glGenTextures(1, &g->present_texture);

        g->glActiveTexture(GL_TEXTURE0);
        g->glBindTexture(GL_TEXTURE_2D, g->present_texture);
        g->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        g->glPixelStorei(GL_UNPACK_ROW_LENGTH, stride_in_pixels);

        if (g->present_tex_width != width || g->present_tex_height != height) {
            g->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                            GL_UNSIGNED_BYTE, rgba);
            g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            g->present_tex_width = width;
            g->present_tex_height = height;
        } else {
            g->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                               rgba);
        }

        g->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

        /* Letterbox the guest framebuffer, preserving its aspect ratio. */
        {
            double scale_x = (double)window_width / width;
            double scale_y = (double)window_height / height;
            double scale = scale_x < scale_y ? scale_x : scale_y;

            viewport_width = (GLint)(width * scale + 0.5);
            viewport_height = (GLint)(height * scale + 0.5);
            viewport_x = (window_width - viewport_width) / 2;
            viewport_y = (window_height - viewport_height) / 2;
        }

        g->glViewport(viewport_x, viewport_y, viewport_width, viewport_height);
        g->glUseProgram(g->present_program);
        g->glUniform1f(g->present_flip_uniform, 1.0f);
        g->glBindVertexArray(g->present_vao);
        g->glDrawArrays(GL_TRIANGLES, 0, 3);
        g->glBindVertexArray(0);
        g->glUseProgram(0);
        g->glBindTexture(GL_TEXTURE_2D, 0);
    }

    g->eglSwapBuffers(g->display, g->window_surface);
}

void host_gfx_gl_set_overlay(HostGpuDevice device, const void *rgba, int width, int height)
{
    HostGles *g = device->impl;

    if (!g)
        return;

    if (!rgba || width <= 0 || height <= 0) {
        g->overlay_width = 0;
        g->overlay_height = 0;
        return;
    }

    while (g->glGetError() != GL_NO_ERROR) {
    }

    g->glActiveTexture(GL_TEXTURE0);
    if (!g->overlay_texture)
        g->glGenTextures(1, &g->overlay_texture);
    g->glBindTexture(GL_TEXTURE_2D, g->overlay_texture);
    g->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    if (g->overlay_width != width || g->overlay_height != height) {
        g->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                        rgba);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        g->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }

    g->glBindTexture(GL_TEXTURE_2D, 0);
    g->overlay_width = width;
    g->overlay_height = height;
}

void host_gfx_gl_present_texture(HostGpuDevice device, unsigned int texture, int width, int height)
{
    HostGles *g = device->impl;
    EGLSurface surface;
    EGLint window_width = 0, window_height = 0;
    bool kms = false;

    if (!g || !texture || width <= 0 || height <= 0)
        return;

    if (g->window_surface != EGL_NO_SURFACE) {
        surface = g->window_surface;
        if (g_window_thread_valid && !pthread_equal(pthread_self(), g_window_thread))
            return;
        if (!g->eglQuerySurface(g->display, surface, EGL_WIDTH, &window_width) ||
            !g->eglQuerySurface(g->display, surface, EGL_HEIGHT, &window_height) ||
            window_width <= 0 || window_height <= 0)
            return;
    } else if (g->kms_surface != EGL_NO_SURFACE) {
        surface = g->kms_surface;
        if (g_window_thread_valid && !pthread_equal(pthread_self(), g_window_thread))
            return;
        window_width = g->kms_width;
        window_height = g->kms_height;
        kms = true;
    } else {
        return;
    }

    if (!g->present_program && !gles_create_present_program(g))
        return;

    if (kms)
        gles_kms_recycle(g);

    g->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    g->glViewport(0, 0, window_width, window_height);
    g->glDisable(GL_SCISSOR_TEST);
    g->glDisable(GL_DEPTH_TEST);
    g->glDisable(GL_STENCIL_TEST);
    g->glDisable(GL_BLEND);
    g->glDisable(GL_CULL_FACE);
    g->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    g->glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    g->glClear(GL_COLOR_BUFFER_BIT);

    while (g->glGetError() != GL_NO_ERROR) {
    }

    {
        double scale_x = (double)window_width / width;
        double scale_y = (double)window_height / height;
        double scale = scale_x < scale_y ? scale_x : scale_y;
        GLint viewport_width = (GLint)(width * scale + 0.5);
        GLint viewport_height = (GLint)(height * scale + 0.5);
        GLint viewport_x = (window_width - viewport_width) / 2;
        GLint viewport_y = (window_height - viewport_height) / 2;

        g->glViewport(viewport_x, viewport_y, viewport_width, viewport_height);
        g->glUseProgram(g->present_program);
        g->glUniform1f(g->present_flip_uniform, 0.0f);
        g->glActiveTexture(GL_TEXTURE0);
        g->glBindTexture(GL_TEXTURE_2D, texture);
        g->glBindVertexArray(g->present_vao);
        g->glDrawArrays(GL_TRIANGLES, 0, 3);
        g->glBindVertexArray(0);
        g->glBindTexture(GL_TEXTURE_2D, 0);
        g->glUseProgram(0);
    }

    if (g->overlay_texture && g->overlay_width > 0 && g->overlay_height > 0) {
        double scale_x = (double)window_width / width;
        double scale_y = (double)window_height / height;
        double scale = scale_x < scale_y ? scale_x : scale_y;
        int viewport_width = (int)(width * scale + 0.5);
        int viewport_height = (int)(height * scale + 0.5);
        int viewport_x = (window_width - viewport_width) / 2;
        int viewport_y = (window_height - viewport_height) / 2;
        int margin = (int)(8.0 * scale + 0.5);
        int overlay_width = (int)(g->overlay_width * scale + 0.5);
        int overlay_height = (int)(g->overlay_height * scale + 0.5);
        int overlay_x;
        int overlay_y;

        if (margin < 2)
            margin = 2;
        overlay_x = viewport_x + viewport_width - overlay_width - margin;
        overlay_y = viewport_y + viewport_height - overlay_height - margin;

        if (overlay_x < viewport_x)
            overlay_x = viewport_x;
        if (overlay_y < viewport_y)
            overlay_y = viewport_y;

        g->glViewport(overlay_x, overlay_y, overlay_width, overlay_height);
        g->glUseProgram(g->present_program);
        g->glUniform1f(g->present_flip_uniform, 1.0f);
        g->glActiveTexture(GL_TEXTURE0);
        g->glBindTexture(GL_TEXTURE_2D, g->overlay_texture);
        g->glBindVertexArray(g->present_vao);
        g->glDrawArrays(GL_TRIANGLES, 0, 3);
        g->glBindVertexArray(0);
        g->glBindTexture(GL_TEXTURE_2D, 0);
        g->glUseProgram(0);
    }

    if (getenv("VITA2NATIVE_DIRECT_PRESENT_DEBUG")) {
        static unsigned int logged;
        GLenum error = g->glGetError();

        if (logged < 20) {
            fprintf(stderr, "SceGxm present texture %u %dx%d: window %dx%d error 0x%x\n",
                    texture, width, height, (int)window_width, (int)window_height, error);
            logged++;
        }
    }

    /* The presenting context is not the rendering one: finish the blit before
       the guest is allowed to overwrite the source texture. */
    if (kms)
        g->glFinish();

    if (!g->eglSwapBuffers(g->display, surface)) {
        fprintf(stderr, "vita2native: eglSwapBuffers failed: 0x%x\n", g->eglGetError());
        return;
    }

    if (kms)
        (void)gles_kms_queue_flip(g);
}
