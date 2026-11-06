/*
 * Measures the raw cost of the operations SceGxm performs every frame on the
 * host GPU: a full-surface clear, a 960x544 readback (direct and through a
 * pixel-pack buffer) and bursts of small draws with different uniform update
 * strategies. This separates driver overhead from the GXM layer when tuning
 * the port on real hardware.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gfx.h"
#include "host_gfx_gl.h"

#define WIDTH 960
#define HEIGHT 544
#define DRAWS 847
#define FRAMES 5

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static GLuint compile(HostGles *gl, GLenum stage, const char *source)
{
    GLuint shader = gl->glCreateShader(stage);
    GLint ok = 0;

    gl->glShaderSource(shader, 1, &source, NULL);
    gl->glCompileShader(shader);
    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];

        gl->glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "compile failed: %s\n", log);
    }
    return shader;
}

int main(void)
{
    static const char *vert_src =
        "#version 310 es\n"
        "layout(location = 0) in vec2 a_pos;\n"
        "layout(binding = 0) uniform Blk { vec4 offset_colour; } blk;\n"
        "uniform vec4 u_colour;\n"
        "void main() { gl_Position = vec4(a_pos * 0.02 + blk.offset_colour.xy, 0.0, 1.0); }\n";
    static const char *frag_src =
        "#version 310 es\n"
        "precision mediump float;\n"
        "layout(binding = 0) uniform Blk { vec4 offset_colour; } blk;\n"
        "uniform vec4 u_colour;\n"
        "out vec4 o_colour;\n"
        "void main() { o_colour = vec4(u_colour.r, blk.offset_colour.z, 0.5, 1.0); }\n";
    HostGpuConfig config = {WIDTH, HEIGHT, true};
    HostGpuDevice device = host_gpu_create(&config);
    HostGles *gl;
    GLuint fbo, tex, program, vbo, ibo, ubo, pbo[2];
    GLint uniform_location;
    static const float quad[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
    static const uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    uint8_t *pixels;
    double t0, t1;
    uint32_t i;

    if (!device || host_gpu_backend(device) != HOST_GPU_BACKEND_GLES) {
        fprintf(stderr, "GPU bench needs a GLES backend\n");
        return 1;
    }

    setvbuf(stdout, NULL, _IONBF, 0);

    gl = host_gpu_get_gles(device);
    if (!gl)
        return 1;

    printf("backend: %s\n", host_gpu_description(device));

    gl->glViewport(0, 0, WIDTH, HEIGHT);

    gl->glGenTextures(1, &tex);
    gl->glBindTexture(GL_TEXTURE_2D, tex);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, WIDTH, HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     NULL);
    gl->glGenFramebuffers(1, &fbo);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FBO incomplete\n");
        return 1;
    }

    program = gl->glCreateProgram();
    gl->glAttachShader(program, compile(gl, GL_VERTEX_SHADER, vert_src));
    gl->glAttachShader(program, compile(gl, GL_FRAGMENT_SHADER, frag_src));
    gl->glLinkProgram(program);
    gl->glUseProgram(program);
    uniform_location = gl->glGetUniformLocation(program, "u_colour");

    gl->glGenBuffers(1, &vbo);
    gl->glBindBuffer(GL_ARRAY_BUFFER, vbo);
    gl->glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    gl->glEnableVertexAttribArray(0);
    gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    gl->glGenBuffers(1, &ibo);
    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

    /* One fixed uniform block, as if every draw shared the same range. */
    gl->glGenBuffers(1, &ubo);
    gl->glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    gl->glBufferData(GL_UNIFORM_BUFFER, 4096, NULL, GL_STREAM_DRAW);
    gl->glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);

    gl->glGenBuffers(2, pbo);
    for (i = 0; i < 2; i++) {
        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i]);
        gl->glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)WIDTH * HEIGHT * 4, NULL,
                         GL_STREAM_READ);
    }
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    pixels = malloc((size_t)WIDTH * HEIGHT * 4);
    if (!pixels)
        return 1;

    gl->glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
    gl->glClear(GL_COLOR_BUFFER_BIT);
    gl->glFinish();

    t0 = now_ms();
    for (i = 0; i < FRAMES; i++) {
        gl->glClear(GL_COLOR_BUFFER_BIT);
        gl->glFinish();
    }
    t1 = now_ms();
    printf("clear+finish: %.2f ms\n", (t1 - t0) / FRAMES);

    t0 = now_ms();
    for (i = 0; i < FRAMES; i++)
        gl->glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    t1 = now_ms();
    printf("readback 1x1: %.2f ms\n", (t1 - t0) / FRAMES);

    t0 = now_ms();
    for (i = 0; i < FRAMES; i++)
        gl->glReadPixels(0, 0, 480, 272, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    t1 = now_ms();
    printf("readback 480x272: %.2f ms\n", (t1 - t0) / FRAMES);

    t0 = now_ms();
    for (i = 0; i < FRAMES; i++)
        gl->glReadPixels(0, 0, 240, 136, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    t1 = now_ms();
    printf("readback 240x136: %.2f ms\n", (t1 - t0) / FRAMES);

    t0 = now_ms();
    for (i = 0; i < FRAMES; i++)
        gl->glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    t1 = now_ms();
    printf("readback direct: %.2f ms\n", (t1 - t0) / FRAMES);

    t0 = now_ms();
    for (i = 0; i < FRAMES; i++) {
        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[0]);
        gl->glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    gl->glFinish();
    t1 = now_ms();
    printf("readback into PBO (no map): %.2f ms\n", (t1 - t0) / FRAMES);

    /* Double-buffered PBO readback: the GPU writes frame N while the CPU
       consumes frame N-1. */
    t0 = now_ms();
    for (i = 0; i < FRAMES; i++) {
        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i & 1]);
        gl->glReadPixels(0, 0, WIDTH, HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, 0);
        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[(i & 1) ^ 1]);
        if (i > 0) {
            void *mapped = gl->glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, WIDTH * HEIGHT * 4,
                                                GL_MAP_READ_BIT);

            if (mapped) {
                memcpy(pixels, mapped, (size_t)WIDTH * HEIGHT * 4);
                gl->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
        }
        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    t1 = now_ms();
    printf("readback PBO double-buffered: %.2f ms\n", (t1 - t0) / FRAMES);

    t0 = now_ms();
    for (i = 0; i < FRAMES; i++) {
        static const float colour[4] = { 0.0f, 0.0f, 0.5f, 1.0f };
        uint32_t slot;

        gl->glClear(GL_COLOR_BUFFER_BIT);
        for (slot = 0; slot < DRAWS; slot++) {
            uint32_t offset = (slot % 16) * 256;

            gl->glBindBuffer(GL_UNIFORM_BUFFER, ubo);
            gl->glBufferSubData(GL_UNIFORM_BUFFER, (GLintptr)offset, sizeof(colour), colour);
            gl->glBindBufferRange(GL_UNIFORM_BUFFER, 0, ubo, (GLintptr)offset, sizeof(colour));
            gl->glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        }
        gl->glFinish();
    }
    t1 = now_ms();
    printf("%u draws + per-draw UBO range: %.2f ms/frame\n", DRAWS, (t1 - t0) / FRAMES);

    t0 = now_ms();
    for (i = 0; i < FRAMES; i++) {
        static const float colour[4] = { 0.0f, 0.0f, 0.5f, 1.0f };
        uint32_t slot;

        gl->glClear(GL_COLOR_BUFFER_BIT);
        for (slot = 0; slot < DRAWS; slot++) {
            gl->glBindBuffer(GL_UNIFORM_BUFFER, ubo);
            gl->glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(colour), colour);
            gl->glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        }
        gl->glFinish();
    }
    t1 = now_ms();
    printf("%u draws + per-draw UBO subdata (fixed binding): %.2f ms/frame\n", DRAWS,
           (t1 - t0) / FRAMES);

    t0 = now_ms();
    for (i = 0; i < FRAMES; i++) {
        static const float colour[4] = { 0.0f, 0.0f, 0.5f, 1.0f };
        uint32_t slot;

        gl->glClear(GL_COLOR_BUFFER_BIT);
        for (slot = 0; slot < DRAWS; slot++) {
            gl->glUniform4fv(uniform_location, 1, colour);
            gl->glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        }
        gl->glFinish();
    }
    t1 = now_ms();
    printf("%u draws + per-draw glUniform4fv: %.2f ms/frame\n", DRAWS, (t1 - t0) / FRAMES);

    t0 = now_ms();
    for (i = 0; i < FRAMES; i++) {
        uint32_t slot;

        gl->glClear(GL_COLOR_BUFFER_BIT);
        for (slot = 0; slot < DRAWS; slot++)
            gl->glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
        gl->glFinish();
    }
    t1 = now_ms();
    printf("%u draws + static uniforms: %.2f ms/frame\n", DRAWS, (t1 - t0) / FRAMES);

    free(pixels);
    host_gpu_destroy(device);

    return 0;
}
