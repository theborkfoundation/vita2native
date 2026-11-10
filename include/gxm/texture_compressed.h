#ifndef GXM_TEXTURE_COMPRESSED_H
#define GXM_TEXTURE_COMPRESSED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Decodes a PVRTC image (including the PVRTC-II variants) into tightly packed
 * RGBA8. `width`/`height` are the stored layout dimensions (already padded to
 * the block size); `two_bpp` selects 2bpp vs 4bpp and `pvrt2` selects PVRTC-II.
 */
int gxm_texture_decode_pvrtc(const void *data, uint32_t width, uint32_t height, uint32_t two_bpp,
                             uint32_t pvrt2, uint8_t *dest);

/* Reference PowerVR SDK decoder. Used as the golden implementation by tests. */
int gxm_texture_decode_pvrtc_reference(const void *data, uint32_t width, uint32_t height,
                                       uint32_t two_bpp, uint32_t pvrt2, uint8_t *dest);

/*
 * Optimised PVRTC-I 4bpp decoder. Writes tightly packed RGBA8. Returns -1 when
 * the input is not a power-of-two PVRTC-I 4bpp image, so callers can fall back
 * to the reference decoder.
 */
int gxm_texture_decode_pvrtc4_fast(const void *data, uint32_t width, uint32_t height,
                                   uint8_t *dest);

#ifdef __cplusplus
}
#endif

#endif
