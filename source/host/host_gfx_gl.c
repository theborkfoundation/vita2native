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
#include <fcntl.h>
#include <pthread.h>
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

/*
 * Some handhelds ship a vendor GLES blob (ARM Bifrost) behind EGL 1.4 with no
 * surfaceless platform and no DRM render driver, so Mesa cannot create a
 * screen. The blob does accept a GBM device, so create one when libgbm is
 * available and offer it as a display candidate.
 */
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

    g->gbm_lib = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g->gbm_lib)
        g->gbm_lib = dlopen("libgbm.so", RTLD_NOW | RTLD_LOCAL);
    if (!g->gbm_lib)
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
            return g->gbm_device;
        }
        close(fd);
    }

    return NULL;
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
        if (g->surface != EGL_NO_SURFACE)
            g->eglDestroySurface(g->display, g->surface);
        if (g->context != EGL_NO_CONTEXT)
            g->eglDestroyContext(g->display, g->context);
        if (g->eglReleaseThread)
            g->eglReleaseThread();
        g->eglTerminate(g->display);
    }

    if (g->gbm_device) {
        typedef void (*gbm_device_destroy_fn)(void *device);
        gbm_device_destroy_fn destroy_device =
            g->gbm_lib ? (gbm_device_destroy_fn)dlsym(g->gbm_lib, "gbm_device_destroy") : NULL;

        if (destroy_device)
            destroy_device(g->gbm_device);
    }
    if (g->drm_fd >= 0)
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
     * the other options are fallbacks.
     */
    {
        EGLDisplay candidates[8];
        int num_candidates = 0;
        bool initialized = false;

#define GXM_ADD_SURFACELESS()                                                                      \
    do {                                                                                           \
        if (g->eglGetPlatformDisplay)                                                              \
            candidates[num_candidates++] = g->eglGetPlatformDisplay(                               \
                EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);                         \
        if (g->eglGetPlatformDisplayEXT)                                                           \
            candidates[num_candidates++] = g->eglGetPlatformDisplayEXT(                            \
                EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);                         \
    } while (0)

        if (config->headless) {
            GXM_ADD_SURFACELESS();
            candidates[num_candidates++] = g->eglGetDisplay(EGL_DEFAULT_DISPLAY);
        } else {
            candidates[num_candidates++] = g->eglGetDisplay(EGL_DEFAULT_DISPLAY);
            GXM_ADD_SURFACELESS();
        }
#undef GXM_ADD_SURFACELESS

        /* Vendor blobs on handhelds need a GBM device; Mesa ignores this when
           one of the platforms above already worked. */
        {
            void *gbm = gxm_gbm_device_create(g);

            if (gbm) {
                if (g->eglGetPlatformDisplayEXT)
                    candidates[num_candidates++] = g->eglGetPlatformDisplayEXT(
                        EGL_PLATFORM_GBM_KHR, (EGLNativeDisplayType)gbm, NULL);
                candidates[num_candidates++] = g->eglGetDisplay((EGLNativeDisplayType)gbm);
            }
        }

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
        "out vec4 o_color;\n"
        "void main() { o_color = texture(u_tex, vec2(v_uv.x, 1.0 - v_uv.y)); }\n";
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
    g->glUseProgram(0);

    if (!g->present_vao)
        g->glGenVertexArrays(1, &g->present_vao);

    g->present_program = program;
    return true;
}

int host_gfx_gl_attach_window(HostGpuDevice device, void *native_window)
{
    HostGles *g = device->impl;
    EGLSurface surface;

    if (!g || !native_window || !g->window_capable || !g->eglCreateWindowSurface)
        return -1;

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
    else
        gl_thread_bind(g, g_thread_state.surface);
}

void host_gfx_gl_detach_window(HostGpuDevice device)
{
    HostGles *g = device->impl;

    if (!g || g->window_surface == EGL_NO_SURFACE)
        return;

    gl_thread_bind(g, g_thread_state.surface);
    g_thread_state.window = false;
    g->eglDestroySurface(g->display, g->window_surface);
    g->window_surface = EGL_NO_SURFACE;
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
        g->glBindVertexArray(g->present_vao);
        g->glDrawArrays(GL_TRIANGLES, 0, 3);
        g->glBindVertexArray(0);
        g->glUseProgram(0);
        g->glBindTexture(GL_TEXTURE_2D, 0);
    }

    g->eglSwapBuffers(g->display, g->window_surface);
}
