#ifndef VITA2NATIVE_HOST_GFX_INTERNAL_H
#define VITA2NATIVE_HOST_GFX_INTERNAL_H

#include "gfx.h"

struct HostGpuDevice {
    HostGpuBackend backend;
    char description[160];
    void *impl;
};

/* Null backend. */
HostGpuDevice host_gfx_null_create(const HostGpuConfig *config);

/* GLES backend returns NULL when EGL/GLESv2 cannot provide a GLES 3.x context. */
HostGpuDevice host_gfx_gl_create(const HostGpuConfig *config);
void host_gfx_gl_destroy(HostGpuDevice device);
void host_gfx_gl_begin_frame(HostGpuDevice device);
void host_gfx_gl_end_frame(HostGpuDevice device);
void host_gfx_gl_clear(HostGpuDevice device, float r, float g, float b, float a);
int host_gfx_gl_read_pixels(HostGpuDevice device, int x, int y, int width, int height, void *rgba,
                            size_t size);
void host_gfx_gl_finish(HostGpuDevice device);

#endif
