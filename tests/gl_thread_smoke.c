/*
 * Runs the GLES backend from a worker thread, the way vita2native drives it:
 * the device is created on the main thread and guest threads do the rendering.
 *
 * The point is to catch per-thread context handling regressions (EGL contexts
 * and their drawing surfaces are per-thread) without needing guest code.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"
#include "host_gfx_gl.h"

typedef struct {
    HostGpuDevice device;
    int result;
} WorkerArgs;

static const char *const vertex_source =
    "#version 310 es\n"
    "layout(location = 0) in vec4 aPosition;\n"
    "void main() { gl_Position = aPosition; }\n";

static const char *const fragment_source =
    "#version 310 es\n"
    "precision mediump float;\n"
    "layout(location = 0) out vec4 oColor;\n"
    "void main() { oColor = vec4(0.0, 1.0, 0.0, 1.0); }\n";

/* Compiling a program here matters: llvmpipe runs its LLVM shader compiler on
   whichever thread issues the draw, which is a different stack than the one
   the device was created on. */
static int compile_program(HostGles *g)
{
    GLuint vs = g->glCreateShader(GL_VERTEX_SHADER);
    GLuint fs = g->glCreateShader(GL_FRAGMENT_SHADER);
    GLuint program;
    GLint ok = 0;

    g->glShaderSource(vs, 1, &vertex_source, NULL);
    g->glCompileShader(vs);
    g->glShaderSource(fs, 1, &fragment_source, NULL);
    g->glCompileShader(fs);

    program = g->glCreateProgram();
    g->glAttachShader(program, vs);
    g->glAttachShader(program, fs);
    g->glLinkProgram(program);
    g->glGetProgramiv(program, GL_LINK_STATUS, &ok);

    g->glDeleteShader(vs);
    g->glDeleteShader(fs);

    return ok ? 0 : -1;
}

static void *worker(void *arg)
{
    WorkerArgs *args = arg;
    uint8_t pixel[4];
    HostGles *g;
    int ret;

    args->result = -1;

    host_gpu_thread_enter(args->device);

    g = host_gpu_get_gles(args->device);
    if (!g || compile_program(g) != 0)
        return NULL;

    host_gpu_begin_frame(args->device);
    host_gpu_clear(args->device, 0.0f, 1.0f, 0.0f, 1.0f);

    ret = host_gpu_read_pixels(args->device, 8, 8, 1, 1, pixel, sizeof(pixel));
    if (ret != 0)
        return NULL;
    if (pixel[0] != 0 || pixel[1] != 255 || pixel[2] != 0 || pixel[3] != 255)
        return NULL;

    host_gpu_end_frame(args->device);
    host_gpu_finish(args->device);

    args->result = 0;
    return NULL;
}

int main(void)
{
    HostGpuConfig config = {320, 200, true};
    HostGpuDevice device = host_gpu_create(&config);
    WorkerArgs args = { device, -1 };
    pthread_t thread;
    pthread_attr_t attr;
    int ret;

    if (!device) {
        puts("gfx_smoke: no device");
        return 1;
    }

    printf("gfx backend: %s\n", host_gpu_description(device));

    if (host_gpu_backend(device) != HOST_GPU_BACKEND_GLES) {
        puts("gl_thread_smoke: no GLES backend, nothing to test");
        host_gpu_destroy(device);
        return 0;
    }

    /* Guest threads get large stacks in vita2native; match that here. */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    ret = pthread_create(&thread, &attr, worker, &args);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        puts("gl_thread_smoke: could not create the worker thread");
        host_gpu_destroy(device);
        return 1;
    }

    pthread_join(thread, NULL);

    host_gpu_destroy(device);

    if (args.result != 0) {
        puts("gl_thread_smoke: rendering from a worker thread failed");
        return 1;
    }

    puts("gl_thread_smoke: ok");
    return 0;
}
