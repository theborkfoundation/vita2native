#ifndef VITA2NATIVE_GFX_H
#define VITA2NATIVE_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct HostGpuDevice *HostGpuDevice;
typedef struct HostGpuMemBlock *HostGpuMemBlock;

typedef enum {
    HOST_GPU_BACKEND_NULL,
    HOST_GPU_BACKEND_GLES,
} HostGpuBackend;

typedef struct {
    int width;
    int height;
    bool headless;
} HostGpuConfig;

/*
 * Creates the GPU device. The GLES 3.x backend is loaded at runtime (EGL +
 * GLESv2), so the executable links without graphics libraries. If GLES is not
 * usable, a null backend is returned instead; check host_gpu_backend().
 *
 * Returns NULL only when memory allocation fails.
 */
HostGpuDevice host_gpu_create(const HostGpuConfig *config);
void host_gpu_destroy(HostGpuDevice device);

HostGpuBackend host_gpu_backend(HostGpuDevice device);
/* Human-readable backend/renderer/version, e.g. "GLES 3.2 (Mesa...)". */
const char *host_gpu_description(HostGpuDevice device);

/* Bind the offscreen color target and make it current for drawing. */
void host_gpu_begin_frame(HostGpuDevice device);
/* Finish the frame. Presentation is added together with the GXM backend. */
void host_gpu_end_frame(HostGpuDevice device);

void host_gpu_clear(HostGpuDevice device, float r, float g, float b, float a);

/* Read `width * height` RGBA8 pixels starting at (x, y). Returns 0 on success. */
int host_gpu_read_pixels(HostGpuDevice device, int x, int y, int width, int height, void *rgba,
                         size_t size);

void host_gpu_finish(HostGpuDevice device);

/*
 * Presentation.
 *
 * attach_window() takes a native window handle (an X11 Window, or a Wayland
 * wl_egl_window on platforms that support it) and makes an EGL window surface
 * current. Returns 0 on success, -1 when the backend or platform cannot
 * present to a window (the caller should fall back to CPU presentation).
 */
int host_gpu_attach_window(HostGpuDevice device, void *native_window);
void host_gpu_detach_window(HostGpuDevice device);

/*
 * Uploads an RGBA8 guest framebuffer (stride in pixels) and swaps the window.
 * Pass rgba == NULL to present a black frame. No-op without a window surface.
 */
void host_gpu_present_framebuffer(HostGpuDevice device, const void *rgba, int width, int height,
                                  int stride_in_pixels);

HostGpuMemBlock host_gpu_memblock_create(void *cpu_addr, size_t size);
void host_gpu_memblock_destroy(HostGpuMemBlock block);

#endif
