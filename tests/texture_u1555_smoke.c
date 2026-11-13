/*
 * U1U5U5U5 decode equivalence.
 *
 * The LUT path in decode_linear_fast16() has to produce exactly what the
 * generic per-texel decoder produces, including the swizzle handling - getting
 * R and B the wrong way round is easy to miss by eye and would tint every
 * surface decoded through this format. VITA2NATIVE_NO_TEXTURE_LUT forces the
 * generic path, so the same input can be decoded both ways and compared.
 *
 * The format is 1/5/5/5 bits per channel, so a full sweep of the 16-bit wire
 * space is practical: every possible texel is covered, not a sample.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/gxm.h>

#include "gxm/texture.h"
#include "test.h"

#define WIDTH  256
#define HEIGHT 256

static void build_texture(SceGxmTextureInner *tex, unsigned swizzle)
{
    memset(tex, 0, sizeof(*tex));
    tex->base_format = (SCE_GXM_TEXTURE_BASE_FORMAT_U1U5U5U5 & 0x1f000000u) >> 24;
    tex->swizzle_format = swizzle;
    tex->format0 = 0;
    /* get_type() shifts this by 29 and compare against the public constants,
       so LINEAR (0x60000000) is 3. A zeroed struct means SWIZZLED, which
       reads layout_width * layout_height texels instead of the row-major one. */
    tex->type = (uint32_t)SCE_GXM_TEXTURE_LINEAR >> 29;
    tex->width = WIDTH - 1;
    tex->height = HEIGHT - 1;
    tex->mip_count = 15; /* no mip chain */
    tex->uaddr_mode = tex->vaddr_mode = SCE_GXM_TEXTURE_ADDR_CLAMP;
}

int main(void)
{
    static uint8_t src[WIDTH * HEIGHT * 2];
    static uint8_t lut[WIDTH * HEIGHT * 4];
    static uint8_t generic[WIDTH * HEIGHT * 4];

    /* Cover the whole 16-bit wire space: 65536 distinct words, then repeat. */
    for (uint32_t i = 0; i < WIDTH * HEIGHT; i++) {
        uint16_t word = (uint16_t)(i & 0xffffu);

        src[i * 2 + 0] = (uint8_t)(word & 0xff);
        src[i * 2 + 1] = (uint8_t)(word >> 8);
    }

    for (unsigned swizzle = 0; swizzle < 8; swizzle++) {
        SceGxmTextureInner tex;

        build_texture(&tex, swizzle);
        fprintf(stderr, "swizzle %u: decoding generic\n", swizzle);
        fflush(stderr);

        setenv("VITA2NATIVE_NO_TEXTURE_LUT", "1", 1);
        REQUIRE(gxm_texture_decode_level_at(generic, &tex, src, 0, 0) == 0,
                "swizzle %u: generic decode failed", swizzle);
        unsetenv("VITA2NATIVE_NO_TEXTURE_LUT");

        fprintf(stderr, "swizzle %u: decoding LUT\n", swizzle);
        fflush(stderr);
        REQUIRE(gxm_texture_decode_level_at(lut, &tex, src, 0, 0) == 0,
                "swizzle %u: LUT decode failed", swizzle);

        CHECK(memcmp(lut, generic, sizeof(lut)) == 0, "swizzle %u: LUT differs from generic",
              swizzle);
        if (memcmp(lut, generic, sizeof(lut)) != 0) {
            /* Report the first differing texel in full, plus the wire word that
               produced it, so the channel mapping can be compared by hand. */
            size_t i = 0;
            uint16_t wire;

            while (i + 3 < sizeof(lut) && memcmp(lut + i, generic + i, 4) == 0)
                i += 4;
            memcpy(&wire, src + (i / 4) * 2, sizeof(wire));
            fprintf(stderr,
                    "  first diff texel %zu wire=%04x: generic=%02x%02x%02x%02x "
                    "lut=%02x%02x%02x%02x\n",
                    i / 4, wire, generic[i + 3], generic[i + 2], generic[i + 1], generic[i],
                    lut[i + 3], lut[i + 2], lut[i + 1], lut[i]);
        }

        /* A wrong channel order still looks plausible, so assert the alpha of a
           fully opaque word and the red channel of a known pure-red word. */
        if (swizzle == 1) { /* ARGB */
            /* bits: A=15, R=14..10, G=9..5, B=4..0 */
            uint16_t opaque_red = (uint16_t)(0x8000u | (31u << 10));
            uint32_t expect = 255u | (0u << 8) | (0u << 16) | (255u << 24);
            uint32_t got;

            memset(src, 0, sizeof(src));
            src[0] = (uint8_t)(opaque_red & 0xff);
            src[1] = (uint8_t)(opaque_red >> 8);
            REQUIRE(gxm_texture_decode_level_at(lut, &tex, src, 0, 0) == 0,
                    "swizzle 1: known-word decode failed");
            memcpy(&got, lut, sizeof(got));
            CHECK(got == expect, "swizzle 1: pure red decoded as %08x, want %08x", got, expect);
        }
    }

    return test_status("texture_u1555_smoke");
}
