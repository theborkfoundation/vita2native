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
void host_gfx_gl_thread_enter(HostGpuDevice device);
int host_gfx_gl_attach_window(HostGpuDevice device, void *native_window);
int host_gfx_gl_attach_kms(HostGpuDevice device, int drm_fd, int width, int height);
void host_gfx_gl_detach_window(HostGpuDevice device);
void host_gfx_gl_present_framebuffer(HostGpuDevice device, const void *rgba, int width, int height,
                                     int stride_in_pixels);
void host_gfx_gl_present_texture(HostGpuDevice device, unsigned int texture, int width, int height);
void host_gfx_gl_set_overlay(HostGpuDevice device, const void *rgba, int width, int height);

#endif
