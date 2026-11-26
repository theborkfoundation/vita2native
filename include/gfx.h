#ifndef VITA2NATIVE_GFX_H
#define VITA2NATIVE_GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host.h"

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

/* Device created by host_gpu_create(); NULL before creation. */
HostGpuDevice host_gpu_current(void);

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
 * Establishes a GLES context for the calling thread.
 *
 * EGL contexts are per-thread, and guest threads are host threads, so any
 * guest thread that issues GXM calls must call this first. Threads other than
 * the one that created the device get a context that shares objects (textures,
 * buffers, shaders) with the primary one. Calling it repeatedly is cheap.
 */
void host_gpu_thread_enter(HostGpuDevice device);

/*
 * The direct-present path needs to know whether the frame being presented was
 * rendered on the calling thread. When it was, the blit is ordered after the
 * scene draws in the same GL context and the frame does not need a full
 * pipeline drain before it. host_gpu_note_render_thread() is called at
 * sceGxmEndScene, host_gpu_last_present_is_current_thread() returns true when
 * the last direct presentation ran on the calling thread.
 */
void host_gpu_note_render_thread(HostGpuDevice device);
bool host_gpu_last_present_is_current_thread(HostGpuDevice device);

/*
 * Presentation.
 *
 * attach_window() takes the window SDL created and makes an EGL window surface
 * current on it. The backend builds whatever EGL needs from it (an X11 Window
 * is used directly; a Wayland wl_surface gets a wl_egl_window). Returns 0 on
 * success, -1 when the platform cannot present to that window, in which case
 * the caller falls back to CPU presentation.
 */

int host_gpu_attach_window(HostGpuDevice device, const HostWindowInfo *window);
void host_gpu_detach_window(HostGpuDevice device);

/*
 * Uploads an RGBA8 guest framebuffer (stride in pixels) and swaps the window.
 * Pass rgba == NULL to present a black frame. No-op without a window surface.
 */
void host_gpu_present_framebuffer(HostGpuDevice device, const void *rgba, int width, int height,
                                  int stride_in_pixels);

/*
 * Presents an existing GL texture owned by `device` and swaps the window.
 * Used by the direct-present path to skip the GPU-to-CPU readback entirely.
 */
void host_gpu_present_texture(HostGpuDevice device, unsigned int texture, int width, int height);

/*
 * Reads `framebuffer` back through a linear dma-buf and returns its CPU mapping.
 * Rows are bottom-up like glReadPixels, `stride` is in bytes, and the pointer is
 * valid until the next call. Returns false when the backend cannot do it (no
 * render node, software GL), in which case the caller falls back to
 * glReadPixels.
 */
bool host_gpu_read_target(HostGpuDevice device, unsigned int framebuffer, int width, int height,
                          const void **pixels, unsigned int *stride);

/*
 * Sets an RGBA8 debug overlay drawn on top of the next directly presented
 * frames. Pass NULL to clear it. The caller keeps ownership of the pixels; the
 * buffer must stay valid until the next present.
 */
void host_gpu_set_overlay(HostGpuDevice device, const void *rgba, int width, int height);

HostGpuMemBlock host_gpu_memblock_create(void *cpu_addr, size_t size);
void host_gpu_memblock_destroy(HostGpuMemBlock block);

#endif
