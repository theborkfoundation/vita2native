#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/gxm.h>

#include "gxm/texture.h"

static uint32_t part1by1(uint32_t x)
{
    x &= 0x0000ffffu;
    x = (x ^ (x << 8)) & 0x00ff00ffu;
    x = (x ^ (x << 4)) & 0x0f0f0f0fu;
    x = (x ^ (x << 2)) & 0x33333333u;
    x = (x ^ (x << 1)) & 0x55555555u;
    return x;
}

static uint32_t encode_morton(uint32_t x, uint32_t y, uint32_t min, uint32_t k)
{
    return (((x >> k) | (y >> k)) << (2 * k)) | (part1by1(x & (min - 1)) << 1) |
           part1by1(y & (min - 1));
}

int main(void)
{
    /* Format conversion: ABGR bytes -> RGBA8. */
    {
        const uint8_t abgr[4] = {10, 20, 30, 40};
        uint8_t rgba[4];
        assert(gxm_texture_decode_rgba8(rgba, abgr, 1, 1, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
                                        SCE_GXM_TEXTURE_LINEAR) == 0);
        assert(rgba[0] == 40 && rgba[1] == 30 && rgba[2] == 20 && rgba[3] == 10);
    }

    /* Swizzle round trip: 4x4 U8 image. */
    {
        uint8_t linear[16];
        uint8_t swizzled[16];
        uint8_t decoded[16 * 4];

        for (uint32_t y = 0; y < 4; y++)
            for (uint32_t x = 0; x < 4; x++)
                linear[y * 4 + x] = (uint8_t)(y * 4 + x + 1);

        for (uint32_t y = 0; y < 4; y++)
            for (uint32_t x = 0; x < 4; x++)
                swizzled[encode_morton(x, y, 4, 2)] = linear[y * 4 + x];

        assert(gxm_texture_decode_rgba8(decoded, swizzled, 4, 4, SCE_GXM_TEXTURE_FORMAT_U8_000R,
                                        SCE_GXM_TEXTURE_SWIZZLED) == 0);

        for (uint32_t i = 0; i < 16; i++) {
            assert(decoded[i * 4 + 0] == linear[i]);
            assert(decoded[i * 4 + 3] == 255);
        }
    }

    puts("texture_smoke: ok");
    return 0;
}
