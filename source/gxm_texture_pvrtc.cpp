/*
 * PVRTC decoder shim.
 *
 * The actual decompressor is Vita3K's copy of the PowerVR SDK implementation
 * (MIT licensed): external/Vita3K/vita3k/renderer/src/texture/pvrt-dec.cpp.
 */
#include <renderer/pvrt-dec.h>

#include "gxm/texture_compressed.h"

extern "C" int gxm_texture_decode_pvrtc(const void *data, uint32_t width, uint32_t height,
                                        uint32_t two_bpp, uint32_t pvrt2, uint8_t *dest)
{
    if (!data || !dest || !width || !height)
        return -1;

    pvr::PVRTDecompressPVRTC(data, two_bpp ? 1 : 0, width, height, pvrt2 ? 1 : 0, dest);
    return 0;
}
