/*
 * PVRTC decoder shim.
 *
 * The actual decompressor is Vita3K's copy of the PowerVR SDK implementation
 * (MIT licensed): external/Vita3K/vita3k/renderer/src/texture/pvrt-dec.cpp.
 * PVRTC-I 4bpp goes through the optimised decoder in gxm_texture_pvrtc_fast.c;
 * the other variants stay on the reference implementation.
 */
#include <stdlib.h>

#include <renderer/pvrt-dec.h>

#include "gxm/texture_compressed.h"

extern "C" int gxm_texture_decode_pvrtc_reference(const void *data, uint32_t width, uint32_t height,
                                                  uint32_t two_bpp, uint32_t pvrt2, uint8_t *dest)
{
    if (!data || !dest || !width || !height)
        return -1;

    pvr::PVRTDecompressPVRTC(data, two_bpp ? 1 : 0, width, height, pvrt2 ? 1 : 0, dest);
    return 0;
}

extern "C" int gxm_texture_decode_pvrtc(const void *data, uint32_t width, uint32_t height,
                                        uint32_t two_bpp, uint32_t pvrt2, uint8_t *dest)
{
    static int fast_enabled = -1;

    if (!data || !dest || !width || !height)
        return -1;

    if (fast_enabled < 0)
        fast_enabled = getenv("VITA2NATIVE_NO_PVRTC_FAST") == NULL;

    if (fast_enabled && !two_bpp && !pvrt2 &&
        gxm_texture_decode_pvrtc4_fast(data, width, height, dest) == 0)
        return 0;

    pvr::PVRTDecompressPVRTC(data, two_bpp ? 1 : 0, width, height, pvrt2 ? 1 : 0, dest);
    return 0;
}
