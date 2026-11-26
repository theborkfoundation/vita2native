/*
 * Staged readback: blit a render target into a linear dma-buf and hand the CPU
 * its mapping.
 *
 * glReadPixels on a tiled GPU runs the driver's per-pixel transfer path. On the
 * Orange Pi's Mali-G31 a 960x544 read measured ~99 ms, of which the blit is
 * 2.8 ms; the cost is the driver walking the surface, not the bytes. PBOs and
 * AFBC-off don't change it. A linear buffer the CPU can memcpy does.
 *
 * The staging buffer is a gbm BO imported as an EGLImage, so the blit is a plain
 * GPU copy. Several sizes are kept because titles alternate targets inside a
 * frame (a 960x544 scene plus a small UI one); rebuilding the buffer per scene
 * costs more than the copy it saves.
 *
 * Every failure — no libgbm, no DRM render node, software GL, an incomplete
 * framebuffer — returns false so the caller keeps glReadPixels.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "host_gfx_gl.h"
#include "host_gfx_internal.h"

/* After host_gfx_gl.h: gl2ext.h needs the GLES3 base types in scope. */
#include <GLES2/gl2ext.h>

/* gbm is dlopen'd: the runtime also has to start where it is absent. */
struct gbm_device;
struct gbm_bo;

typedef struct {
    void *lib;
    struct gbm_device *(*create_device)(int fd);
    void (*device_destroy)(struct gbm_device *device);
    struct gbm_bo *(*bo_create)(struct gbm_device *device, uint32_t width, uint32_t height,
                                uint32_t format, uint32_t flags);
    void (*bo_destroy)(struct gbm_bo *bo);
    uint32_t (*bo_get_stride)(struct gbm_bo *bo);
    int (*bo_get_fd)(struct gbm_bo *bo);
    void *(*bo_map)(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                    uint32_t flags, uint32_t *stride, void **map_data);
    void (*bo_unmap)(struct gbm_bo *bo, void *map_data);
} GbmApi;

#define GBM_BO_TRANSFER_READ_WRITE 0x3u
#define GBM_BO_USE_RENDERING       (1u << 2)
#define GBM_BO_USE_LINEAR          (1u << 4)
/*
 * The staging buffer must be byte-identical to the GL readback the rest of the
 * code expects: RGBA bytes. DRM names a format by its 32-bit word, so ARGB8888
 * is B,G,R,A in memory and ABGR8888 is the R,G,B,A one.
 */
#define DRM_FORMAT_ABGR8888        0x34324241u

/* Distinct sizes a frame may read from; one per render target in practice. */
#define HOST_READBACK_TARGETS 4

typedef struct {
    bool used;
    int width;
    int height;
    struct gbm_bo *bo;
    EGLImageKHR image;
    GLuint texture;
    GLuint fbo;
    const void *mapping;
    void *map_data;
    unsigned int stride;
    unsigned int last_used;
} HostReadbackTarget;

typedef struct {
    bool initialised;
    bool available;

    GbmApi gbm;
    struct gbm_device *device;
    int drm_fd;

    /* Entry points that only exist on EGL 1.5 / extensions. */
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;

    unsigned int clock;
    HostReadbackTarget targets[HOST_READBACK_TARGETS];
} HostReadback;

static HostReadback g_readback;

static bool software_renderer(HostGles *gl)
{
    const char *renderer = (const char *)gl->glGetString(GL_RENDERER);

    if (!renderer)
        return false;

    /* A software driver's readback is already a memcpy; a blit plus a copy of
       an uncached mapping would be slower than the direct read. */
    return strstr(renderer, "llvmpipe") || strstr(renderer, "swrast") ||
           strstr(renderer, "softpipe");
}

static bool load_gbm(GbmApi *api)
{
    api->lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!api->lib)
        api->lib = dlopen("libgbm.so", RTLD_NOW | RTLD_LOCAL);
    if (!api->lib)
        return false;

    *(void **)(&api->create_device) = dlsym(api->lib, "gbm_create_device");
    *(void **)(&api->device_destroy) = dlsym(api->lib, "gbm_device_destroy");
    *(void **)(&api->bo_create) = dlsym(api->lib, "gbm_bo_create");
    *(void **)(&api->bo_destroy) = dlsym(api->lib, "gbm_bo_destroy");
    *(void **)(&api->bo_get_stride) = dlsym(api->lib, "gbm_bo_get_stride");
    *(void **)(&api->bo_get_fd) = dlsym(api->lib, "gbm_bo_get_fd");
    *(void **)(&api->bo_map) = dlsym(api->lib, "gbm_bo_map");
    *(void **)(&api->bo_unmap) = dlsym(api->lib, "gbm_bo_unmap");

    return api->create_device && api->device_destroy && api->bo_create && api->bo_destroy &&
           api->bo_get_stride && api->bo_get_fd && api->bo_map && api->bo_unmap;
}

static int open_drm_node(void)
{
    static const char *const nodes[] = {
        "/dev/dri/renderD128", "/dev/dri/renderD129", "/dev/dri/renderD130", "/dev/dri/card0",
    };
    size_t count = sizeof(nodes) / sizeof(nodes[0]);

    for (size_t i = 0; i < count; i++) {
        int fd = open(nodes[i], O_RDWR | O_CLOEXEC);

        if (fd >= 0)
            return fd;
    }

    return -1;
}

static void destroy_target(HostGles *gl, HostReadbackTarget *target)
{
    HostReadback *r = &g_readback;

    if (!target->used)
        return;

    if (gl) {
        if (target->fbo)
            gl->glDeleteFramebuffers(1, &target->fbo);
        if (target->texture)
            gl->glDeleteTextures(1, &target->texture);
    }
    if (target->image != EGL_NO_IMAGE_KHR && r->destroy_image && gl)
        r->destroy_image(gl->display, target->image);
    if (target->bo) {
        if (target->mapping)
            r->gbm.bo_unmap(target->bo, target->map_data);
        r->gbm.bo_destroy(target->bo);
    }

    memset(target, 0, sizeof(*target));
    target->image = EGL_NO_IMAGE_KHR;
}

static bool create_target(HostGles *gl, HostReadbackTarget *target, int width, int height)
{
    HostReadback *r = &g_readback;
    uint32_t map_stride = 0;
    GLint previous = 0;
    EGLint attrs[24];
    GLenum status;
    int n = 0;
    int fd;

    memset(target, 0, sizeof(*target));
    target->image = EGL_NO_IMAGE_KHR;

    target->bo = r->gbm.bo_create(r->device, (uint32_t)width, (uint32_t)height,
                                  DRM_FORMAT_ABGR8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    if (!target->bo)
        return false;

    /* Kept mapped for the buffer's lifetime: the blit writes it every frame. */
    target->mapping = r->gbm.bo_map(target->bo, 0, 0, (uint32_t)width, (uint32_t)height,
                                    GBM_BO_TRANSFER_READ_WRITE, &map_stride, &target->map_data);
    if (!target->mapping) {
        destroy_target(gl, target);
        return false;
    }
    target->stride = map_stride;

    fd = r->gbm.bo_get_fd(target->bo);
    if (fd < 0) {
        destroy_target(gl, target);
        return false;
    }

    attrs[n++] = EGL_WIDTH;
    attrs[n++] = width;
    attrs[n++] = EGL_HEIGHT;
    attrs[n++] = height;
    attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[n++] = (EGLint)DRM_FORMAT_ABGR8888;
    attrs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attrs[n++] = fd;
    attrs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attrs[n++] = 0;
    attrs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attrs[n++] = (EGLint)target->stride;
    attrs[n++] = EGL_NONE;

    target->image =
        r->create_image(gl->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (target->image == EGL_NO_IMAGE_KHR) {
        destroy_target(gl, target);
        return false;
    }

    gl->glGenTextures(1, &target->texture);
    gl->glBindTexture(GL_TEXTURE_2D, target->texture);
    r->image_target_texture(GL_TEXTURE_2D, (GLeglImageOES)target->image);

    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    gl->glGenFramebuffers(1, &target->fbo);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target->texture,
                               0);
    status = gl->glCheckFramebufferStatus(GL_FRAMEBUFFER);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        destroy_target(gl, target);
        return false;
    }

    target->width = width;
    target->height = height;
    target->used = true;
    return true;
}

/* Returns the staging target for this size, evicting the least recently used. */
static HostReadbackTarget *find_target(HostGles *gl, int width, int height)
{
    HostReadback *r = &g_readback;
    HostReadbackTarget *victim = NULL;

    r->clock++;

    for (size_t i = 0; i < HOST_READBACK_TARGETS; i++) {
        HostReadbackTarget *target = &r->targets[i];

        if (target->used && target->width == width && target->height == height) {
            target->last_used = r->clock;
            return target;
        }
        if (!target->used) {
            victim = target;
            continue;
        }
        if (!victim || target->last_used < victim->last_used)
            victim = target;
    }

    if (!victim)
        return NULL;

    destroy_target(gl, victim);
    if (!create_target(gl, victim, width, height))
        return NULL;

    victim->last_used = r->clock;
    return victim;
}

static void init(HostGles *gl)
{
    HostReadback *r = &g_readback;

    r->initialised = true;
    r->available = false;
    r->drm_fd = -1;

    if (software_renderer(gl))
        return;
    if (!load_gbm(&r->gbm))
        return;

    r->create_image = (PFNEGLCREATEIMAGEKHRPROC)gl->eglGetProcAddress("eglCreateImageKHR");
    r->destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)gl->eglGetProcAddress("eglDestroyImageKHR");
    r->image_target_texture =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)gl->eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!r->create_image || !r->destroy_image || !r->image_target_texture)
        return;

    r->drm_fd = open_drm_node();
    if (r->drm_fd < 0)
        return;

    r->device = r->gbm.create_device(r->drm_fd);
    if (!r->device)
        return;

    r->available = true;
}

bool host_gfx_readback_read_target(HostGpuDevice device, unsigned int framebuffer, int width,
                                   int height, const void **pixels, unsigned int *stride)
{
    HostGles *gl = host_gpu_get_gles(device);
    HostReadbackTarget *target;
    GLint previous_read = 0;
    GLint previous_draw = 0;

    if (!gl || !pixels || !stride || width <= 0 || height <= 0)
        return false;

    /* GL calls below need this thread's context current. */
    host_gfx_gl_thread_enter(device);

    if (!g_readback.initialised)
        init(gl);
    if (!g_readback.available)
        return false;

    target = find_target(gl, width, height);
    if (!target)
        return false;

    /*
     * No flip: the blit uses the same origin as glReadPixels, so the caller's
     * bottom-up row handling stays correct.
     */
    gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target->fbo);
    gl->glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);
    gl->glFinish();
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read);
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)previous_draw);

    *pixels = target->mapping;
    *stride = target->stride;
    return true;
}

void host_gfx_readback_shutdown(HostGpuDevice device)
{
    HostReadback *r = &g_readback;
    HostGles *gl = host_gpu_get_gles(device);

    if (!r->initialised)
        return;

    if (gl) {
        host_gfx_gl_thread_enter(device);
        for (size_t i = 0; i < HOST_READBACK_TARGETS; i++)
            destroy_target(gl, &r->targets[i]);
    }

    if (r->device) {
        r->gbm.device_destroy(r->device);
        r->device = NULL;
    }
    if (r->drm_fd >= 0) {
        close(r->drm_fd);
        r->drm_fd = -1;
    }
    if (r->gbm.lib) {
        dlclose(r->gbm.lib);
        memset(&r->gbm, 0, sizeof(r->gbm));
    }

    r->initialised = false;
    r->available = false;
}
