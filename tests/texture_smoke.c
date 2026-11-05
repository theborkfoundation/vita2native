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

static uint32_t log2_of(uint32_t value)
{
    uint32_t bit = 0;
    while (value >>= 1)
        bit++;
    return bit;
}

static uint32_t encode_morton(uint32_t x, uint32_t y, uint32_t min, uint32_t k)
{
    return (((x >> k) | (y >> k)) << (2 * k)) | (part1by1(x & (min - 1)) << 1) |
           part1by1(y & (min - 1));
}

static void init_texture(SceGxmTextureInner *texture, const void *data, SceGxmTextureFormat format,
                         uint32_t width, uint32_t height, uint32_t mip_count,
                         SceGxmTextureType type)
{
    memset(texture, 0, sizeof(*texture));
    texture->base_format = (format & 0x1f000000u) >> 24;
    texture->format0 = (format & 0x80000000u) >> 31;
    texture->swizzle_format = (format >> 12) & 7;
    texture->normalize_mode = 1;
    texture->mip_count = mip_count ? mip_count - 1 : 0;
    if (type == SCE_GXM_TEXTURE_SWIZZLED || type == SCE_GXM_TEXTURE_CUBE) {
        texture->width_base2 = log2_of(width);
        texture->height_base2 = log2_of(height);
    } else {
        texture->width = width - 1;
        texture->height = height - 1;
    }
    texture->type = (uint32_t)type >> 29;
    texture->data_addr = (uint32_t)(uintptr_t)data >> 2;
}

static void test_swizzle4(void)
{
    static const struct {
        SceGxmTextureFormat format;
        uint8_t bytes[4];
    } cases[] = {
        { SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, { 10, 20, 30, 40 } },
        { SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB, { 30, 20, 10, 40 } },
        { SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_RGBA, { 40, 30, 20, 10 } },
        { SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_BGRA, { 40, 10, 20, 30 } },
        { SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8 | SCE_GXM_TEXTURE_SWIZZLE4_1BGR, { 10, 20, 30, 255 } },
        { SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8 | SCE_GXM_TEXTURE_SWIZZLE4_1RGB, { 30, 20, 10, 255 } },
        { SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8 | SCE_GXM_TEXTURE_SWIZZLE4_RGB1, { 255, 30, 20, 10 } },
        { SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8 | SCE_GXM_TEXTURE_SWIZZLE4_BGR1, { 255, 10, 20, 30 } },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t rgba[4];
        assert(gxm_texture_decode_rgba8(rgba, cases[i].bytes, 1, 1, cases[i].format,
                                        SCE_GXM_TEXTURE_LINEAR) == 0);
        assert(rgba[0] == 10 && rgba[1] == 20 && rgba[2] == 30);
        assert(rgba[3] == (cases[i].format == SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR ||
                                   cases[i].format == SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ARGB ||
                                   cases[i].format == SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_RGBA ||
                                   cases[i].format == SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_BGRA
                               ? 40
                               : 255));
    }
}

static void test_packed(void)
{
    /* RGB565_BGR: word [B5 G6 R5], red in the low bits. */
    {
        uint16_t word = (uint16_t)((30u << 11) | (20u << 5) | 10u);
        uint8_t rgba[4];

        assert(gxm_texture_decode_rgba8(rgba, &word, 1, 1, SCE_GXM_TEXTURE_FORMAT_U5U6U5_BGR,
                                        SCE_GXM_TEXTURE_LINEAR) == 0);
        assert(rgba[0] == (10 * 255 + 15) / 31);
        assert(rgba[1] == (20 * 255 + 31) / 63);
        assert(rgba[2] == (30 * 255 + 15) / 31);
        assert(rgba[3] == 255);
    }

    /* RGB565_RGB: word [R5 G6 B5], red in the high bits. */
    {
        uint16_t word = (uint16_t)((10u << 11) | (20u << 5) | 30u);
        uint8_t rgba[4];

        assert(gxm_texture_decode_rgba8(rgba, &word, 1, 1, SCE_GXM_TEXTURE_FORMAT_U5U6U5_RGB,
                                        SCE_GXM_TEXTURE_LINEAR) == 0);
        assert(rgba[0] == (10 * 255 + 15) / 31);
        assert(rgba[1] == (20 * 255 + 31) / 63);
        assert(rgba[2] == (30 * 255 + 15) / 31);
    }

    /* ARGB4444: word [A4 B4 G4 R4]. */
    {
        uint16_t word = (uint16_t)((15u << 12) | (12u << 8) | (8u << 4) | 4u);
        uint8_t rgba[4];

        assert(gxm_texture_decode_rgba8(rgba, &word, 1, 1,
                                        SCE_GXM_TEXTURE_FORMAT_U4U4U4U4_ABGR,
                                        SCE_GXM_TEXTURE_LINEAR) == 0);
        assert(rgba[0] == 4 * 17 && rgba[1] == 8 * 17 && rgba[2] == 12 * 17);
        assert(rgba[3] == 15 * 17);
    }

    /* ARGB1555: word [A1 B5 G5 R5]. */
    {
        uint16_t word = (uint16_t)(0x8000u | (12u << 10) | (8u << 5) | 4u);
        uint8_t rgba[4];

        assert(gxm_texture_decode_rgba8(rgba, &word, 1, 1,
                                        SCE_GXM_TEXTURE_FORMAT_U1U5U5U5_ABGR,
                                        SCE_GXM_TEXTURE_LINEAR) == 0);
        assert(rgba[0] == (4 * 255 + 15) / 31);
        assert(rgba[1] == (8 * 255 + 15) / 31);
        assert(rgba[2] == (12 * 255 + 15) / 31);
        assert(rgba[3] == 255);
    }

    /* U8U3U3U2_ARGB: word [A8 R3 G3 B2]. */
    {
        uint16_t word = (uint16_t)((200u << 8) | (5u << 5) | (3u << 2) | 2u);
        uint8_t rgba[4];

        assert(gxm_texture_decode_rgba8(rgba, &word, 1, 1,
                                        SCE_GXM_TEXTURE_FORMAT_U8U3U3U2_ARGB,
                                        SCE_GXM_TEXTURE_LINEAR) == 0);
        assert(rgba[0] == (5 * 255 + 3) / 7);
        assert(rgba[1] == (3 * 255 + 3) / 7);
        assert(rgba[2] == (2 * 255 + 1) / 3);
        assert(rgba[3] == 200);
    }
}

static void test_swizzled_mips(void)
{
    uint8_t data[8 * 8 * 4 + 4 * 4 * 4 + 2 * 2 * 4];
    SceGxmTextureInner texture;
    GxmTextureLevel level;
    uint8_t decoded[4 * 4 * 4];
    uint32_t expected = 20;

    for (uint32_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)i;

    init_texture(&texture, data, SCE_GXM_TEXTURE_FORMAT_U8_R, 8, 8, 3,
                 SCE_GXM_TEXTURE_SWIZZLED);

    assert(gxm_texture_get_level(&texture, 0, 0, &level) == 0);
    assert(level.width == 8 && level.height == 8);
    assert(level.offset == 0 && level.size == 8 * 8);

    assert(gxm_texture_get_level(&texture, 0, 1, &level) == 0);
    assert(level.width == 4 && level.height == 4);
    assert(level.offset == 8 * 8 && level.size == 4 * 4);

    assert(gxm_texture_get_level(&texture, 0, 2, &level) == 0);
    assert(level.width == 2 && level.height == 2);
    assert(level.offset == 8 * 8 + 4 * 4 && level.size == 2 * 2);

    assert(gxm_texture_get_level(&texture, 0, 3, &level) != 0);

    /* Level 1 is a 4x4 swizzled image starting at byte 64. */
    assert(gxm_texture_decode_level_at(decoded, &texture, data, 0, 1) == 0);
    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 4; x++) {
            uint32_t index = encode_morton(x, y, 4, 2);
            uint8_t value = data[64 + index];

            assert(decoded[(y * 4 + x) * 4] == value);
            assert(decoded[(y * 4 + x) * 4 + 3] == 255);
        }
    }
    (void)expected;
}

static void test_linear_strided(void)
{
    uint8_t data[2 * 8];
    SceGxmTextureInner texture;
    uint8_t decoded[2 * 3 * 4];
    uint32_t encoded_stride;

    memset(data, 0xee, sizeof(data));
    for (uint32_t x = 0; x < 3; x++) {
        data[x] = (uint8_t)(1 + x);
        data[8 + x] = (uint8_t)(4 + x);
    }

    init_texture(&texture, data, SCE_GXM_TEXTURE_FORMAT_U8_000R, 3, 2, 1,
                 SCE_GXM_TEXTURE_LINEAR_STRIDED);
    encoded_stride = 8 / 4 - 1;
    texture.mip_filter = encoded_stride & 1;
    texture.min_filter = (encoded_stride >> 1) & 3;
    texture.mip_count = (encoded_stride >> 3) & 0xf;
    texture.lod_bias = (encoded_stride >> 7) & 0x3f;

    assert(gxm_texture_get_stride_in_bytes(&texture) == 8);
    assert(gxm_texture_decode_level_at(decoded, &texture, data, 0, 0) == 0);

    for (uint32_t i = 0; i < 6; i++)
        assert(decoded[i * 4] == i + 1);
}

static void test_palette(void)
{
#if UINTPTR_MAX <= 0xffffffffu
    static const uint8_t indices[4] = { 0, 1, 2, 3 };
    /* A8B8G8R8 words: red, green, blue, white. GXM stores the address >> 6. */
    static const uint32_t palette[4] __attribute__((aligned(64))) = {
        0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu,
    };
    SceGxmTextureInner texture;
    uint8_t decoded[4 * 4];

    init_texture(&texture, indices, SCE_GXM_TEXTURE_FORMAT_P8_ABGR, 4, 1, 1,
                 SCE_GXM_TEXTURE_LINEAR);
    texture.palette_addr = (uint32_t)(uintptr_t)palette >> 6;

    assert(gxm_texture_decode_level_at(decoded, &texture, indices, 0, 0) == 0);
    assert(decoded[0] == 255 && decoded[1] == 0 && decoded[2] == 0 && decoded[3] == 255);
    assert(decoded[4] == 0 && decoded[5] == 255 && decoded[6] == 0 && decoded[7] == 255);
    assert(decoded[8] == 0 && decoded[9] == 0 && decoded[10] == 255 && decoded[11] == 255);
    assert(decoded[12] == 255 && decoded[13] == 255 && decoded[14] == 255 && decoded[15] == 255);

    /* P4: low nibble is the even pixel. */
    {
        static const uint8_t nibbles[1] = { 0x21 };
        static const uint32_t palette2[4] __attribute__((aligned(64))) = {
            0x00000000u, 0xffffffffu, 0xff0000ffu, 0,
        };
        SceGxmTextureInner texture4;

        init_texture(&texture4, nibbles, SCE_GXM_TEXTURE_FORMAT_P4_ABGR, 2, 1, 1,
                     SCE_GXM_TEXTURE_LINEAR);
        texture4.palette_addr = (uint32_t)(uintptr_t)palette2 >> 6;

        assert(gxm_texture_decode_level_at(decoded, &texture4, nibbles, 0, 0) == 0);
        assert(decoded[0] == 255 && decoded[1] == 255 && decoded[2] == 255);
        assert(decoded[4] == 255 && decoded[5] == 0 && decoded[6] == 0);
    }
#endif
}

static void test_tiled(void)
{
    uint8_t data[32 * 32];
    SceGxmTextureInner texture;
    uint8_t decoded[32 * 32 * 4];

    for (uint32_t y = 0; y < 32; y++) {
        for (uint32_t x = 0; x < 32; x++) {
            uint32_t tile = (x >> 5) + (y >> 5);
            uint32_t index = (tile << 10) | (x & 31) | ((y & 31) << 5);

            data[index] = (uint8_t)(x + y);
        }
    }

    init_texture(&texture, data, SCE_GXM_TEXTURE_FORMAT_U8_000R, 32, 32, 1,
                 SCE_GXM_TEXTURE_TILED);
    assert(gxm_texture_decode_level_at(decoded, &texture, data, 0, 0) == 0);

    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 32; x++)
            assert(decoded[(y * 32 + x) * 4] == (uint8_t)(x + y));
}

static void test_compressed(void)
{
    /* BC1: c0 = red, c1 = green, all indices 0 => solid red. */
    {
        const uint8_t bc1[8] = { 0x00, 0xf8, 0xe0, 0x07, 0x00, 0x00, 0x00, 0x00 };
        uint8_t rgba[4 * 4 * 4];

        assert(gxm_texture_decode_rgba8(rgba, bc1, 4, 4, SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR,
                                        SCE_GXM_TEXTURE_LINEAR) == 0);
        for (int i = 0; i < 16; i++) {
            assert(rgba[i * 4 + 0] == 255);
            assert(rgba[i * 4 + 1] == 0);
            assert(rgba[i * 4 + 2] == 0);
            assert(rgba[i * 4 + 3] == 255);
        }
    }

    /* BC3: 8-bit alpha block (a0 = a1 = 255, indices 0) plus a red colour block. */
    {
        uint8_t bc3[16] = { 0 };
        uint8_t rgba[4 * 4 * 4];

        bc3[0] = 255;
        bc3[1] = 255;
        bc3[8] = 0x00;
        bc3[9] = 0xf8;
        bc3[10] = 0xe0;
        bc3[11] = 0x07;
        assert(gxm_texture_decode_rgba8(rgba, bc3, 4, 4, SCE_GXM_TEXTURE_FORMAT_UBC3_ABGR,
                                        SCE_GXM_TEXTURE_LINEAR) == 0);
        for (int i = 0; i < 16; i++) {
            assert(rgba[i * 4 + 0] == 255);
            assert(rgba[i * 4 + 1] == 0);
            assert(rgba[i * 4 + 2] == 0);
            assert(rgba[i * 4 + 3] == 255);
        }
    }

    /* BC4: a0 = 255, a1 = 0, all indices 0 => solid 255 in the red channel. */
    {
        const uint8_t bc4[8] = { 255, 0, 0, 0, 0, 0, 0, 0 };
        uint8_t rgba[4 * 4 * 4];

        assert(gxm_texture_decode_rgba8(rgba, bc4, 4, 4,
                                        SCE_GXM_TEXTURE_BASE_FORMAT_UBC4 |
                                            SCE_GXM_TEXTURE_SWIZZLE1_R,
                                        SCE_GXM_TEXTURE_LINEAR) == 0);
        for (int i = 0; i < 16; i++) {
            assert(rgba[i * 4 + 0] == 255);
            assert(rgba[i * 4 + 1] == 0);
            assert(rgba[i * 4 + 2] == 0);
            assert(rgba[i * 4 + 3] == 255);
        }
    }
}

static void test_yuv(void)
{
    /* NV12, 4x4, all Y = U = V = 128: a neutral grey around 130. */
    uint8_t nv12[8 * 4 + 8 * 2];
    uint8_t rgba[4 * 4 * 4];

    memset(nv12, 128, sizeof(nv12));
    assert(gxm_texture_decode_rgba8(rgba, nv12, 4, 4,
                                    SCE_GXM_TEXTURE_FORMAT_YUV420P2_CSC0,
                                    SCE_GXM_TEXTURE_LINEAR) == 0);
    for (int i = 0; i < 16; i++) {
        assert(rgba[i * 4 + 0] >= 129 && rgba[i * 4 + 0] <= 131);
        assert(rgba[i * 4 + 1] == rgba[i * 4 + 0]);
        assert(rgba[i * 4 + 2] == rgba[i * 4 + 0]);
        assert(rgba[i * 4 + 3] == 255);
    }

    /* YUYV 4:2:2, two pixels with Y = 128 and neutral chroma. */
    {
        uint8_t yuyv[8 * 2] = { 128, 128, 128, 128 };

        assert(gxm_texture_decode_rgba8(rgba, yuyv, 2, 1,
                                        SCE_GXM_TEXTURE_FORMAT_YUYV422_CSC0,
                                        SCE_GXM_TEXTURE_LINEAR) == 0);
        assert(rgba[0] >= 129 && rgba[0] <= 131);
        assert(rgba[4] >= 129 && rgba[4] <= 131);
    }
}

int main(void)
{
    test_swizzle4();
    test_packed();
    test_swizzled_mips();
    test_linear_strided();
    test_palette();
    test_tiled();
    test_compressed();
    test_yuv();

    /* Format conversion: ABGR bytes -> RGBA8. */
    {
        const uint8_t abgr[4] = { 10, 20, 30, 40 };
        uint8_t rgba[4];
        assert(gxm_texture_decode_rgba8(rgba, abgr, 1, 1, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
                                        SCE_GXM_TEXTURE_LINEAR) == 0);
        assert(rgba[0] == 10 && rgba[1] == 20 && rgba[2] == 30 && rgba[3] == 40);
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

        assert(gxm_texture_decode_rgba8(decoded, swizzled, 4, 4, SCE_GXM_TEXTURE_FORMAT_U8_R,
                                        SCE_GXM_TEXTURE_SWIZZLED) == 0);

        for (uint32_t i = 0; i < 16; i++) {
            assert(decoded[i * 4 + 0] == linear[i]);
            assert(decoded[i * 4 + 3] == 255);
        }
    }

    puts("texture_smoke: ok");
    return 0;
}
