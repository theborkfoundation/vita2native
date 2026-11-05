/*
 * GXM texture decoding.
 *
 * The guest uploads textures in Vita layouts: linear (optionally strided),
 * Morton-swizzled, arbitrary-swizzled, 32x32 tiled, or cube. Formats range from
 * single-channel 8-bit to packed 16-bit and floating point. This file converts a
 * level into tightly packed RGBA8, which is what the GLES backend uploads.
 *
 * The GXM format name encodes the component order from the most significant to
 * the least significant bit of the texel word (for example U8U8U8U8_ABGR means
 * A in bits 24..31 down to R in bits 0..7; little-endian memory therefore holds
 * R, G, B, A). Swizzle bits select a different letter order for the same base
 * format. Paletted formats look the index up in an A8B8G8R8 palette.
 */
#include "gxm/texture.h"
#include "gxm/texture_compressed.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers */

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return alignment ? (value + alignment - 1) / alignment * alignment : value;
}

static uint32_t next_pow2(uint32_t value)
{
    uint32_t result = 1;
    while (result < value)
        result <<= 1;
    return result;
}

static uint32_t highest_bit(uint32_t value)
{
    uint32_t bit = 0;
    while (value >>= 1)
        bit++;
    return bit;
}

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

static uint8_t clamp_u8(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (uint8_t)value;
}

/* --------------------------------------------------------------- formats */

enum {
    KIND_UNORM,
    KIND_SNORM,
    KIND_F16,
    KIND_F32,
    KIND_F32M,
    KIND_FLOAT10,
    KIND_FLOAT11,
};

typedef struct {
    uint8_t count;
    uint8_t total_bits;
    uint8_t width[4];
    uint8_t kind[4];
} FormatInfo;

static const char *const swizzle4_letters[8] = {
    "ABGR", "ARGB", "RGBA", "BGRA", "1BGR", "1RGB", "RGB1", "BGR1",
};

static const char *const swizzle3_letters[2] = {
    "BGR",
    "RGB",
};

static uint32_t base_format(SceGxmTextureFormat format)
{
    return format & SCE_GXM_TEXTURE_BASE_FORMAT_MASK;
}

uint32_t gxm_texture_base_bits(SceGxmTextureFormat format)
{
    switch (base_format(format)) {
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_P8:
        return 8;

    case SCE_GXM_TEXTURE_BASE_FORMAT_P4:
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC1:
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC4:
    case SCE_GXM_TEXTURE_BASE_FORMAT_SBC4:
    case SCE_GXM_TEXTURE_BASE_FORMAT_PVRT2BPP:
    case SCE_GXM_TEXTURE_BASE_FORMAT_PVRTII2BPP:
        return 4;

    case SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U3U3U2:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U5U6U5:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S5S5U6:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_YUV422:
    case SCE_GXM_TEXTURE_BASE_FORMAT_PVRT4BPP:
    case SCE_GXM_TEXTURE_BASE_FORMAT_PVRTII4BPP:
        return 16;

    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8:
        return 24;

    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8S8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U2U10U10U10:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16U16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S16S16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F16F16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32M:
    case SCE_GXM_TEXTURE_BASE_FORMAT_X8S8S8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_X8U24:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_SE5M9M9M9:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F11F11F10:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U2F10F10F10:
        return 32;

    case SCE_GXM_TEXTURE_BASE_FORMAT_YUV420P2:
    case SCE_GXM_TEXTURE_BASE_FORMAT_YUV420P3:
        return 12;

    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC2:
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC3:
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC5:
    case SCE_GXM_TEXTURE_BASE_FORMAT_SBC5:
        return 32;

    case SCE_GXM_TEXTURE_BASE_FORMAT_F16F16F16F16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16U16U16U16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S16S16S16S16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32F32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U32U32:
        return 64;

    default:
        return 0;
    }
}

uint32_t gxm_texture_format_bytes(SceGxmTextureFormat format)
{
    return (gxm_texture_base_bits(format) + 7) / 8;
}

static int format_info(uint32_t base, uint32_t swizzle, FormatInfo *info)
{
    memset(info, 0, sizeof(*info));

    switch (base) {
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32M:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S32:
        info->count = 1;
        info->total_bits = (uint8_t)gxm_texture_base_bits((SceGxmTextureFormat)base);
        info->width[0] = info->total_bits;
        info->kind[0] = base == SCE_GXM_TEXTURE_BASE_FORMAT_S8 ||
                                base == SCE_GXM_TEXTURE_BASE_FORMAT_S16 ||
                                base == SCE_GXM_TEXTURE_BASE_FORMAT_S32
                            ? KIND_SNORM
                        : base == SCE_GXM_TEXTURE_BASE_FORMAT_F16 ? KIND_F16
                        : base == SCE_GXM_TEXTURE_BASE_FORMAT_F32 ? KIND_F32
                        : base == SCE_GXM_TEXTURE_BASE_FORMAT_F32M ? KIND_F32M
                                                                   : KIND_UNORM;
        return 0;

    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16U16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S16S16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F16F16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F32F32:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U32U32: {
        uint8_t bits = (uint8_t)(gxm_texture_base_bits((SceGxmTextureFormat)base) / 2);
        info->count = 2;
        info->total_bits = bits * 2;
        info->width[0] = info->width[1] = bits;
        info->kind[0] = info->kind[1] =
            base == SCE_GXM_TEXTURE_BASE_FORMAT_S8S8 || base == SCE_GXM_TEXTURE_BASE_FORMAT_S16S16
                ? KIND_SNORM
            : base == SCE_GXM_TEXTURE_BASE_FORMAT_F16F16 ? KIND_F16
            : base == SCE_GXM_TEXTURE_BASE_FORMAT_F32F32 ? KIND_F32
                                                         : KIND_UNORM;
        return 0;
    }

    case SCE_GXM_TEXTURE_BASE_FORMAT_U5U6U5:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S5S5U6:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8:
        info->count = 3;
        info->total_bits = (uint8_t)gxm_texture_base_bits((SceGxmTextureFormat)base);
        if (base == SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8) {
            info->width[0] = info->width[1] = info->width[2] = 8;
            info->kind[0] = info->kind[1] = info->kind[2] = KIND_UNORM;
        } else if (base == SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8) {
            info->width[0] = info->width[1] = info->width[2] = 8;
            info->kind[0] = info->kind[1] = info->kind[2] = KIND_SNORM;
        } else if (base == SCE_GXM_TEXTURE_BASE_FORMAT_U5U6U5) {
            info->width[0] = 5;
            info->width[1] = 6;
            info->width[2] = 5;
            info->kind[0] = info->kind[1] = info->kind[2] = KIND_UNORM;
        } else {
            info->width[0] = 5;
            info->width[1] = 5;
            info->width[2] = 6;
            info->kind[0] = KIND_SNORM;
            info->kind[1] = KIND_SNORM;
            info->kind[2] = KIND_UNORM;
        }
        return 0;

    case SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U2U10U10U10:
        info->count = 4;
        info->total_bits = (uint8_t)gxm_texture_base_bits((SceGxmTextureFormat)base);
        if (base == SCE_GXM_TEXTURE_BASE_FORMAT_U4U4U4U4) {
            info->width[0] = info->width[1] = info->width[2] = info->width[3] = 4;
            info->kind[0] = info->kind[1] = info->kind[2] = info->kind[3] = KIND_UNORM;
        } else if (base == SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5) {
            info->width[0] = 1;
            info->width[1] = info->width[2] = info->width[3] = 5;
            info->kind[0] = info->kind[1] = info->kind[2] = info->kind[3] = KIND_UNORM;
        } else {
            info->width[0] = 2;
            info->width[1] = info->width[2] = info->width[3] = 10;
            info->kind[0] = KIND_UNORM;
            info->kind[1] = info->kind[2] = info->kind[3] = KIND_UNORM;
        }
        return 0;

    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U3U3U2:
        info->count = 4;
        info->total_bits = 16;
        info->width[0] = 8;
        info->width[1] = info->width[2] = 3;
        info->width[3] = 2;
        info->kind[0] = info->kind[1] = info->kind[2] = info->kind[3] = KIND_UNORM;
        return 0;

    case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8S8:
    case SCE_GXM_TEXTURE_BASE_FORMAT_F16F16F16F16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_U16U16U16U16:
    case SCE_GXM_TEXTURE_BASE_FORMAT_S16S16S16S16:
        info->count = 4;
        info->total_bits = (uint8_t)gxm_texture_base_bits((SceGxmTextureFormat)base);
        info->width[0] = info->width[1] = info->width[2] = info->width[3] =
            (uint8_t)(info->total_bits / 4);
        info->kind[0] = info->kind[1] = info->kind[2] = info->kind[3] =
            base == SCE_GXM_TEXTURE_BASE_FORMAT_S8S8S8S8 ||
                    base == SCE_GXM_TEXTURE_BASE_FORMAT_S16S16S16S16
                ? KIND_SNORM
            : base == SCE_GXM_TEXTURE_BASE_FORMAT_F16F16F16F16 ? KIND_F16
                                                               : KIND_UNORM;
        return 0;

    case SCE_GXM_TEXTURE_BASE_FORMAT_X8S8S8U8:
        /* Only three components are used; the high X byte is ignored. */
        info->count = 3;
        info->total_bits = 24;
        info->width[0] = info->width[1] = info->width[2] = 8;
        info->kind[0] = info->kind[1] = KIND_SNORM;
        info->kind[2] = KIND_UNORM;
        return 0;

    case SCE_GXM_TEXTURE_BASE_FORMAT_X8U24:
        info->count = 2;
        info->total_bits = 32;
        info->width[0] = 24;
        info->width[1] = 8;
        info->kind[0] = KIND_UNORM;
        info->kind[1] = KIND_UNORM;
        return 0;

    case SCE_GXM_TEXTURE_BASE_FORMAT_U2F10F10F10:
        info->count = 4;
        info->total_bits = 32;
        info->width[0] = 2;
        info->width[1] = info->width[2] = info->width[3] = 10;
        info->kind[0] = KIND_UNORM;
        info->kind[1] = info->kind[2] = info->kind[3] = KIND_FLOAT10;
        return 0;

    case SCE_GXM_TEXTURE_BASE_FORMAT_F11F11F10:
        info->count = 3;
        info->total_bits = 32;
        info->width[0] = 10;
        info->width[1] = info->width[2] = 11;
        info->kind[0] = KIND_FLOAT10;
        info->kind[1] = info->kind[2] = KIND_FLOAT11;
        return 0;

    case SCE_GXM_TEXTURE_BASE_FORMAT_SE5M9M9M9:
        info->count = 3;
        info->total_bits = 32;
        info->width[0] = 9;
        info->width[1] = info->width[2] = 9;
        info->kind[0] = KIND_F16; /* shared exponent, handled separately */
        info->kind[1] = info->kind[2] = KIND_F16;
        return 0;

    case SCE_GXM_TEXTURE_BASE_FORMAT_P4:
    case SCE_GXM_TEXTURE_BASE_FORMAT_P8:
        info->count = 4;
        info->total_bits = 32;
        info->width[0] = info->width[1] = info->width[2] = info->width[3] = 8;
        info->kind[0] = info->kind[1] = info->kind[2] = info->kind[3] = KIND_UNORM;
        (void)swizzle;
        return 0;

    default:
        return -1;
    }
}

/* ------------------------------------------------------ component decode */

static float half_to_float(uint16_t h)
{
    uint32_t sign = (h >> 15) & 1;
    uint32_t exponent = (h >> 10) & 0x1f;
    uint32_t mantissa = h & 0x3ff;
    uint32_t bits;
    float value;

    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign << 31;
        } else {
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400) == 0) {
                mantissa <<= 1;
                exponent--;
            }
            mantissa &= 0x3ff;
            bits = (sign << 31) | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 0x1f) {
        bits = (sign << 31) | 0x7f800000u | (mantissa << 13);
    } else {
        bits = (sign << 31) | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static float ufloat_to_float(uint32_t value, unsigned int exponent_bits, unsigned int mantissa_bits)
{
    uint32_t exponent = value >> mantissa_bits;
    uint32_t mantissa = value & ((1u << mantissa_bits) - 1);
    int bias = (1 << (exponent_bits - 1)) - 1;

    if (exponent == 0)
        return ldexpf((float)mantissa / (float)(1u << mantissa_bits), 1 - bias);
    if (exponent == (1u << exponent_bits) - 1)
        return 1.0f; /* inf/nan: clamp */
    return ldexpf(1.0f + (float)mantissa / (float)(1u << mantissa_bits),
                  (int)exponent - bias);
}

static uint8_t component_to_u8(uint32_t raw, uint32_t bits, int kind)
{
    switch (kind) {
    case KIND_SNORM: {
        int32_t value;
        int32_t maximum;
        int32_t scaled;

        if (bits >= 32)
            value = (int32_t)raw;
        else if (raw & (1u << (bits - 1)))
            value = (int32_t)(raw | ~((1u << bits) - 1));
        else
            value = (int32_t)raw;

        maximum = (int32_t)((1u << (bits - 1)) - 1);
        scaled = (int32_t)(((int64_t)value * 127 + maximum / 2) / maximum) + 128;
        return clamp_u8(scaled);
    }

    case KIND_F16: {
        float value = half_to_float((uint16_t)raw);
        return clamp_u8((int)(value * 255.0f + 0.5f));
    }

    case KIND_F32:
    case KIND_F32M: {
        uint32_t bits_value = raw;
        float value;

        if (kind == KIND_F32M)
            bits_value &= 0x7fffffffu;
        memcpy(&value, &bits_value, sizeof(value));
        return clamp_u8((int)(value * 255.0f + 0.5f));
    }

    case KIND_FLOAT10:
        return clamp_u8((int)(ufloat_to_float(raw, 5, 5) * 255.0f + 0.5f));

    case KIND_FLOAT11:
        return clamp_u8((int)(ufloat_to_float(raw, 5, 6) * 255.0f + 0.5f));

    case KIND_UNORM:
    default: {
        uint64_t maximum;
        uint64_t scaled;

        if (bits == 0)
            return 0;
        maximum = bits >= 32 ? 0xffffffffull : ((1ull << bits) - 1);
        scaled = ((uint64_t)raw * 255u + maximum / 2) / maximum;
        return clamp_u8((int)scaled);
    }
    }
}

static uint8_t letter_channel(const uint8_t values[4], const char *letters, int count, char channel)
{
    for (int i = 0; i < count; i++) {
        if (letters[i] == channel)
            return values[i];
    }

    if (channel == 'A')
        return 255;

    return channel == '1' ? 255 : 0;
}

static void unpack_lettered(uint8_t out[4], uint64_t word, const FormatInfo *info,
                            const char *letters)
{
    uint8_t values[4] = {0, 0, 0, 0};
    uint32_t position = info->total_bits;

    for (int i = 0; i < info->count; i++) {
        uint32_t width = info->width[i];
        uint64_t mask = width >= 64 ? ~0ull : ((1ull << width) - 1);
        uint32_t raw = (uint32_t)((word >> (position - width)) & mask);

        values[i] = component_to_u8(raw, width, info->kind[i]);
        position -= width;
    }

    out[0] = letter_channel(values, letters, info->count, 'R');
    out[1] = letter_channel(values, letters, info->count, 'G');
    out[2] = letter_channel(values, letters, info->count, 'B');
    out[3] = letter_channel(values, letters, info->count, 'A');
}

static uint64_t read_word(const uint8_t *data, uint32_t index, uint32_t bits)
{
    const uint8_t *p;
    uint64_t value = 0;
    uint32_t bytes;

    if (bits == 4) {
        uint8_t byte = data[index >> 1];
        return (index & 1) ? (byte >> 4) : (byte & 0xf);
    }

    bytes = bits / 8;
    p = data + (size_t)index * bytes;
    for (uint32_t i = 0; i < bytes && i < 8; i++)
        value |= (uint64_t)p[i] << (8 * i);

    return value;
}

static int decode_texel(uint8_t out[4], const uint8_t *data, uint32_t index,
                        SceGxmTextureFormat format)
{
    uint32_t base = base_format(format);
    uint32_t swizzle = (format >> 12) & 7;
    FormatInfo info;
    uint32_t bits = gxm_texture_base_bits(format);
    uint64_t word;

    if (base == SCE_GXM_TEXTURE_BASE_FORMAT_P4 || base == SCE_GXM_TEXTURE_BASE_FORMAT_P8)
        return -1; /* handled by the palette-aware caller */

    if (format_info(base, swizzle, &info) != 0)
        return -1;

    word = read_word(data, index, bits);

    if (base == SCE_GXM_TEXTURE_BASE_FORMAT_X8U24) {
        uint32_t value = (uint32_t)word;
        uint32_t depth = swizzle == 1 ? (value >> 8) : (value & 0xffffffu);
        out[0] = component_to_u8(depth, 24, KIND_UNORM);
        out[1] = 0;
        out[2] = 0;
        out[3] = 255;
        return 0;
    }

    if (base == SCE_GXM_TEXTURE_BASE_FORMAT_SE5M9M9M9) {
        /* Vita3K's shared-exponent expansion into half floats. */
        uint32_t packed = (uint32_t)word;
        uint16_t exponent = (uint16_t)(packed >> 17);
        uint16_t h0 = (uint16_t)(exponent | ((packed & (0x1ffu << 18)) >> 17));
        uint16_t h1 = (uint16_t)(exponent | ((packed & (0x1ffu << 9)) >> 8));
        uint16_t h2 = (uint16_t)(exponent | ((packed & 0x1ffu) << 1));
        uint8_t values[3];

        values[0] = component_to_u8(h0, 16, KIND_F16);
        values[1] = component_to_u8(h1, 16, KIND_F16);
        values[2] = component_to_u8(h2, 16, KIND_F16);

        if (swizzle == 0) { /* BGR */
            out[0] = values[2];
            out[1] = values[1];
            out[2] = values[0];
        } else {
            out[0] = values[0];
            out[1] = values[1];
            out[2] = values[2];
        }
        out[3] = 255;
        return 0;
    }

    if (info.count == 4) {
        const char *letters = base == SCE_GXM_TEXTURE_BASE_FORMAT_U8U3U3U2
                                  ? "ARGB"
                                  : swizzle4_letters[swizzle & 7];
        unpack_lettered(out, word, &info, letters);
        return 0;
    }

    if (info.count == 3) {
        unpack_lettered(out, word, &info, swizzle3_letters[swizzle & 1]);
        return 0;
    }

    if (info.count == 2) {
        uint64_t mask = info.width[0] >= 64 ? ~0ull : ((1ull << info.width[0]) - 1);
        uint8_t first = component_to_u8((uint32_t)(word & mask), info.width[0], info.kind[0]);
        uint8_t second = component_to_u8((uint32_t)(word >> info.width[0]), info.width[1],
                                         info.kind[1]);

        switch (swizzle) {
        case 0: /* GR */
            out[0] = first;
            out[1] = second;
            out[2] = 0;
            out[3] = 255;
            break;
        case 1: /* 00GR */
            out[0] = first;
            out[1] = second;
            out[2] = 0;
            out[3] = 0;
            break;
        case 2: /* GRRR */
            out[0] = first;
            out[1] = first;
            out[2] = first;
            out[3] = second;
            break;
        case 3: /* RGGG */
            out[0] = second;
            out[1] = second;
            out[2] = second;
            out[3] = first;
            break;
        case 4: /* GRGR */
            out[0] = first;
            out[1] = second;
            out[2] = first;
            out[3] = second;
            break;
        case 5: /* 00RG */
            out[0] = second;
            out[1] = first;
            out[2] = 0;
            out[3] = 0;
            break;
        default:
            out[0] = first;
            out[1] = second;
            out[2] = 0;
            out[3] = 255;
            break;
        }
        return 0;
    }

    /* One component. */
    {
        uint64_t mask = info.width[0] >= 64 ? ~0ull : ((1ull << info.width[0]) - 1);
        uint8_t value = component_to_u8((uint32_t)(word & mask), info.width[0], info.kind[0]);

        switch (swizzle) {
        case 0: /* R */
            out[0] = value;
            out[1] = out[2] = 0;
            out[3] = 255;
            break;
        case 1: /* 000R */
            out[0] = value;
            out[1] = out[2] = out[3] = 0;
            break;
        case 2: /* 111R */
            out[0] = value;
            out[1] = out[2] = out[3] = 255;
            break;
        case 3: /* RRRR */
            out[0] = out[1] = out[2] = out[3] = value;
            break;
        case 4: /* 0RRR */
            out[0] = out[1] = out[2] = value;
            out[3] = 0;
            break;
        case 5: /* 1RRR */
            out[0] = out[1] = out[2] = value;
            out[3] = 255;
            break;
        case 6: /* R000 */
            out[0] = out[1] = out[2] = 0;
            out[3] = value;
            break;
        default: /* R111 */
            out[0] = out[1] = out[2] = 255;
            out[3] = value;
            break;
        }
    }

    return 0;
}

static int decode_paletted_texel(uint8_t out[4], const uint8_t *data, uint32_t index,
                                 SceGxmTextureFormat format, const uint32_t *palette)
{
    uint32_t base = base_format(format);
    uint32_t swizzle = (format >> 12) & 7;
    uint32_t palette_index;
    FormatInfo info;
    uint32_t word;

    if (!palette)
        return -1;

    if (base == SCE_GXM_TEXTURE_BASE_FORMAT_P4) {
        uint8_t byte = data[index >> 1];
        palette_index = (index & 1) ? (byte >> 4) : (byte & 0xf);
    } else {
        palette_index = data[index];
    }

    word = palette[palette_index];
    format_info(SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8, swizzle, &info);
    unpack_lettered(out, word, &info, swizzle4_letters[swizzle & 7]);
    return 0;
}

/* ------------------------------------------------------------ level layout */

static bool is_cube(SceGxmTextureType type)
{
    return type == SCE_GXM_TEXTURE_CUBE || type == SCE_GXM_TEXTURE_CUBE_ARBITRARY;
}

static bool is_arbitrary(SceGxmTextureType type)
{
    return type == SCE_GXM_TEXTURE_SWIZZLED_ARBITRARY ||
           type == SCE_GXM_TEXTURE_CUBE_ARBITRARY;
}

static void level_dimensions(const SceGxmTextureInner *texture, uint32_t level, uint32_t *width,
                             uint32_t *height, uint32_t *layout_width, uint32_t *layout_height)
{
    SceGxmTextureType type = (SceGxmTextureType)gxm_texture_get_type(texture);
    uint32_t base_width = (uint32_t)gxm_texture_get_width(texture);
    uint32_t base_height = (uint32_t)gxm_texture_get_height(texture);
    uint32_t w = base_width >> level;
    uint32_t h = base_height >> level;

    if (!w)
        w = 1;
    if (!h)
        h = 1;

    *width = w;
    *height = h;

    switch (type) {
    case SCE_GXM_TEXTURE_LINEAR:
        *layout_width = align_up(w, 8);
        *layout_height = h;
        break;
    case SCE_GXM_TEXTURE_LINEAR_STRIDED: {
        uint32_t stride = (uint32_t)gxm_texture_get_stride_in_bytes(texture);
        uint32_t bits = gxm_texture_base_bits(gxm_texture_get_format(texture));
        if (bits)
            *layout_width = (uint32_t)((uint64_t)stride * 8 / bits);
        else
            *layout_width = w;
        *layout_height = h;
        break;
    }
    case SCE_GXM_TEXTURE_TILED:
        *layout_width = align_up(w, 32);
        *layout_height = align_up(h, 32);
        break;
    case SCE_GXM_TEXTURE_SWIZZLED_ARBITRARY:
    case SCE_GXM_TEXTURE_CUBE_ARBITRARY:
        *layout_width = next_pow2(w);
        *layout_height = next_pow2(h);
        break;
    default:
        *layout_width = w;
        *layout_height = h;
        break;
    }
}

static uint32_t level_size(const GxmTextureLevel *level, uint32_t bits)
{
    uint64_t pixels = (uint64_t)level->layout_width * level->layout_height;
    return (uint32_t)((pixels * bits + 7) / 8);
}

static uint32_t max_mip_count(uint32_t width, uint32_t height)
{
    uint32_t largest = width > height ? width : height;
    return highest_bit(largest) + 1;
}

uint32_t gxm_texture_true_mip_count(const SceGxmTextureInner *texture)
{
    uint32_t count;

    if (gxm_texture_get_type(texture) == SCE_GXM_TEXTURE_LINEAR_STRIDED)
        return 1;

    count = (texture->mip_count + 1) & 0xf;
    return count ? count : 1;
}

int gxm_texture_get_level(const SceGxmTextureInner *texture, uint32_t face, uint32_t level,
                          GxmTextureLevel *out)
{
    SceGxmTextureType type = (SceGxmTextureType)gxm_texture_get_type(texture);
    uint32_t width = (uint32_t)gxm_texture_get_width(texture);
    uint32_t height = (uint32_t)gxm_texture_get_height(texture);
    uint32_t bits = gxm_texture_base_bits(gxm_texture_get_format(texture));
    uint32_t mips = gxm_texture_true_mip_count(texture);
    uint32_t face_count = is_cube(type) ? 6 : 1;
    uint32_t faces_to_skip;
    uint32_t offset = 0;
    uint32_t face_align = 4;

    if (!texture || !out || !bits || face >= face_count || level >= mips)
        return -1;

    /*
     * Cube maps with a real mip chain reserve room for every possible level on
     * each face, and align each face to 2 KiB (Vita3K's texture cache does the
     * same). Textures without a mip chain pack faces back to back.
     */
    if (is_cube(type) && texture->mip_count != 0xf) {
        uint32_t bpp = bits;
        bool align = (width >= 32 && height >= 32 && bpp <= 8) ||
                     (width >= 16 && height >= 16 && (bpp == 16 || bpp == 32)) ||
                     (width >= 8 && height >= 8 && bpp == 64);
        if (align)
            face_align = 2048;
    }

    for (faces_to_skip = 0; faces_to_skip < face; faces_to_skip++) {
        for (uint32_t l = 0; l < mips; l++) {
            GxmTextureLevel info;

            level_dimensions(texture, l, &info.width, &info.height, &info.layout_width,
                             &info.layout_height);
            offset += level_size(&info, bits);
        }

        if (is_cube(type) && texture->mip_count != 0xf) {
            uint32_t max_mips = max_mip_count(width, height);
            for (uint32_t l = mips; l < max_mips; l++) {
                GxmTextureLevel info;

                level_dimensions(texture, l, &info.width, &info.height, &info.layout_width,
                                 &info.layout_height);
                offset += level_size(&info, bits);
            }
        }

        if (is_cube(type))
            offset = align_up(offset, face_align);
    }

    for (uint32_t l = 0; l < level; l++) {
        GxmTextureLevel info;

        level_dimensions(texture, l, &info.width, &info.height, &info.layout_width,
                         &info.layout_height);
        offset += level_size(&info, bits);
    }

    level_dimensions(texture, level, &out->width, &out->height, &out->layout_width,
                     &out->layout_height);
    out->offset = offset;
    out->size = level_size(out, bits);
    return 0;
}

/* --------------------------------------------------------------- decoding */

/* ----------------------------------------------------- compressed blocks */

static bool is_bc_format(uint32_t base)
{
    return base == SCE_GXM_TEXTURE_BASE_FORMAT_UBC1 ||
           base == SCE_GXM_TEXTURE_BASE_FORMAT_UBC2 ||
           base == SCE_GXM_TEXTURE_BASE_FORMAT_UBC3 ||
           base == SCE_GXM_TEXTURE_BASE_FORMAT_UBC4 ||
           base == SCE_GXM_TEXTURE_BASE_FORMAT_SBC4 ||
           base == SCE_GXM_TEXTURE_BASE_FORMAT_UBC5 ||
           base == SCE_GXM_TEXTURE_BASE_FORMAT_SBC5;
}

static bool is_pvrt_format(uint32_t base)
{
    return base == SCE_GXM_TEXTURE_BASE_FORMAT_PVRT2BPP ||
           base == SCE_GXM_TEXTURE_BASE_FORMAT_PVRT4BPP ||
           base == SCE_GXM_TEXTURE_BASE_FORMAT_PVRTII2BPP ||
           base == SCE_GXM_TEXTURE_BASE_FORMAT_PVRTII4BPP;
}

static uint32_t compressed_block_bytes(uint32_t base)
{
    switch (base) {
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC1:
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC4:
    case SCE_GXM_TEXTURE_BASE_FORMAT_SBC4:
        return 8;
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC2:
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC3:
    case SCE_GXM_TEXTURE_BASE_FORMAT_UBC5:
    case SCE_GXM_TEXTURE_BASE_FORMAT_SBC5:
        return 16;
    default:
        return 0;
    }
}

static void expand_rgb565(uint16_t value, uint8_t rgb[3])
{
    uint32_t r = (value >> 11) & 0x1f;
    uint32_t g = (value >> 5) & 0x3f;
    uint32_t b = value & 0x1f;

    rgb[0] = (uint8_t)((r << 3) | (r >> 2));
    rgb[1] = (uint8_t)((g << 2) | (g >> 4));
    rgb[2] = (uint8_t)((b << 3) | (b >> 2));
}

static void decode_bc1_colors(const uint8_t *block, uint8_t colors[4][4], bool allow_three)
{
    uint16_t c0 = (uint16_t)(block[0] | (block[1] << 8));
    uint16_t c1 = (uint16_t)(block[2] | (block[3] << 8));
    uint8_t rgb0[3], rgb1[3];

    expand_rgb565(c0, rgb0);
    expand_rgb565(c1, rgb1);

    memcpy(colors[0], rgb0, 3);
    colors[0][3] = 255;
    memcpy(colors[1], rgb1, 3);
    colors[1][3] = 255;

    if (c0 > c1 || !allow_three) {
        for (int i = 0; i < 3; i++) {
            colors[2][i] = (uint8_t)((2 * rgb0[i] + rgb1[i]) / 3);
            colors[3][i] = (uint8_t)((rgb0[i] + 2 * rgb1[i]) / 3);
        }
        colors[2][3] = colors[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++) {
            colors[2][i] = (uint8_t)((rgb0[i] + rgb1[i]) / 2);
            colors[3][i] = 0;
        }
        colors[2][3] = 255;
        colors[3][3] = 0;
    }
}

static void decode_bc4_channel(const uint8_t *block, uint8_t values[16])
{
    uint8_t a0 = block[0];
    uint8_t a1 = block[1];
    uint64_t bits = 0;

    for (int i = 0; i < 6; i++)
        bits |= (uint64_t)block[2 + i] << (8 * i);

    if (a0 > a1) {
        values[0] = a0;
        values[1] = a1;
        for (int i = 0; i < 6; i++)
            values[2 + i] = (uint8_t)(((6 - i) * a0 + (1 + i) * a1) / 7);
    } else {
        values[0] = a0;
        values[1] = a1;
        for (int i = 0; i < 4; i++)
            values[2 + i] = (uint8_t)(((4 - i) * a0 + (1 + i) * a1) / 5);
        values[6] = 0;
        values[7] = 255;
    }

    for (int i = 0; i < 16; i++)
        values[i] = values[(bits >> (3 * i)) & 7];
}

static void decode_bc_block(const uint8_t *block, uint32_t base, uint8_t pixels[16][4])
{
    if (base == SCE_GXM_TEXTURE_BASE_FORMAT_UBC4 || base == SCE_GXM_TEXTURE_BASE_FORMAT_SBC4) {
        uint8_t values[16];

        decode_bc4_channel(block, values);
        for (int i = 0; i < 16; i++) {
            pixels[i][0] = values[i];
            pixels[i][1] = 0;
            pixels[i][2] = 0;
            pixels[i][3] = 255;
        }
        return;
    }

    if (base == SCE_GXM_TEXTURE_BASE_FORMAT_UBC5 || base == SCE_GXM_TEXTURE_BASE_FORMAT_SBC5) {
        uint8_t red[16], green[16];

        decode_bc4_channel(block, red);
        decode_bc4_channel(block + 8, green);
        for (int i = 0; i < 16; i++) {
            pixels[i][0] = red[i];
            pixels[i][1] = green[i];
            pixels[i][2] = 0;
            pixels[i][3] = 255;
        }
        return;
    }

    {
        const uint8_t *color_block = (base == SCE_GXM_TEXTURE_BASE_FORMAT_UBC2 ||
                                      base == SCE_GXM_TEXTURE_BASE_FORMAT_UBC3)
                                         ? block + 8
                                         : block;
        uint8_t colors[4][4];
        uint32_t color_bits = (uint32_t)color_block[4] | ((uint32_t)color_block[5] << 8) |
                              ((uint32_t)color_block[6] << 16) |
                              ((uint32_t)color_block[7] << 24);
        bool allow_three = base != SCE_GXM_TEXTURE_BASE_FORMAT_UBC2 &&
                           base != SCE_GXM_TEXTURE_BASE_FORMAT_UBC3;
        int i;

        decode_bc1_colors(color_block, colors, allow_three);

        for (i = 0; i < 16; i++)
            memcpy(pixels[i], colors[(color_bits >> (2 * i)) & 3], 4);

        if (base == SCE_GXM_TEXTURE_BASE_FORMAT_UBC2) {
            for (i = 0; i < 16; i++) {
                uint8_t alpha = (uint8_t)((block[i >> 1] >> ((i & 1) * 4)) & 0xf);
                pixels[i][3] = (uint8_t)(alpha * 17);
            }
        } else if (base == SCE_GXM_TEXTURE_BASE_FORMAT_UBC3) {
            uint8_t alphas[16];

            decode_bc4_channel(block, alphas);
            for (i = 0; i < 16; i++)
                pixels[i][3] = alphas[i];
        }
    }
}

static void copy_decoded_block(uint8_t *dest, uint32_t width, uint32_t height, uint32_t x0,
                               uint32_t y0, const uint8_t pixels[16][4])
{
    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            uint32_t px = x0 + x;
            uint32_t py = y0 + y;

            if (px >= width || py >= height)
                continue;
            memcpy(dest + ((size_t)py * width + px) * 4, pixels[y * 4 + x], 4);
        }
    }
}

static int decode_bc_level(uint8_t *dest, const uint8_t *input, SceGxmTextureType type,
                           SceGxmTextureFormat format, const GxmTextureLevel *info)
{
    uint32_t base = base_format(format);
    uint32_t block_bytes = compressed_block_bytes(base);
    uint32_t blocks_x = (info->width + 3) / 4;
    uint32_t blocks_y = (info->height + 3) / 4;

    if (!block_bytes)
        return -1;

    if (type == SCE_GXM_TEXTURE_SWIZZLED || type == SCE_GXM_TEXTURE_CUBE || is_arbitrary(type)) {
        uint32_t layout_x = info->layout_width / 4;
        uint32_t layout_y = info->layout_height / 4;
        uint32_t minimum = layout_x < layout_y ? layout_x : layout_y;
        uint32_t k = highest_bit(minimum);
        uint32_t count = layout_x * layout_y;

        if (!minimum)
            return -1;

        for (uint32_t i = 0; i < count; i++) {
            uint32_t bx = decode_morton2_x(i) & (minimum - 1);
            uint32_t by = decode_morton2_y(i) & (minimum - 1);
            uint32_t upper = (i >> (2 * k)) << k;
            uint8_t pixels[16][4];

            if (layout_x >= layout_y)
                bx |= upper;
            else
                by |= upper;

            if (bx >= blocks_x || by >= blocks_y)
                continue;

            decode_bc_block(input + (size_t)i * block_bytes, base, pixels);
            copy_decoded_block(dest, info->width, info->height, bx * 4, by * 4, pixels);
        }

        return 0;
    }

    for (uint32_t by = 0; by < blocks_y; by++) {
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            uint32_t index;
            uint8_t pixels[16][4];

            if (type == SCE_GXM_TEXTURE_TILED) {
                uint32_t tile = (bx >> 3) + (info->layout_width / 32) * (by >> 3);
                index = (tile << 6) | (bx & 7) | ((by & 7) << 3);
            } else {
                index = by * (info->layout_width / 4) + bx;
            }

            decode_bc_block(input + (size_t)index * block_bytes, base, pixels);
            copy_decoded_block(dest, info->width, info->height, bx * 4, by * 4, pixels);
        }
    }

    return 0;
}

static int decode_pvrt_level(uint8_t *dest, const uint8_t *input, SceGxmTextureFormat format,
                             const GxmTextureLevel *info)
{
    uint32_t base = base_format(format);
    uint32_t two_bpp = base == SCE_GXM_TEXTURE_BASE_FORMAT_PVRT2BPP ||
                       base == SCE_GXM_TEXTURE_BASE_FORMAT_PVRTII2BPP;
    uint32_t pvrt2 = base == SCE_GXM_TEXTURE_BASE_FORMAT_PVRTII2BPP ||
                     base == SCE_GXM_TEXTURE_BASE_FORMAT_PVRTII4BPP;
    uint8_t *decoded;

    if (!info->layout_width || !info->layout_height)
        return -1;

    decoded = malloc((size_t)info->layout_width * info->layout_height * 4);
    if (!decoded)
        return -1;

    if (gxm_texture_decode_pvrtc(input, info->layout_width, info->layout_height, two_bpp, pvrt2,
                                 decoded) != 0) {
        free(decoded);
        return -1;
    }

    for (uint32_t y = 0; y < info->height; y++)
        memcpy(dest + (size_t)y * info->width * 4,
               decoded + (size_t)y * info->layout_width * 4, (size_t)info->width * 4);

    free(decoded);
    return 0;
}

static bool is_yuv_format(uint32_t base)
{
    return base == SCE_GXM_TEXTURE_BASE_FORMAT_YUV420P2 ||
           base == SCE_GXM_TEXTURE_BASE_FORMAT_YUV420P3 ||
           base == SCE_GXM_TEXTURE_BASE_FORMAT_YUV422;
}

static uint8_t yuv_clamp(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (uint8_t)value;
}

static void yuv_to_rgb(uint8_t out[4], int y, int u, int v, bool bt709)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    int r, g, b;

    if (bt709) {
        r = (298 * c + 459 * e + 128) >> 8;
        g = (298 * c - 55 * d - 136 * e + 128) >> 8;
        b = (298 * c + 541 * d + 128) >> 8;
    } else {
        r = (298 * c + 409 * e + 128) >> 8;
        g = (298 * c - 100 * d - 208 * e + 128) >> 8;
        b = (298 * c + 516 * d + 128) >> 8;
    }

    out[0] = yuv_clamp(r);
    out[1] = yuv_clamp(g);
    out[2] = yuv_clamp(b);
    out[3] = 255;
}

static int decode_yuv_level(uint8_t *dest, const uint8_t *input, SceGxmTextureFormat format,
                            const GxmTextureLevel *info)
{
    uint32_t base = base_format(format);
    uint32_t swizzle = (format >> 12) & 7;

    if (base == SCE_GXM_TEXTURE_BASE_FORMAT_YUV422) {
        bool bt709 = (swizzle & 4) != 0;
        uint32_t mode = swizzle & 3;

        for (uint32_t y = 0; y < info->height; y++) {
            const uint8_t *row = input + (size_t)y * info->layout_width * 2;

            for (uint32_t x = 0; x < info->width; x += 2) {
                const uint8_t *pair = row + (size_t)x * 2;
                int y0 = pair[1];
                int y1 = pair[3];
                int u = 0, v = 0;

                switch (mode) {
                case 1: /* YVYU */
                    v = pair[0];
                    u = pair[2];
                    break;
                case 2: /* UYVY */
                    u = pair[0];
                    v = pair[2];
                    y0 = pair[1];
                    y1 = pair[3];
                    break;
                case 3: /* VYUY */
                    v = pair[0];
                    u = pair[2];
                    y0 = pair[1];
                    y1 = pair[3];
                    break;
                case 0: /* YUYV */
                default:
                    u = pair[1];
                    v = pair[3];
                    y0 = pair[0];
                    y1 = pair[2];
                    break;
                }

                yuv_to_rgb(dest + ((size_t)y * info->width + x) * 4, y0, u, v, bt709);
                if (x + 1 < info->width)
                    yuv_to_rgb(dest + ((size_t)y * info->width + x + 1) * 4, y1, u, v, bt709);
            }
        }

        return 0;
    }

    /* 4:2:0, either NV12 (P2) or three planes (P3). */
    {
        uint32_t layout_w = info->layout_width;
        uint32_t layout_h = info->layout_height;
        const uint8_t *luma = input;
        const uint8_t *chroma = input + (size_t)layout_w * layout_h;
        size_t chroma_plane = (size_t)(layout_w / 2) * (layout_h / 2);

        for (uint32_t y = 0; y < info->height; y++) {
            for (uint32_t x = 0; x < info->width; x++) {
                uint32_t cx = x / 2;
                uint32_t cy = y / 2;
                int u, v;

                if (base == SCE_GXM_TEXTURE_BASE_FORMAT_YUV420P3) {
                    u = chroma[(size_t)cy * (layout_w / 2) + cx];
                    v = chroma[chroma_plane + (size_t)cy * (layout_w / 2) + cx];
                } else {
                    u = chroma[(size_t)cy * layout_w + cx * 2];
                    v = chroma[(size_t)cy * layout_w + cx * 2 + 1];
                }

                yuv_to_rgb(dest + ((size_t)y * info->width + x) * 4,
                           luma[(size_t)y * layout_w + x], u, v, false);
            }
        }
    }

    return 0;
}

static int decode_level_rgba8(uint8_t *dest, const uint8_t *input,
                              const SceGxmTextureInner *texture, SceGxmTextureType type,
                              SceGxmTextureFormat format, const GxmTextureLevel *info)
{
    uint32_t base = base_format(format);
    const uint32_t *palette = NULL;

    if (base == SCE_GXM_TEXTURE_BASE_FORMAT_P4 || base == SCE_GXM_TEXTURE_BASE_FORMAT_P8)
        palette = (const uint32_t *)(uintptr_t)(texture->palette_addr << 6);

    if (is_bc_format(base))
        return decode_bc_level(dest, input, type, format, info);
    if (is_pvrt_format(base))
        return decode_pvrt_level(dest, input, format, info);
    if (is_yuv_format(base))
        return decode_yuv_level(dest, input, format, info);

    if (type == SCE_GXM_TEXTURE_SWIZZLED || type == SCE_GXM_TEXTURE_CUBE ||
        is_arbitrary(type)) {
        uint32_t minimum = info->layout_width < info->layout_height ? info->layout_width
                                                                    : info->layout_height;
        uint32_t k = highest_bit(minimum);
        uint32_t count = info->layout_width * info->layout_height;

        if (!minimum)
            return -1;

        for (uint32_t i = 0; i < count; i++) {
            uint32_t x = decode_morton2_x(i) & (minimum - 1);
            uint32_t y = decode_morton2_y(i) & (minimum - 1);
            uint32_t upper = (i >> (2 * k)) << k;

            if (info->layout_width >= info->layout_height)
                x |= upper;
            else
                y |= upper;

            if (x >= info->width || y >= info->height)
                continue;

            if (palette) {
                if (decode_paletted_texel(&dest[(y * info->width + x) * 4], input, i, format,
                                          palette) != 0)
                    return -1;
            } else if (decode_texel(&dest[(y * info->width + x) * 4], input, i, format) != 0) {
                return -1;
            }
        }

        return 0;
    }

    if (type == SCE_GXM_TEXTURE_TILED) {
        uint32_t tiles_per_row = info->layout_width >> 5;

        for (uint32_t y = 0; y < info->height; y++) {
            for (uint32_t x = 0; x < info->width; x++) {
                uint32_t tile = (x >> 5) + tiles_per_row * (y >> 5);
                uint32_t index = (tile << 10) | (x & 31) | ((y & 31) << 5);

                if (palette) {
                    if (decode_paletted_texel(&dest[(y * info->width + x) * 4], input, index,
                                              format, palette) != 0)
                        return -1;
                } else if (decode_texel(&dest[(y * info->width + x) * 4], input, index, format) !=
                           0) {
                    return -1;
                }
            }
        }

        return 0;
    }

    /* Linear and linear-strided layouts. */
    for (uint32_t y = 0; y < info->height; y++) {
        for (uint32_t x = 0; x < info->width; x++) {
            uint32_t index = y * info->layout_width + x;

            if (palette) {
                if (decode_paletted_texel(&dest[(y * info->width + x) * 4], input, index, format,
                                          palette) != 0)
                    return -1;
            } else if (decode_texel(&dest[(y * info->width + x) * 4], input, index, format) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

int gxm_texture_decode_level_at(uint8_t *dest, const SceGxmTextureInner *texture, const void *data,
                                uint32_t face, uint32_t level)
{
    SceGxmTextureType type;
    SceGxmTextureFormat format;
    GxmTextureLevel info;

    if (!dest || !texture || !data)
        return -1;
    if (gxm_texture_get_level(texture, face, level, &info) != 0)
        return -1;

    type = (SceGxmTextureType)gxm_texture_get_type(texture);
    format = (SceGxmTextureFormat)gxm_texture_get_format(texture);

    return decode_level_rgba8(dest, (const uint8_t *)data + info.offset, texture, type, format,
                              &info);
}

int gxm_texture_decode_level_rgba8(uint8_t *dest, const SceGxmTextureInner *texture, uint32_t face,
                                   uint32_t level)
{
    return gxm_texture_decode_level_at(dest, texture, gxm_texture_get_data(texture), face, level);
}

int gxm_texture_decode_rgba8(uint8_t *dest, const void *src, uint32_t width, uint32_t height,
                             SceGxmTextureFormat format, SceGxmTextureType type)
{
    SceGxmTextureInner texture;
    GxmTextureLevel info;
    uint32_t bits = gxm_texture_base_bits(format);

    if (!dest || !src || !width || !height || !bits)
        return -1;

    memset(&texture, 0, sizeof(texture));
    texture.base_format = (base_format(format) >> 24) & 0x1f;
    texture.format0 = (format & 0x80000000u) >> 31;
    texture.swizzle_format = (format >> 12) & 7;
    texture.normalize_mode = 1;
    texture.mip_count = 0;

    memset(&info, 0, sizeof(info));
    info.width = width;
    info.height = height;
    if (type == SCE_GXM_TEXTURE_SWIZZLED || type == SCE_GXM_TEXTURE_CUBE) {
        info.layout_width = width;
        info.layout_height = height;
    } else {
        info.layout_width = align_up(width, 8);
        info.layout_height = height;
    }
    info.size = (uint32_t)(((uint64_t)info.layout_width * info.layout_height * bits + 7) / 8);

    return decode_level_rgba8(dest, (const uint8_t *)src, &texture, type, format, &info);
}
