/*
 * Optimised PVRTC-I 4bpp decoder.
 *
 * The PowerVR SDK reference implementation (external/Vita3K/.../pvrt-dec.cpp)
 * decodes every 4x4 block through two 16x8 modulation arrays that are zeroed
 * for each block, two full-colour struct interpolations and per-pixel struct
 * arithmetic. On a Cortex-A53 that costs ~255 ns per pixel, which is far worse
 * than any other texture format in the port.
 *
 * PVRTC-I 4bpp only needs the 2x2 centre of each of the four words' modulation
 * grids, and the bilinear endpoint interpolation is a fixed 4x4 weight pattern,
 * so the same output can be produced with integer scalar code and no per-block
 * scratch. This decoder reproduces the reference output bit for bit; PVRTC-II
 * and 2bpp stay on the reference path.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gxm/texture_compressed.h"

static const uint8_t pvrtc_mod_normal[4] = { 0, 3, 5, 8 };
/* Mode 1: raw 2 is punch-through alpha with an effective modulation of 4. */
static const uint8_t pvrtc_mod_alpha[4] = { 0, 4, 4, 8 };

static bool pvrtc_is_pow2(uint32_t value)
{
    return value && !(value & (value - 1));
}

static uint32_t pvrtc_twiddle(uint32_t x_size, uint32_t y_size, uint32_t x, uint32_t y)
{
    uint32_t min_dimension = x_size;
    uint32_t max_value = y;
    uint32_t twiddled = 0;
    uint32_t src_bit = 1;
    uint32_t dst_bit = 1;
    uint32_t shift = 0;

    if (y_size < x_size) {
        min_dimension = y_size;
        max_value = x;
    }

    while (src_bit < min_dimension) {
        if (y & src_bit)
            twiddled |= dst_bit;
        if (x & src_bit)
            twiddled |= dst_bit << 1;
        src_bit <<= 1;
        dst_bit <<= 2;
        shift++;
    }

    max_value >>= shift;
    twiddled |= max_value << (2 * shift);
    return twiddled;
}

static inline uint32_t pvrtc_expand4(uint32_t value)
{
    return (value << 1) | (value >> 3);
}

static inline uint32_t pvrtc_expand3(uint32_t value)
{
    return (value << 2) | (value >> 1);
}

/* Colour A of a PVRTC-I word (the reference's getColorA with uiII == 0). */
static inline void pvrtc_color_a(uint32_t data, int color[4])
{
    if (data & 0x8000u) {
        color[0] = (int)((data >> 10) & 0x1f);
        color[1] = (int)((data >> 5) & 0x1f);
        color[2] = (int)pvrtc_expand4((data >> 1) & 0xf);
        color[3] = 15;
    } else {
        color[0] = (int)pvrtc_expand4((data >> 8) & 0xf);
        color[1] = (int)pvrtc_expand4((data >> 4) & 0xf);
        color[2] = (int)pvrtc_expand3((data >> 1) & 0x7);
        color[3] = (int)(((data >> 12) & 0x7) << 1);
    }
}

/* Colour B of a PVRTC-I word (the reference's getColorB). */
static inline void pvrtc_color_b(uint32_t data, int color[4])
{
    if (data & 0x80000000u) {
        color[0] = (int)((data >> 26) & 0x1f);
        color[1] = (int)((data >> 21) & 0x1f);
        color[2] = (int)((data >> 16) & 0x1f);
        color[3] = 15;
    } else {
        color[0] = (int)pvrtc_expand4((data >> 24) & 0xf);
        color[1] = (int)pvrtc_expand4((data >> 20) & 0xf);
        color[2] = (int)pvrtc_expand4((data >> 16) & 0xf);
        color[3] = (int)(((data >> 28) & 0x7) << 1);
    }
}

/*
 * One modulation value. The high bit marks punch-through alpha, matching the
 * reference's "mod -= 10" path for mode-1 raw value 2.
 */
static inline uint8_t pvrtc_mod_value(uint32_t data, uint32_t shift, uint32_t alpha_mode)
{
    uint32_t raw = (data >> shift) & 3u;

    if (alpha_mode)
        return (uint8_t)(pvrtc_mod_alpha[raw] | (raw == 2 ? 0x80u : 0u));
    return pvrtc_mod_normal[raw];
}

/*
 * Decode the 4x4 pixels around the corner of words P, Q, R and S. The corner
 * sits at (word_x + 1, word_y + 1); the reference walks corners from -1 so the
 * outer ring wraps around the texture.
 */
static void pvrtc4_block(const uint32_t *words, uint32_t num_x, uint32_t num_y,
                         const uint32_t *twiddle, int word_x, int word_y, uint8_t *dest,
                         uint32_t width)
{
    uint32_t px = word_x < 0 ? num_x - 1 : (uint32_t)word_x;
    uint32_t py = word_y < 0 ? num_y - 1 : (uint32_t)word_y;
    uint32_t qx = (uint32_t)(word_x + 1);
    uint32_t qy = (uint32_t)(word_y + 1);
    uint32_t off_p = twiddle[py * num_x + px] * 2u;
    uint32_t off_q = twiddle[py * num_x + qx] * 2u;
    uint32_t off_r = twiddle[qy * num_x + px] * 2u;
    uint32_t off_s = twiddle[qy * num_x + qx] * 2u;
    uint32_t mod_p = words[off_p];
    uint32_t mod_q = words[off_q];
    uint32_t mod_r = words[off_r];
    uint32_t mod_s = words[off_s];
    uint32_t col_p = words[off_p + 1];
    uint32_t col_q = words[off_q + 1];
    uint32_t col_r = words[off_r + 1];
    uint32_t col_s = words[off_s + 1];
    uint32_t mode_p = col_p & 1u;
    uint32_t mode_q = col_q & 1u;
    uint32_t mode_r = col_r & 1u;
    uint32_t mode_s = col_s & 1u;
    int ca[4][4];
    int cb[4][4];
    uint8_t mod[16];

    pvrtc_color_a(col_p, ca[0]);
    pvrtc_color_a(col_q, ca[1]);
    pvrtc_color_a(col_r, ca[2]);
    pvrtc_color_a(col_s, ca[3]);
    pvrtc_color_b(col_p, cb[0]);
    pvrtc_color_b(col_q, cb[1]);
    pvrtc_color_b(col_r, cb[2]);
    pvrtc_color_b(col_s, cb[3]);

    /*
     * Each word contributes the centre 2x2 of its 4x4 modulation grid:
     * P bits 20/22/28/30, Q 16/18/24/26, R 4/6/12/14, S 0/2/8/10.
     */
    mod[0] = pvrtc_mod_value(mod_p, 20, mode_p);
    mod[1] = pvrtc_mod_value(mod_p, 22, mode_p);
    mod[4] = pvrtc_mod_value(mod_p, 28, mode_p);
    mod[5] = pvrtc_mod_value(mod_p, 30, mode_p);

    mod[2] = pvrtc_mod_value(mod_q, 16, mode_q);
    mod[3] = pvrtc_mod_value(mod_q, 18, mode_q);
    mod[6] = pvrtc_mod_value(mod_q, 24, mode_q);
    mod[7] = pvrtc_mod_value(mod_q, 26, mode_q);

    mod[8] = pvrtc_mod_value(mod_r, 4, mode_r);
    mod[9] = pvrtc_mod_value(mod_r, 6, mode_r);
    mod[12] = pvrtc_mod_value(mod_r, 12, mode_r);
    mod[13] = pvrtc_mod_value(mod_r, 14, mode_r);

    mod[10] = pvrtc_mod_value(mod_s, 0, mode_s);
    mod[11] = pvrtc_mod_value(mod_s, 2, mode_s);
    mod[14] = pvrtc_mod_value(mod_s, 8, mode_s);
    mod[15] = pvrtc_mod_value(mod_s, 10, mode_s);

    for (uint32_t col = 0; col < 4; col++) {
        int res_a[4];
        int step_a[4];
        int res_b[4];
        int step_b[4];
        uint32_t x = col < 2 ? px * 4u + 2u + col : qx * 4u + (col - 2u);

        /*
         * The reference interpolates 4x4 pixels from the 2x2 endpoint colours
         * with (4-x)(4-y) style weights (summing to 16). Reproduce its
         * incremental evaluation: the outer loop walks destination columns
         * (P towards Q) and the inner loop walks rows (P towards R). RGB is
         * scaled by 33/64, alpha by 17/16.
         */
        for (uint32_t c = 0; c < 4; c++) {
            int hp = 4 * ca[0][c] + (int)col * (ca[1][c] - ca[0][c]);
            int hr = 4 * ca[2][c] + (int)col * (ca[3][c] - ca[2][c]);

            res_a[c] = 4 * hp;
            step_a[c] = hr - hp;

            hp = 4 * cb[0][c] + (int)col * (cb[1][c] - cb[0][c]);
            hr = 4 * cb[2][c] + (int)col * (cb[3][c] - cb[2][c]);

            res_b[c] = 4 * hp;
            step_b[c] = hr - hp;
        }

        for (uint32_t row = 0; row < 4; row++) {
            uint8_t modulation = mod[row * 4 + col];
            uint32_t m = modulation & 0x0fu;
            uint32_t y = row < 2 ? py * 4u + 2u + row : qy * 4u + (row - 2u);
            uint8_t *pixel_dest = dest + (size_t)y * width * 4u + (size_t)x * 4u;
            uint32_t pixel = 0;

            for (uint32_t c = 0; c < 3; c++) {
                uint32_t va = ((uint32_t)res_a[c] >> 6) + ((uint32_t)res_a[c] >> 1);
                uint32_t vb = ((uint32_t)res_b[c] >> 6) + ((uint32_t)res_b[c] >> 1);
                uint32_t value = (va * (8 - m) + vb * m) >> 3;

                pixel |= value << (c * 8);
                res_a[c] += step_a[c];
                res_b[c] += step_b[c];
            }

            {
                uint32_t va = ((uint32_t)res_a[3] >> 4) + (uint32_t)res_a[3];
                uint32_t vb = ((uint32_t)res_b[3] >> 4) + (uint32_t)res_b[3];
                uint32_t value = (modulation & 0x80u) ? 0 : (va * (8 - m) + vb * m) >> 3;

                pixel |= value << 24;
                res_a[3] += step_a[3];
                res_b[3] += step_b[3];
            }

            memcpy(pixel_dest, &pixel, sizeof(pixel));
        }
    }
}

int gxm_texture_decode_pvrtc4_fast(const void *data, uint32_t width, uint32_t height, uint8_t *dest)
{
    const uint32_t *words = data;
    uint32_t num_x;
    uint32_t num_y;
    uint32_t *twiddle;

    if (!data || !dest || width < 8 || height < 8)
        return -1;
    if ((width & 3u) || (height & 3u) || !pvrtc_is_pow2(width) || !pvrtc_is_pow2(height))
        return -1;

    num_x = width / 4;
    num_y = height / 4;
    twiddle = malloc((size_t)num_x * num_y * sizeof(*twiddle));
    if (!twiddle)
        return -1;

    for (uint32_t y = 0; y < num_y; y++) {
        for (uint32_t x = 0; x < num_x; x++)
            twiddle[y * num_x + x] = pvrtc_twiddle(num_x, num_y, x, y);
    }

    for (int word_y = -1; word_y < (int)num_y - 1; word_y++) {
        for (int word_x = -1; word_x < (int)num_x - 1; word_x++)
            pvrtc4_block(words, num_x, num_y, twiddle, word_x, word_y, dest, width);
    }

    free(twiddle);
    return 0;
}
