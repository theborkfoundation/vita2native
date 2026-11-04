#include "gxm/texture.h"

#include <string.h>

/* See Vita3K's renderer/src/texture/format.cpp (Morton codes). */
static uint32_t compact_one_by_one(uint32_t x)
{
    x &= 0x55555555u;
    x = (x ^ (x >> 1)) & 0x33333333u;
    x = (x ^ (x >> 2)) & 0x0f0f0f0fu;
    x = (x ^ (x >> 4)) & 0x00ff00ffu;
    x = (x ^ (x >> 8)) & 0x0000ffffu;
    return x;
}

static uint32_t decode_morton2_x(uint32_t code)
{
    return compact_one_by_one(code >> 1);
}

static uint32_t decode_morton2_y(uint32_t code)
{
    return compact_one_by_one(code);
}

static uint32_t highest_bit(uint32_t v)
{
    uint32_t bit = 0;
    while (v >>= 1)
        bit++;
    return bit;
}

uint32_t gxm_texture_format_bytes(SceGxmTextureFormat format)
{
    switch (format & 0x9F000000u) {
    case 0x00000000u: /* U8 */
        return 1;
    case 0x02000000u: /* U4U4U4U4 */
    case 0x05000000u: /* U5U6U5 */
    case 0x01000000u: /* U1U5U5U5 */
    case 0x03000000u: /* U5U5U5U1 */
        return 2;
    case 0x0C000000u: /* U8U8U8U8 */
        return 4;
    default:
        return 0;
    }
}

/* Expand one source pixel to RGBA8. Returns 0 on success. */
static int decode_pixel(uint8_t out[4], const uint8_t *src, SceGxmTextureFormat format)
{
    uint32_t base = format & 0x9F000000u;
    uint16_t v16;

    switch (base) {
    case 0x0C000000u: { /* U8U8U8U8, byte order depends on the swizzle field */
        const uint8_t *a = src;
        switch (format) {
        case SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR: /* A B G R */
            out[0] = a[3];
            out[1] = a[2];
            out[2] = a[1];
            out[3] = a[0];
            break;
        case SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB: /* A R G B */
            out[0] = a[1];
            out[1] = a[2];
            out[2] = a[3];
            out[3] = a[0];
            break;
        case SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_BGRA: /* B G R A */
            out[0] = a[2];
            out[1] = a[1];
            out[2] = a[0];
            out[3] = a[3];
            break;
        default: /* RGBA */
            memcpy(out, a, 4);
            break;
        }
        return 0;
    }

    case 0x05000000u: { /* U5U6U5 (RGB565) */
        v16 = (uint16_t)(src[0] | (src[1] << 8));
        uint8_t r = (uint8_t)((v16 >> 11) & 0x1F);
        uint8_t g = (uint8_t)((v16 >> 5) & 0x3F);
        uint8_t b = (uint8_t)(v16 & 0x1F);
        out[0] = (uint8_t)((r << 3) | (r >> 2));
        out[1] = (uint8_t)((g << 2) | (g >> 4));
        out[2] = (uint8_t)((b << 3) | (b >> 2));
        out[3] = 255;
        return 0;
    }

    case 0x02000000u: { /* U4U4U4U4, ARGB nibbles */
        v16 = (uint16_t)(src[0] | (src[1] << 8));
        out[0] = (uint8_t)(((v16 >> 8) & 0xF) * 17);
        out[1] = (uint8_t)(((v16 >> 4) & 0xF) * 17);
        out[2] = (uint8_t)((v16 & 0xF) * 17);
        out[3] = (uint8_t)(((v16 >> 12) & 0xF) * 17);
        return 0;
    }

    case 0x00000000u: /* U8 */
        out[0] = out[1] = out[2] = src[0];
        out[3] = 255;
        return 0;

    default:
        return -1;
    }
}

int gxm_texture_decode_rgba8(uint8_t *dest, const void *src, uint32_t width, uint32_t height,
                             SceGxmTextureFormat format, SceGxmTextureType type)
{
    const uint8_t *input = src;
    uint32_t bpp = gxm_texture_format_bytes(format);

    if (!dest || !src || !width || !height || !bpp)
        return -1;

    if (type == SCE_GXM_TEXTURE_SWIZZLED || type == SCE_GXM_TEXTURE_CUBE) {
        uint32_t min = width < height ? width : height;
        uint32_t k = highest_bit(min);
        uint32_t count = width * height;

        if (!min)
            return -1;

        for (uint32_t i = 0; i < count; i++) {
            uint32_t x = decode_morton2_x(i) & (min - 1);
            uint32_t y = decode_morton2_y(i) & (min - 1);
            uint32_t upper = (i >> (2 * k)) << k;

            if (width >= height)
                x |= upper;
            else
                y |= upper;

            if (x >= width || y >= height)
                continue;

            if (decode_pixel(&dest[(y * width + x) * 4], &input[i * bpp], format) != 0)
                return -1;
        }

        return 0;
    }

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            if (decode_pixel(&dest[(y * width + x) * 4], &input[(y * width + x) * bpp], format) != 0)
                return -1;
        }
    }

    return 0;
}
