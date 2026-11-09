#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_gfx_internal.h"

struct HostGpuMemBlock {
    void *cpu_addr;
    size_t size;
};

static HostGpuDevice g_host_gpu;

HostGpuDevice host_gpu_current(void)
{
    return g_host_gpu;
}

HostGpuDevice host_gpu_create(const HostGpuConfig *config)
{
    static const HostGpuConfig defaults = {640, 480, true};
    HostGpuDevice device;

    if (!config)
        config = &defaults;

    /*
     * Panfrost's default tiled/AFBC layouts make glReadPixels roughly twice as
     * slow (195 ns/pixel on Mali-G31), and this port reads the whole frame
     * back every frame for sceDisplaySetFrameBuf. Linear layouts only affect
     * Panfrost, and a caller-supplied PAN_MESA_DEBUG wins. Direct presentation
     * removes the readback, so it keeps the faster tiled layout.
     */
    if (!getenv("VITA2NATIVE_DIRECT_PRESENT") && !getenv("PAN_MESA_DEBUG"))
        setenv("PAN_MESA_DEBUG", "linear", 0);

    device = host_gfx_gl_create(config);
    if (device) {
        g_host_gpu = device;
        return device;
    }

    device = host_gfx_null_create(config);
    if (device) {
        g_host_gpu = device;
        fprintf(stderr, "vita2native: GLES 3.x unavailable, using the null graphics backend\n");
        fflush(stderr);
    }

    return device;
}

void host_gpu_destroy(HostGpuDevice device)
{
    if (!device)
        return;

    if (device->backend == HOST_GPU_BACKEND_GLES)
        host_gfx_gl_destroy(device);
    else
        free(device);
}

HostGpuBackend host_gpu_backend(HostGpuDevice device)
{
    return device ? device->backend : HOST_GPU_BACKEND_NULL;
}

const char *host_gpu_description(HostGpuDevice device)
{
    return device ? device->description : "unavailable";
}

void host_gpu_begin_frame(HostGpuDevice device)
{
    if (!device)
        return;

    if (device->backend == HOST_GPU_BACKEND_GLES)
        host_gfx_gl_begin_frame(device);
}

void host_gpu_end_frame(HostGpuDevice device)
{
    if (!device)
        return;

    if (device->backend == HOST_GPU_BACKEND_GLES)
        host_gfx_gl_end_frame(device);
}

void host_gpu_clear(HostGpuDevice device, float r, float g, float b, float a)
{
    if (!device)
        return;

    if (device->backend == HOST_GPU_BACKEND_GLES)
        host_gfx_gl_clear(device, r, g, b, a);
}

int host_gpu_read_pixels(HostGpuDevice device, int x, int y, int width, int height, void *rgba,
                         size_t size)
{
    if (!device || device->backend != HOST_GPU_BACKEND_GLES)
        return -1;

    return host_gfx_gl_read_pixels(device, x, y, width, height, rgba, size);
}

void host_gpu_finish(HostGpuDevice device)
{
    if (!device)
        return;

    if (device->backend == HOST_GPU_BACKEND_GLES)
        host_gfx_gl_finish(device);
}

void host_gpu_thread_enter(HostGpuDevice device)
{
    if (!device || device->backend != HOST_GPU_BACKEND_GLES)
        return;

    host_gfx_gl_thread_enter(device);
}

void host_gpu_note_render_thread(HostGpuDevice device)
{
    if (!device || device->backend != HOST_GPU_BACKEND_GLES)
        return;

    host_gfx_gl_note_render_thread(device);
}

bool host_gpu_last_present_is_current_thread(HostGpuDevice device)
{
    if (!device || device->backend != HOST_GPU_BACKEND_GLES)
        return false;

    return host_gfx_gl_last_present_is_current_thread(device);
}

int host_gpu_attach_window(HostGpuDevice device, void *native_window)
{
    if (!device || device->backend != HOST_GPU_BACKEND_GLES)
        return -1;

    return host_gfx_gl_attach_window(device, native_window);
}

int host_gpu_attach_kms(HostGpuDevice device, int drm_fd, int width, int height)
{
    if (!device || device->backend != HOST_GPU_BACKEND_GLES)
        return -1;

    return host_gfx_gl_attach_kms(device, drm_fd, width, height);
}

void host_gpu_detach_window(HostGpuDevice device)
{
    if (!device)
        return;

    if (device->backend == HOST_GPU_BACKEND_GLES)
        host_gfx_gl_detach_window(device);
}

void host_gpu_present_framebuffer(HostGpuDevice device, const void *rgba, int width, int height,
                                  int stride_in_pixels)
{
    if (!device)
        return;

    if (device->backend == HOST_GPU_BACKEND_GLES)
        host_gfx_gl_present_framebuffer(device, rgba, width, height, stride_in_pixels);
}

void host_gpu_present_texture(HostGpuDevice device, unsigned int texture, int width, int height)
{
    if (!device)
        return;

    if (device->backend == HOST_GPU_BACKEND_GLES)
        host_gfx_gl_present_texture(device, texture, width, height);
}

void host_gpu_set_overlay(HostGpuDevice device, const void *rgba, int width, int height)
{
    if (!device)
        return;

    if (device->backend == HOST_GPU_BACKEND_GLES)
        host_gfx_gl_set_overlay(device, rgba, width, height);
}

HostGpuMemBlock host_gpu_memblock_create(void *cpu_addr, size_t size)
{
    HostGpuMemBlock block = malloc(sizeof(*block));

    if (!block)
        return NULL;

    block->cpu_addr = cpu_addr;
    block->size = size;

    return block;
}

void host_gpu_memblock_destroy(HostGpuMemBlock block)
{
    free(block);
}
