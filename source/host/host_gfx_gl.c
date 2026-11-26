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

#include "frame_profile.h"
#include "host_gfx_gl.h"
#include "host_gfx_internal.h"
#include "host_kms_present.h"
#include "host_overlay_image.h"
#include "host_present_layout.h"
#include "settings.h"

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

/* EGL native window and window surface, built from the SDL window. */
static void *gles_native_window(HostGles *g, const HostWindowInfo *win);
static EGLSurface gles_attach_window_surface(HostGles *g, void *native_window);

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

    /*
     * Shares objects with the primary context. Built from the pbuffer config
     * when there is one, so this thread's drawable can be a pbuffer: the window
     * config a window surface needs usually cannot make one.
     */
    context = g->eglCreateContext(g->display, g->thread_config ? g->thread_config : g->egl_config,
                                  g->context, g_thread_context_attribs);
    if (context == EGL_NO_CONTEXT) {
        fprintf(stderr, "vita2native: eglCreateContext(thread) failed: 0x%x\n",
                g->eglGetError());
        return EGL_NO_CONTEXT;
    }

    /*
     * Guest threads each need a drawable, and the window config usually has no
     * EGL_PBUFFER_BIT. Give them a pbuffer from the pbuffer-only config; if the
     * display has none, they fall back to surfaceless.
     */
    if (g->thread_config)
        surface = g->eglCreatePbufferSurface(g->display, g->thread_config,
                                             g_thread_pbuffer_attribs);
    else
        surface = EGL_NO_SURFACE;

    if (surface == EGL_NO_SURFACE && g->thread_config) {
        fprintf(stderr, "vita2native: eglCreatePbufferSurface(thread) failed: 0x%x\n",
                g->eglGetError());
        g->eglDestroyContext(g->display, context);
        return EGL_NO_CONTEXT;
    }

    gl_remember_thread_context(g, context, surface);
    g_thread_state.gles = g;
    g_thread_state.context = context;
    g_thread_state.surface = surface;
    /*
     * Nothing is current yet, so "bound" must not claim the surface we are
     * about to bind: gl_thread_bind() skips eglMakeCurrent when the two match,
     * and a context that was never made current fails every shader compile with
     * an empty info log.
     */
    g_thread_state.bound = (EGLSurface)(uintptr_t)-1;
    g_thread_state.window = false;

    if (vita2native_env_flag("VITA2NATIVE_DEBUG_PRESENT"))
        fprintf(stderr, "vita2native: thread ctx created (pbuffer %p, config %p)\n",
                (void *)surface, (void *)g->thread_config);

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
        if (g->glDeleteTextures) {
            g->glDeleteTextures(1, &g->color_texture);
            g->glDeleteTextures(1, &g->present_texture);
            g->glDeleteTextures(1, &g->border_texture);
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
        /* The gbm_surface must outlive the EGL surface built on it. */
        if (g->window_surface != EGL_NO_SURFACE)
            g->eglDestroySurface(g->display, g->window_surface);
        if (g->surface != EGL_NO_SURFACE)
            g->eglDestroySurface(g->display, g->surface);
        if (g->context != EGL_NO_CONTEXT)
            g->eglDestroyContext(g->display, g->context);
        if (g->eglReleaseThread)
            g->eglReleaseThread();
        g->eglTerminate(g->display);
    }

    /* After the EGL surface that wraps it, never before. */
    host_kms_present_destroy(g->kms);
    g->kms = NULL;

    if (g->egl_lib)
        dlclose(g->egl_lib);
    if (g->gles_lib)
        dlclose(g->gles_lib);

    free(g);
}

/*
 * Test binaries link this file without the SDL video layer, which is what owns
 * the window. The emulator's definition in host_video_sdl.c overrides this.
 */
__attribute__((weak)) bool host_video_window_info(HostWindowInfo *info)
{
    if (info)
        memset(info, 0, sizeof(*info));
    return false;
}

HostGpuDevice host_gfx_gl_create(const HostGpuConfig *config)
{
    static const char *const egl_names[] = {
        "libEGL.so.1", "libEGL.so", "libEGL.dylib", NULL,
    };
    static const char *const gles_names[] = {
        "libGLESv2.so.2", "libGLESv2.so", "libGLESv2.dylib", NULL,
    };
    /*
     * Window *and* pbuffer is what the primary surface wants, but many displays
     * (this Wayland one included) offer no such config. Ask for window+pbuffer,
     * then window alone, then pbuffer alone: the primary context draws to the
     * window, and guest threads take a separate pbuffer config.
     */
    static const EGLint config_attribs_window[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
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
    /* Optional: only the window presenter uses these, to find a config the
       window's visual accepts. */
    *(void **)(&g->eglGetConfigs) = dlsym(g->egl_lib, "eglGetConfigs");
    *(void **)(&g->eglGetConfigAttrib) = dlsym(g->egl_lib, "eglGetConfigAttrib");
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
     * SDL decided the platform; follow it.
     *
     * SDL has already probed X11, Wayland, KMSDRM and the headless drivers in
     * its own order and created a window. This builds the EGL display for the
     * handle it reports, so the guest renders into a surface that belongs to
     * that window. Probing platforms here would create a second, unrelated EGL
     * display that cannot present to SDL's window at all.
     */
    {
        HostWindowInfo win;
        EGLDisplay dpy = EGL_NO_DISPLAY;

        memset(&win, 0, sizeof(win));

        if (!config->headless && host_video_window_info(&win)) {
            switch (win.type) {
            case HOST_WINDOW_X11:
                if (g->eglGetPlatformDisplayEXT)
                    dpy = g->eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_KHR, win.display, NULL);
                if (dpy == EGL_NO_DISPLAY)
                    dpy = g->eglGetDisplay((EGLNativeDisplayType)win.display);
                break;
            case HOST_WINDOW_WAYLAND:
                if (g->eglGetPlatformDisplayEXT)
                    dpy = g->eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_KHR, win.display, NULL);
                if (dpy == EGL_NO_DISPLAY)
                    dpy = g->eglGetDisplay((EGLNativeDisplayType)win.display);
                break;
            case HOST_WINDOW_KMSDRM:
                /* SDL owns the DRM node; ask EGL for its default display on it. */
                if (g->eglGetPlatformDisplayEXT && win.gbm_device)
                    dpy = g->eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, win.gbm_device, NULL);
                if (dpy == EGL_NO_DISPLAY)
                    dpy = g->eglGetDisplay(EGL_DEFAULT_DISPLAY);
                break;
            default:
                break;
            }
        }

        /* No window (headless, or SDL found no driver): render offscreen. */
        if (dpy == EGL_NO_DISPLAY && g->eglGetPlatformDisplayEXT)
            dpy = g->eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY,
                                              NULL);
        if (dpy == EGL_NO_DISPLAY && g->eglGetPlatformDisplay)
            dpy = g->eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (dpy == EGL_NO_DISPLAY)
            dpy = g->eglGetDisplay(EGL_DEFAULT_DISPLAY);

        if (dpy == EGL_NO_DISPLAY || !g->eglInitialize(dpy, &egl_major, &egl_minor)) {
            fprintf(stderr, "vita2native: no usable EGL display: 0x%x\n", g->eglGetError());
            goto fail;
        }

        g->display = dpy;
        g->window_info = win;

        if (vita2native_env_flag("VITA2NATIVE_DEBUG_PRESENT"))
            fprintf(stderr, "vita2native: EGL display %p from window type %d (headless=%d)\n",
                    (void *)dpy, (int)win.type, config->headless ? 1 : 0);
    }

    if (!g->eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "vita2native: eglBindAPI(EGL_OPENGL_ES_API) failed\n");
        goto fail;
    }

    /*
     * One config cannot usually be both: this Wayland display (like many
     * others) has no config carrying EGL_WINDOW_BIT and EGL_PBUFFER_BIT
     * together. The primary context draws to the window, so it takes a window
     * config; guest threads each need their own drawable and take a
     * pbuffer-only config. Contexts created from different configs still share
     * objects with the primary one.
     */
    if (!config->headless &&
        g->eglChooseConfig(g->display, config_attribs_window, &egl_config, 1, &num_configs) &&
        num_configs >= 1) {
        g->window_capable = true;
    } else if (!g->eglChooseConfig(g->display, config_attribs_pbuffer, &egl_config, 1,
                                   &num_configs) ||
               num_configs < 1) {
        fprintf(stderr, "vita2native: no EGL config with GLES 3.x (window query: %d configs, err "
                        "0x%x)\n",
                (int)num_configs, g->eglGetError());
        goto fail;
    }

    {
        EGLint thread_configs = 0;

        if (!g->eglChooseConfig(g->display, config_attribs_pbuffer, &g->thread_config, 1,
                                &thread_configs) ||
            thread_configs < 1)
            g->thread_config = NULL;
    }

    g->egl_config = egl_config;

    g->context = g->eglCreateContext(g->display, egl_config, EGL_NO_CONTEXT, context_attribs);
    if (g->context == EGL_NO_CONTEXT) {
        fprintf(stderr, "vita2native: eglCreateContext failed: 0x%x\n", g->eglGetError());
        goto fail;
    }

    /*
     * The primary context starts on an offscreen drawable, never the window.
     *
     * The device is built on the thread that calls host_gpu_create, but the
     * guest presents from its own host thread. An EGL window surface can only
     * be current on the thread that created it, and binding it elsewhere fails
     * with EGL_BAD_ACCESS, so the window surface is created lazily in
     * host_gfx_gl_attach_window() - on the thread that presents.
     */
    if (g->thread_config)
        g->surface = g->eglCreatePbufferSurface(g->display, g->thread_config, pbuffer_attribs);

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

    if (vita2native_env_flag("VITA2NATIVE_DEBUG_GL_INFO")) {
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
        /*
         * Alpha is forced to 1: the guest's render target keeps the alpha it
         * blended with, and a window that is composited per pixel (the VNC X
         * server is) would then blend the presented frame against black -
         * Shakedown's white logo card came out dark, with only the opaque FPS
         * overlay unaffected. The scanout path ignores alpha either way.
         */
        "    o_color = vec4(texture(u_tex, uv).rgb, 1.0);\n"
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

/* Decode the embedded artwork once. Without one, generate a placeholder. */
static bool gles_create_border_texture(HostGles *g)
{
    enum { BORDER_WIDTH = 128, BORDER_HEIGHT = 32 };
    unsigned char fallback[BORDER_WIDTH * BORDER_HEIGHT * 4];
    int width = 0, height = 0;
    unsigned char *image = host_overlay_image_load(&width, &height);
    const unsigned char *pixels;

    if (image) {
        GLint max_size = 0;
        g->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
        if (max_size <= 0 || width > max_size || height > max_size) {
            fprintf(stderr, "vita2native: embedded overlay %dx%d exceeds GPU texture limit %d\n",
                    width, height, max_size);
            host_overlay_image_free(image);
            image = NULL;
        }
    }
    pixels = image;
    if (image) {
        /* The presenter outputs opaque pixels: composite transparent PNG pixels
           against the same blue-gray base used by the default border. */
        for (size_t i = 0; i < (size_t)width * height; ++i) {
            unsigned char *p = &image[i * 4];
            unsigned int alpha = p[3];
            p[0] = (unsigned char)((p[0] * alpha + 21 * (255 - alpha) + 127) / 255);
            p[1] = (unsigned char)((p[1] * alpha + 33 * (255 - alpha) + 127) / 255);
            p[2] = (unsigned char)((p[2] * alpha + 48 * (255 - alpha) + 127) / 255);
            p[3] = 255;
        }
        /* PNG rows start at the top; GLES texture coordinates start below. */
        g->border_flip = true;
    } else {
        width = BORDER_WIDTH;
        height = BORDER_HEIGHT;
        pixels = fallback;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                unsigned char *p = &fallback[(y * width + x) * 4];
                int stripe = ((x + 2 * y) % 32) < 2 ? 11 : 0;
                int glow = (x < 64 ? x : 127 - x) / 5;
                p[0] = (unsigned char)(21 + glow + stripe);
                p[1] = (unsigned char)(33 + glow + stripe);
                p[2] = (unsigned char)(48 + glow + stripe);
                p[3] = 255;
            }
        }
    }
    g->glGenTextures(1, &g->border_texture);
    if (!g->border_texture) {
        host_overlay_image_free(image);
        return false;
    }
    g->glActiveTexture(GL_TEXTURE0);
    g->glBindTexture(GL_TEXTURE_2D, g->border_texture);
    g->glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    g->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, pixels);
    host_overlay_image_free(image);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

/* Compute one aspect-fit rectangle for both presentation paths. */
static void gles_present_rect(int window_width, int window_height, int width, int height,
                              GLint *x, GLint *y, GLint *w, GLint *h)
{
    HostPresentRect rect = host_present_layout(window_width, window_height, width, height);
    *x = rect.x;
    *y = window_height - rect.y - rect.height; /* GLES origin is bottom left. */
    *w = rect.width;
    *h = rect.height;
}

/* Draw only the space outside the guest frame; no guest pixels are covered. */
static void gles_draw_border(HostGles *g, int window_width, int window_height,
                             GLint x, GLint y, GLint width, GLint height)
{
    const GLint rectangles[][4] = {
        {0, 0, window_width, y},
        {0, y + height, window_width, window_height - y - height},
        {0, y, x, height},
        {x + width, y, window_width - x - width, height},
    };
    if (x == 0 && y == 0 && width == window_width && height == window_height)
        return;
    if (!g->border_texture && !gles_create_border_texture(g))
        return;
    g->glUseProgram(g->present_program);
    g->glUniform1f(g->present_flip_uniform, g->border_flip ? 1.0f : 0.0f);
    g->glActiveTexture(GL_TEXTURE0);
    g->glBindTexture(GL_TEXTURE_2D, g->border_texture);
    g->glBindVertexArray(g->present_vao);
    for (unsigned int i = 0; i < sizeof(rectangles) / sizeof(rectangles[0]); ++i) {
        if (rectangles[i][2] <= 0 || rectangles[i][3] <= 0)
            continue;
        g->glViewport(rectangles[i][0], rectangles[i][1], rectangles[i][2], rectangles[i][3]);
        g->glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    g->glBindVertexArray(0);
    g->glBindTexture(GL_TEXTURE_2D, 0);
    g->glUseProgram(0);
}

/*
 * The window's X visual id, read through a dlopen'd libX11.
 *
 * EGL refuses a window surface whose visual differs from the config's
 * EGL_NATIVE_VISUAL_ID, and SDL creates the window with whatever visual its own
 * backend picked. XVisualIDFromVisual() is used rather than reading the Visual
 * struct, so the only layout this depends on is XWindowAttributes: six ints,
 * then the Visual pointer, which holds for 32- and 64-bit.
 */
static int gxm_x11_ignore_error(void *display, void *event)
{
    (void)display;
    (void)event;
    return 0;
}

static bool gxm_x11_visual_id(const void *native_window, EGLint *visual_id)
{
    typedef void *(*x_open_display)(const char *);
    typedef int (*x_get_window_attributes)(void *, unsigned long, void *);
    typedef unsigned long (*x_visual_id_from_visual)(void *, const void *);
    typedef int (*x_close_display)(void *);
    typedef void *(*x_set_error_handler)(void *);
    typedef char *(*x_display_string)(void *);

    static bool resolved;
    static void *library;
    static x_open_display open_display;
    static x_get_window_attributes get_window_attributes;
    static x_visual_id_from_visual visual_id_from_visual;
    static x_close_display close_display;
    static x_set_error_handler set_error_handler;
    static x_display_string display_string;

    unsigned char attributes[256];
    void *display;
    void *visual;

    if (!resolved) {
        resolved = true;
        library = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (library) {
            *(void **)(&open_display) = dlsym(library, "XOpenDisplay");
            *(void **)(&get_window_attributes) = dlsym(library, "XGetWindowAttributes");
            *(void **)(&visual_id_from_visual) = dlsym(library, "XVisualIDFromVisual");
            *(void **)(&close_display) = dlsym(library, "XCloseDisplay");
            *(void **)(&set_error_handler) = dlsym(library, "XSetErrorHandler");
            *(void **)(&display_string) = dlsym(library, "XDisplayString");
        }
    }

    if (!open_display || !get_window_attributes || !visual_id_from_visual || !close_display)
        return false;

    /*
     * A stale or foreign window id must not kill the process: Xlib's default
     * handler exits on BadWindow.
     */
    if (set_error_handler)
        set_error_handler(gxm_x11_ignore_error);

    display = open_display(NULL);
    if (!display)
        return false;

    if (vita2native_env_flag("VITA2NATIVE_DEBUG_PRESENT"))
        fprintf(stderr, "vita2native: X display '%s', window 0x%lx\n",
                display_string ? display_string(display) : "?",
                (unsigned long)(uintptr_t)native_window);

    memset(attributes, 0, sizeof(attributes));
    if (!get_window_attributes(display, (unsigned long)(uintptr_t)native_window, attributes)) {
        close_display(display);
        return false;
    }

    memcpy(&visual, attributes + 24, sizeof(visual));
    if (!visual) {
        close_display(display);
        return false;
    }

    *visual_id = (EGLint)visual_id_from_visual(display, visual);
    close_display(display);
    return true;
}

/*
 * Creates a window surface, binding it on success.
 *
 * The config chosen for the GL device supports window surfaces in general, but
 * a window surface must also use the same native visual as the window, and SDL
 * creates the window with whichever visual its own backend picked. Asking for a
 * surface with the device config therefore fails with EGL_BAD_MATCH (0x300b) on
 * X11. Rather than mirror Xlib just to read the window's visual, try the device
 * config first and then every window-capable config until one is accepted: the
 * surface only carries the final blit, so it needs RGBA8 and nothing else.
 */
static EGLSurface gles_attach_window_surface(HostGles *g, void *native_window)
{
    EGLConfig candidates[64];
    EGLint count = 0;
    EGLint window_visual = 0;
    bool have_visual = gxm_x11_visual_id(native_window, &window_visual);
    EGLSurface surface;
    EGLint i;

    while (g->eglGetError() != EGL_SUCCESS) {
    }

    if (have_visual &&
        vita2native_env_flag("VITA2NATIVE_DEBUG_PRESENT"))
        fprintf(stderr, "vita2native: window visual 0x%x\n", (unsigned)window_visual);

    surface = g->eglCreateWindowSurface(g->display, g->egl_config, native_window, NULL);
    if (surface != EGL_NO_SURFACE)
        return surface;

    if (!g->eglGetConfigs || !g->eglGetConfigAttrib ||
        !g->eglGetConfigs(g->display, candidates, 64, &count))
        return EGL_NO_SURFACE;

    for (i = 0; i < count; i++) {
        EGLint surface_type = 0, renderable = 0, red_size = 0, native_visual = 0;

        if (!g->eglGetConfigAttrib(g->display, candidates[i], EGL_SURFACE_TYPE, &surface_type) ||
            !(surface_type & EGL_WINDOW_BIT))
            continue;
        if (!g->eglGetConfigAttrib(g->display, candidates[i], EGL_RENDERABLE_TYPE, &renderable) ||
            !(renderable & EGL_OPENGL_ES2_BIT))
            continue;
        if (!g->eglGetConfigAttrib(g->display, candidates[i], EGL_RED_SIZE, &red_size) ||
            red_size < 8)
            continue;
        /* When the window's visual is known, only its config can work. */
        if (have_visual) {
            if (!g->eglGetConfigAttrib(g->display, candidates[i], EGL_NATIVE_VISUAL_ID,
                                       &native_visual) ||
                native_visual != window_visual)
                continue;
        }

        while (g->eglGetError() != EGL_SUCCESS) {
        }
        surface = g->eglCreateWindowSurface(g->display, candidates[i], native_window, NULL);
        if (surface != EGL_NO_SURFACE)
            return surface;
    }

    return EGL_NO_SURFACE;
}

/*
 * Builds the EGL native window from what the platform calls a window.
 *
 * X11 takes the Window id as-is. Wayland needs a wl_egl_window, which is an
 * EGL-side object built on the wl_surface, so we create it here rather than
 * asking SDL for an SDL_GLContext just to obtain one.
 */
static void *gles_native_window(HostGles *g, const HostWindowInfo *win)
{
    typedef void *(*wl_egl_window_create_fn)(void *surface, int width, int height);

    wl_egl_window_create_fn create;
    void *native;

    if (!win || win->type == HOST_WINDOW_NONE)
        return NULL;

    /*
     * KMSDRM: SDL owns the DRM node and the mode, but SysWMinfo reports only
     * the gbm_device, and an EGL window surface needs a gbm_surface on it. We
     * make that surface and, afterwards, the page flip.
     */
    if (win->type == HOST_WINDOW_KMSDRM) {
        if (!g->kms)
            g->kms = host_kms_present_create(win->gbm_device, win->drm_fd, win->width,
                                             win->height);
        return g->kms ? host_kms_present_surface(g->kms) : NULL;
    }

    if (win->type != HOST_WINDOW_WAYLAND)
        return win->window;

    if (!g->wayland_egl_lib) {
        g->wayland_egl_lib = dlopen("libwayland-egl.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!g->wayland_egl_lib)
            g->wayland_egl_lib = dlopen("libwayland-egl.so", RTLD_NOW | RTLD_LOCAL);
        if (g->wayland_egl_lib) {
            *(void **)(&g->wl_egl_window_create) =
                dlsym(g->wayland_egl_lib, "wl_egl_window_create");
            *(void **)(&g->wl_egl_window_destroy) =
                dlsym(g->wayland_egl_lib, "wl_egl_window_destroy");
        }
    }

    create = (wl_egl_window_create_fn)g->wl_egl_window_create;
    if (!create || !win->window) {
        fprintf(stderr, "vita2native: libwayland-egl unavailable; cannot make a window surface\n");
        return NULL;
    }

    native = create(win->window, win->width > 0 ? win->width : 960,
                    win->height > 0 ? win->height : 544);
    if (!native)
        fprintf(stderr, "vita2native: wl_egl_window_create failed\n");

    return native;
}

int host_gfx_gl_attach_window(HostGpuDevice device, const HostWindowInfo *win)
{
    HostGles *g = device->impl;
    EGLSurface surface;
    void *native_window;

    if (!g || !win || !g->window_capable || !g->eglCreateWindowSurface)
        return -1;

    /*
     * Created on the calling thread, which is the one that presents: an EGL
     * window surface belongs to the thread that created it. Repeat calls from
     * the same thread just rebind.
     */
    if (g->window_surface != EGL_NO_SURFACE) {
        if (g_window_thread_valid && !pthread_equal(pthread_self(), g_window_thread))
            return -1;
        if (gl_thread_context(g) == EGL_NO_CONTEXT)
            return -1;
        if (gl_thread_bind(g, g->window_surface) != 0)
            return -1;
        g_thread_state.window = true;
        return 0;
    }

    if (gl_thread_context(g) == EGL_NO_CONTEXT)
        return -1;

    native_window = gles_native_window(g, win);
    if (!native_window)
        return -1;

    while (g->glGetError() != GL_NO_ERROR) {
    }

    surface = gles_attach_window_surface(g, native_window);
    if (surface == EGL_NO_SURFACE) {
        fprintf(stderr, "vita2native: no EGL config for this window's visual\n");
        return -1;
    }

    if (gl_thread_bind(g, surface) != 0) {
        g->eglDestroySurface(g->display, surface);
        return -1;
    }

    g->wayland_egl_window = (win->type == HOST_WINDOW_WAYLAND) ? native_window : NULL;
    g->window_surface = surface;
    g_thread_state.window = true;
    g_window_thread = pthread_self();
    g_window_thread_valid = true;

    /*
     * No vsync on a window: the guest already paces itself on vblank waits, and
     * blocking the presenting thread in eglSwapBuffers costs frames (263 ms a
     * frame through Xvnc's software swap, against 5 ms for the SDL blit).
     */
    if (g->eglSwapInterval)
        g->eglSwapInterval(g->display, 0);

    return 0;
}

void host_gfx_gl_note_render_thread(HostGpuDevice device)
{
    HostGles *g = device ? device->impl : NULL;

    if (!g)
        return;

    g->render_thread = pthread_self();
    g->render_thread_valid = true;
}

bool host_gfx_gl_last_present_is_current_thread(HostGpuDevice device)
{
    HostGles *g = device ? device->impl : NULL;

    return g && g->present_thread_valid &&
           pthread_equal(pthread_self(), g->present_thread);
}

void host_gfx_gl_thread_enter(HostGpuDevice device)
{
    HostGles *g = device ? device->impl : NULL;

    if (!g || g->display == EGL_NO_DISPLAY)
        return;

    if (gl_thread_context(g) == EGL_NO_CONTEXT)
        return;

    /*
     * Bind once per thread and leave it: the guest renders into FBOs, which do
     * not care what the default framebuffer is, so a presenting thread can keep
     * the window surface current and never switch. Switching costs two
     * eglMakeCurrent calls per frame, which is measurable on a software driver.
     */
    if (g_thread_state.window && g->window_surface != EGL_NO_SURFACE)
        gl_thread_bind(g, g->window_surface);
    else
        gl_thread_bind(g, g_thread_state.surface);
}

void host_gfx_gl_detach_window(HostGpuDevice device)
{
    HostGles *g = device->impl;

    if (!g)
        return;

    if (g->window_surface == EGL_NO_SURFACE)
        return;

    if (g_window_thread_valid && !pthread_equal(pthread_self(), g_window_thread))
        return;

    gl_thread_bind(g, g_thread_state.surface);
    g_thread_state.window = false;

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

    if (g_window_thread_valid && !pthread_equal(pthread_self(), g_window_thread))
        return;
    if (gl_thread_bind(g, g->window_surface) != 0)
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

        /* Draw the decorative border, then the aspect-fitted guest frame. */
        gles_present_rect(window_width, window_height, width, height, &viewport_x, &viewport_y,
                          &viewport_width, &viewport_height);
        gles_draw_border(g, window_width, window_height, viewport_x, viewport_y, viewport_width,
                         viewport_height);
        g->glActiveTexture(GL_TEXTURE0);
        g->glBindTexture(GL_TEXTURE_2D, g->present_texture);

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
    if (g->kms)
        host_kms_present_flip(g->kms);
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

    if (!g || !texture || width <= 0 || height <= 0)
        return;

    if (g->window_surface != EGL_NO_SURFACE) {
        surface = g->window_surface;
        if (g_window_thread_valid && !pthread_equal(pthread_self(), g_window_thread))
            return;
        if (gl_thread_bind(g, surface) != 0)
            return;
        if (!g->eglQuerySurface(g->display, surface, EGL_WIDTH, &window_width) ||
            !g->eglQuerySurface(g->display, surface, EGL_HEIGHT, &window_height) ||
            window_width <= 0 || window_height <= 0)
            return;
    } else {
        return;
    }

    if (!g->present_program && !gles_create_present_program(g))
        return;

    /* The frame is about to be presented: remember which thread did it so
       sceGxmEndScene can tell whether a pipeline drain is actually needed. */
    g->present_thread = pthread_self();
    g->present_thread_valid = true;

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
        GLint viewport_x, viewport_y, viewport_width, viewport_height;
        gles_present_rect(window_width, window_height, width, height, &viewport_x, &viewport_y,
                          &viewport_width, &viewport_height);
        gles_draw_border(g, window_width, window_height, viewport_x, viewport_y, viewport_width,
                         viewport_height);

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
        GLint viewport_x, viewport_y, viewport_width, viewport_height;
        gles_present_rect(window_width, window_height, width, height, &viewport_x, &viewport_y,
                          &viewport_width, &viewport_height);
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

    if (vita2native_env_flag("VITA2NATIVE_DEBUG_PRESENT")) {
        static unsigned int logged;
        GLenum error = g->glGetError();

        if (logged < 20) {
            fprintf(stderr, "SceGxm present texture %u %dx%d: window %dx%d error 0x%x\n",
                    texture, width, height, (int)window_width, (int)window_height, error);
            logged++;
        }
    }

    /*
     * The presenting context is not the rendering one: finish the blit before
     * the guest is allowed to overwrite the source texture. When the same
     * thread renders and presents, both use one EGL context and GL command
     * order already guarantees the blit reads this frame's contents, so skip
     * the drain and let the CPU run ahead.
     */
    if (!g->eglSwapBuffers(g->display, surface))
        fprintf(stderr, "vita2native: eglSwapBuffers failed: 0x%x\n", g->eglGetError());
    else if (g->kms)
        host_kms_present_flip(g->kms);
}
