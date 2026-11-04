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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_gfx_gl.h"
#include "host_gfx_internal.h"

static void *open_first(const char *const *names)
{
    for (size_t i = 0; names[i]; i++) {
        void *handle = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
        if (handle)
            return handle;
    }

    return NULL;
}

#define LOAD_EGL(g, name)                                                                          \
    do {                                                                                           \
        *(void **)(&(g)->name) = dlsym((g)->egl_lib, #name);                                       \
        if (!(g)->name) {                                                                          \
            fprintf(stderr, "vita2native: EGL is missing %s\n", #name);                            \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

#define LOAD_GL(g, type, name)                                                                     \
    do {                                                                                           \
        *(void **)(&(g)->name) = dlsym((g)->gles_lib, #name);                                      \
        if (!(g)->name) {                                                                          \
            fprintf(stderr, "vita2native: GLESv2 is missing %s\n", #name);                         \
            goto fail;                                                                             \
        }                                                                                          \
    } while (0)

static void gles_destroy_impl(HostGles *g)
{
    if (!g)
        return;

    if (g->display != EGL_NO_DISPLAY) {
        if (g->glDeleteFramebuffers)
            g->glDeleteFramebuffers(1, &g->framebuffer);
        if (g->glDeleteTextures)
            g->glDeleteTextures(1, &g->color_texture);

        g->eglMakeCurrent(g->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g->surface != EGL_NO_SURFACE)
            g->eglDestroySurface(g->display, g->surface);
        if (g->context != EGL_NO_CONTEXT)
            g->eglDestroyContext(g->display, g->context);
        if (g->eglReleaseThread)
            g->eglReleaseThread();
        g->eglTerminate(g->display);
    }

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
    static const EGLint config_attribs[] = {
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
    g->width = config->width > 0 ? config->width : 1;
    g->height = config->height > 0 ? config->height : 1;

    g->egl_lib = open_first(egl_names);
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
    LOAD_EGL(g, eglDestroySurface);
    LOAD_EGL(g, eglMakeCurrent);
    LOAD_EGL(g, eglReleaseThread);

    /*
     * Prefer the surfaceless platform: it needs no X11/Wayland connection and
     * works on headless machines with a software driver. Try the core EGL 1.5
     * entry point first (glvnd does not always export the EXT one), then the
     * EXT entry point, then the build-time default platform.
     */
    {
        EGLDisplay candidates[3];
        int num_candidates = 0;
        bool initialized = false;

        if (g->eglGetPlatformDisplay) {
            candidates[num_candidates++] = g->eglGetPlatformDisplay(
                EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        }
        if (g->eglGetPlatformDisplayEXT) {
            candidates[num_candidates++] = g->eglGetPlatformDisplayEXT(
                EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        }
        candidates[num_candidates++] = g->eglGetDisplay(EGL_DEFAULT_DISPLAY);

        for (int i = 0; i < num_candidates && !initialized; i++) {
            if (candidates[i] == EGL_NO_DISPLAY)
                continue;

            if (g->eglInitialize(candidates[i], &egl_major, &egl_minor)) {
                g->display = candidates[i];
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

    if (!g->eglChooseConfig(g->display, config_attribs, &egl_config, 1, &num_configs) ||
        num_configs < 1) {
        fprintf(stderr, "vita2native: no EGL config with GLES 3.x\n");
        goto fail;
    }

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

    g->gles_lib = open_first(gles_names);
    if (!g->gles_lib)
        goto fail;

#define HOST_GL_LOAD(type, name) LOAD_GL(g, type, name);
    HOST_GL_FUNCTIONS(HOST_GL_LOAD)
#undef HOST_GL_LOAD

    version = (const char *)g->glGetString(GL_VERSION);
    renderer = (const char *)g->glGetString(GL_RENDERER);
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
