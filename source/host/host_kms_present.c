/*
 * KMS/DRM scanout for the EGL presenter.
 *
 * Only the flip side lives here. SDL opened the card, took DRM master and
 * configured the CRTC before this runs; this module adds the one thing SDL's
 * SysWMinfo cannot expose - a gbm_surface for EGL to render into - and turns
 * each finished frame into a page flip. It never calls drmSetMaster, never
 * searches for connectors, encoders or CRTCs, and never sets a mode.
 *
 * libdrm and libgbm are dlopen()ed and the few structures we touch are
 * declared below in their public ABI order, so the emulator still builds and
 * runs on a host that has neither header nor library (macOS, CI).
 */
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host_kms_present.h"

#define KMS_FLIP_TIMEOUT_MS 2000

#define V2N_DRM_MODE_CONNECTED 1
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

struct HostKmsPresent {
    void *drm_lib;
    void *gbm_lib;

    KmsResources *(*get_resources)(int fd);
    void (*free_resources)(KmsResources *resources);
    KmsConnector *(*get_connector)(int fd, uint32_t connector_id);
    void (*free_connector)(KmsConnector *connector);
    KmsCrtc *(*get_crtc)(int fd, uint32_t crtc_id);
    void (*free_crtc)(KmsCrtc *crtc);
    int (*page_flip)(int fd, uint32_t crtc_id, uint32_t fb_id, uint32_t flags, void *user_data);
    int (*add_fb)(int fd, uint32_t width, uint32_t height, uint32_t depth, uint32_t bpp,
                  uint32_t pitch, uint32_t handle, uint32_t *fb_id);
    int (*rm_fb)(int fd, uint32_t fb_id);
    int (*handle_event)(int fd, KmsEventContext *event);

    void *(*surface_create)(void *gbm, uint32_t width, uint32_t height, uint32_t format,
                            uint32_t flags);
    void (*surface_destroy)(void *surface);
    void *(*surface_lock_front_buffer)(void *surface);
    void (*surface_release_buffer)(void *surface, void *bo);
    uint32_t (*bo_get_width)(void *bo);
    uint32_t (*bo_get_height)(void *bo);
    uint32_t (*bo_get_stride)(void *bo);
    KmsBoHandle (*bo_get_handle)(void *bo);

    int drm_fd;
    uint32_t crtc_id;
    uint32_t connector_id;
    KmsModeInfo mode;
    bool mode_valid;

    void *surface;

    void *scanout_bo;
    uint32_t scanout_fb;
    void *retired_bo;
    uint32_t retired_fb;

    bool flip_pending;
};

static void kms_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec,
                             void *user_data)
{
    HostKmsPresent *kms = user_data;

    (void)fd;
    (void)frame;
    (void)sec;
    (void)usec;
    if (kms)
        kms->flip_pending = false;
}

void host_kms_present_wait_idle(HostKmsPresent *kms)
{
    if (!kms)
        return;

    while (kms->flip_pending) {
        struct pollfd pfd;
        KmsEventContext event;
        int ready;

        pfd.fd = kms->drm_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        ready = poll(&pfd, 1, KMS_FLIP_TIMEOUT_MS);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            break;
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
            break;
        if (!(pfd.revents & POLLIN))
            continue;

        memset(&event, 0, sizeof(event));
        event.version = V2N_DRM_EVENT_CONTEXT_VERSION;
        event.page_flip_handler = kms_flip_handler;
        kms->handle_event(kms->drm_fd, &event);
    }

    /* A flip that never landed must not wedge the next present. */
    kms->flip_pending = false;
}

static void kms_release_retired(HostKmsPresent *kms)
{
    if (!kms->retired_bo)
        return;

    kms->rm_fb(kms->drm_fd, kms->retired_fb);
    kms->surface_release_buffer(kms->surface, kms->retired_bo);
    kms->retired_bo = NULL;
    kms->retired_fb = 0;
}

static bool kms_load_libraries(HostKmsPresent *kms)
{
    kms->drm_lib = dlopen("libdrm.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!kms->drm_lib)
        kms->drm_lib = dlopen("libdrm.so", RTLD_NOW | RTLD_LOCAL);
    if (!kms->drm_lib) {
        fprintf(stderr, "vita2native: KMS: libdrm is not available\n");
        return false;
    }

    *(void **)(&kms->get_resources) = dlsym(kms->drm_lib, "drmModeGetResources");
    *(void **)(&kms->free_resources) = dlsym(kms->drm_lib, "drmModeFreeResources");
    *(void **)(&kms->get_connector) = dlsym(kms->drm_lib, "drmModeGetConnector");
    *(void **)(&kms->free_connector) = dlsym(kms->drm_lib, "drmModeFreeConnector");
    *(void **)(&kms->get_crtc) = dlsym(kms->drm_lib, "drmModeGetCrtc");
    *(void **)(&kms->free_crtc) = dlsym(kms->drm_lib, "drmModeFreeCrtc");
    *(void **)(&kms->page_flip) = dlsym(kms->drm_lib, "drmModePageFlip");
    *(void **)(&kms->add_fb) = dlsym(kms->drm_lib, "drmModeAddFB");
    *(void **)(&kms->rm_fb) = dlsym(kms->drm_lib, "drmModeRmFB");
    *(void **)(&kms->handle_event) = dlsym(kms->drm_lib, "drmHandleEvent");

    if (!kms->get_resources || !kms->free_resources || !kms->get_connector ||
        !kms->free_connector || !kms->get_crtc || !kms->free_crtc || !kms->page_flip ||
        !kms->add_fb || !kms->rm_fb || !kms->handle_event) {
        fprintf(stderr, "vita2native: KMS: libdrm is missing the flip entry points\n");
        return false;
    }

    kms->gbm_lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!kms->gbm_lib)
        kms->gbm_lib = dlopen("libgbm.so", RTLD_NOW | RTLD_LOCAL);
    if (!kms->gbm_lib) {
        fprintf(stderr, "vita2native: KMS: libgbm is not available\n");
        return false;
    }

    *(void **)(&kms->surface_create) = dlsym(kms->gbm_lib, "gbm_surface_create");
    *(void **)(&kms->surface_destroy) = dlsym(kms->gbm_lib, "gbm_surface_destroy");
    *(void **)(&kms->surface_lock_front_buffer) =
        dlsym(kms->gbm_lib, "gbm_surface_lock_front_buffer");
    *(void **)(&kms->surface_release_buffer) =
        dlsym(kms->gbm_lib, "gbm_surface_release_buffer");
    *(void **)(&kms->bo_get_width) = dlsym(kms->gbm_lib, "gbm_bo_get_width");
    *(void **)(&kms->bo_get_height) = dlsym(kms->gbm_lib, "gbm_bo_get_height");
    *(void **)(&kms->bo_get_stride) = dlsym(kms->gbm_lib, "gbm_bo_get_stride");
    *(void **)(&kms->bo_get_handle) = dlsym(kms->gbm_lib, "gbm_bo_get_handle");

    if (!kms->surface_create || !kms->surface_destroy || !kms->surface_lock_front_buffer ||
        !kms->surface_release_buffer || !kms->bo_get_width || !kms->bo_get_height ||
        !kms->bo_get_stride || !kms->bo_get_handle) {
        fprintf(stderr, "vita2native: KMS: libgbm is missing the surface entry points\n");
        return false;
    }

    return true;
}

/*
 * Adopts the output SDL is already driving.
 *
 * The live CRTC is the one with a framebuffer bound; its mode is the mode on
 * screen. Reading it back means this module never picks a mode of its own,
 * which is the whole point of letting SDL own the window.
 */
static bool kms_adopt_output(HostKmsPresent *kms)
{
    KmsResources *resources;
    int i;

    resources = kms->get_resources(kms->drm_fd);
    if (!resources) {
        fprintf(stderr, "vita2native: KMS: drmModeGetResources failed\n");
        return false;
    }

    for (i = 0; i < resources->count_crtcs; i++) {
        KmsCrtc *crtc = kms->get_crtc(kms->drm_fd, resources->crtcs[i]);

        if (!crtc)
            continue;

        if (crtc->mode_valid && crtc->buffer_id) {
            kms->crtc_id = crtc->crtc_id;
            kms->mode = crtc->mode;
            kms->mode_valid = true;
            kms->free_crtc(crtc);
            break;
        }

        kms->free_crtc(crtc);
    }

    if (!kms->mode_valid) {
        fprintf(stderr, "vita2native: KMS: no configured output to adopt\n");
        kms->free_resources(resources);
        return false;
    }

    for (i = 0; i < resources->count_connectors; i++) {
        KmsConnector *connector = kms->get_connector(kms->drm_fd, resources->connectors[i]);

        if (!connector)
            continue;

        if (connector->connection == V2N_DRM_MODE_CONNECTED && connector->count_modes > 0) {
            kms->connector_id = connector->connector_id;
            kms->free_connector(connector);
            break;
        }

        kms->free_connector(connector);
    }

    kms->free_resources(resources);

    if (!kms->connector_id) {
        fprintf(stderr, "vita2native: KMS: no connected display\n");
        return false;
    }

    return true;
}

HostKmsPresent *host_kms_present_create(void *gbm_device, int drm_fd, int width, int height)
{
    HostKmsPresent *kms;

    if (!gbm_device || drm_fd < 0)
        return NULL;

    kms = calloc(1, sizeof(*kms));
    if (!kms)
        return NULL;

    kms->drm_fd = drm_fd;

    if (!kms_load_libraries(kms))
        goto fail;

    if (!kms_adopt_output(kms))
        goto fail;

    /*
     * The surface is display-sized: the presenter blits the guest frame into
     * it, letterboxed, exactly as the window path does.
     */
    if (width <= 0 || height <= 0) {
        width = (int)kms->mode.hdisplay;
        height = (int)kms->mode.vdisplay;
    }

    kms->surface = kms->surface_create(gbm_device, (uint32_t)width, (uint32_t)height,
                                       V2N_GBM_FORMAT_ARGB8888,
                                       V2N_GBM_BO_USE_SCANOUT | V2N_GBM_BO_USE_RENDERING);
    if (!kms->surface) {
        fprintf(stderr, "vita2native: KMS: gbm_surface_create(%dx%d) failed\n", width, height);
        goto fail;
    }

    fprintf(stderr, "vita2native: KMS display %s %ux%u@%u\n", kms->mode.name, kms->mode.hdisplay,
            kms->mode.vdisplay, kms->mode.vrefresh);

    return kms;

fail:
    host_kms_present_destroy(kms);
    return NULL;
}

void *host_kms_present_surface(const HostKmsPresent *kms)
{
    return kms ? kms->surface : NULL;
}

bool host_kms_present_active(const HostKmsPresent *kms)
{
    return kms && kms->surface && kms->crtc_id != 0;
}

int host_kms_present_flip(HostKmsPresent *kms)
{
    void *bo;
    uint32_t fb_id = 0;
    int ret;

    if (!host_kms_present_active(kms))
        return -1;

    /* The display may still scan the buffer retired by the previous flip. */
    host_kms_present_wait_idle(kms);
    kms_release_retired(kms);

    bo = kms->surface_lock_front_buffer(kms->surface);
    if (!bo) {
        fprintf(stderr, "vita2native: KMS: gbm_surface_lock_front_buffer failed\n");
        return -1;
    }

    ret = kms->add_fb(kms->drm_fd, kms->bo_get_width(bo), kms->bo_get_height(bo), 24, 32,
                      kms->bo_get_stride(bo), kms->bo_get_handle(bo).u32, &fb_id);
    if (ret != 0) {
        fprintf(stderr, "vita2native: KMS: drmModeAddFB failed (%d)\n", ret);
        kms->surface_release_buffer(kms->surface, bo);
        return -1;
    }

    /* SDL already set the mode, so every frame is a flip, never a mode set. */
    ret = kms->page_flip(kms->drm_fd, kms->crtc_id, fb_id, V2N_DRM_MODE_PAGE_FLIP_EVENT, kms);
    if (ret != 0) {
        fprintf(stderr, "vita2native: KMS: drmModePageFlip failed (%d)\n", ret);
        kms->rm_fb(kms->drm_fd, fb_id);
        kms->surface_release_buffer(kms->surface, bo);
        return -1;
    }
    kms->flip_pending = true;

    kms->retired_bo = kms->scanout_bo;
    kms->retired_fb = kms->scanout_fb;
    kms->scanout_bo = bo;
    kms->scanout_fb = fb_id;

    return 0;
}

void host_kms_present_destroy(HostKmsPresent *kms)
{
    if (!kms)
        return;

    if (kms->surface) {
        host_kms_present_wait_idle(kms);
        kms_release_retired(kms);

        if (kms->scanout_bo) {
            kms->rm_fb(kms->drm_fd, kms->scanout_fb);
            kms->surface_release_buffer(kms->surface, kms->scanout_bo);
            kms->scanout_bo = NULL;
            kms->scanout_fb = 0;
        }

        kms->surface_destroy(kms->surface);
        kms->surface = NULL;
    }

    if (kms->gbm_lib)
        dlclose(kms->gbm_lib);
    if (kms->drm_lib)
        dlclose(kms->drm_lib);

    free(kms);
}
