#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"

int main(void)
{
    /* The GPU device never aborts startup: without EGL/GLES it reports the
     * null backend, which is what headless machines and CI-like setups get. */
    HostGpuConfig config = {64, 64, true};
    HostGpuDevice device = host_gpu_create(&config);
    uint8_t pixel[4] = {0, 0, 0, 0};
    int ret;

    assert(device != NULL);

    printf("gfx backend: %s\n", host_gpu_description(device));

    host_gpu_clear(device, 1.0f, 0.0f, 0.0f, 1.0f);

    ret = host_gpu_read_pixels(device, 0, 0, 1, 1, pixel, sizeof(pixel));
    if (host_gpu_backend(device) == HOST_GPU_BACKEND_GLES) {
        assert(ret == 0);
        printf("readback: %u %u %u %u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
        assert(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);
    } else {
        assert(ret != 0);
    }

    host_gpu_finish(device);
    host_gpu_destroy(device);

    puts("gfx_smoke: ok");
    return 0;
}
