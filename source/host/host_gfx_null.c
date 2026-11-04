/*
 * Null graphics backend.
 *
 * Used when GLES 3.x cannot be initialized (no EGL/GLESv2, no usable display).
 * It keeps CPU-only homebrew running and makes the failure visible through
 * host_gpu_description().
 */
#include <stdio.h>
#include <stdlib.h>

#include "host_gfx_internal.h"

HostGpuDevice host_gfx_null_create(const HostGpuConfig *config)
{
    HostGpuDevice device = calloc(1, sizeof(*device));

    if (!device)
        return NULL;

    device->backend = HOST_GPU_BACKEND_NULL;
    snprintf(device->description, sizeof(device->description), "null (%dx%d%s)", config->width,
             config->height, config->headless ? ", headless" : "");

    return device;
}
