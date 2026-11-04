#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gfx.h"

int main(void)
{
    /* The GPU device never aborts startup: without EGL/GLES it reports the
     * null backend, which is what headless machines and CI-like setups get. */
    HostGpuConfig config = {320, 200, true};
    HostGpuDevice device = host_gpu_create(&config);
    uint8_t pixel[4] = {0, 0, 0, 0};
    int ret;

    assert(device != NULL);

    printf("gfx backend: %s\n", host_gpu_description(device));

    host_gpu_begin_frame(device);
    host_gpu_clear(device, 1.0f, 0.0f, 0.0f, 1.0f);

    if (host_gpu_backend(device) == HOST_GPU_BACKEND_GLES) {
        /* Top-left and bottom-right corners must be cleared. */
        ret = host_gpu_read_pixels(device, 0, 0, 1, 1, pixel, sizeof(pixel));
        assert(ret == 0);
        assert(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);

        memset(pixel, 0, sizeof(pixel));
        ret = host_gpu_read_pixels(device, 319, 199, 1, 1, pixel, sizeof(pixel));
        assert(ret == 0);
        assert(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);

        printf("readback: %u %u %u %u\n", pixel[0], pixel[1], pixel[2], pixel[3]);
    } else {
        ret = host_gpu_read_pixels(device, 0, 0, 1, 1, pixel, sizeof(pixel));
        assert(ret != 0);
    }

    host_gpu_end_frame(device);
    host_gpu_finish(device);
    host_gpu_destroy(device);

    puts("gfx_smoke: ok");
    return 0;
}
