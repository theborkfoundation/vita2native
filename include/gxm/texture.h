#ifndef GXM_TEXTURE_H
#define GXM_TEXTURE_H

#include <stdint.h>

#include <psp2/gxm.h>

#include "gxm/util.h"

/*
 * Description of one mip level inside a GXM texture.
 *
 * `width`/`height` are the logical dimensions of the level. `layout_width` is
 * the number of texels stored per row (linear and tiled layouts pad rows), and
 * `offset` is the byte offset from the start of the texture data (including the
 * cube face, if any).
 */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t layout_width;
    uint32_t layout_height;
    uint32_t offset;
    uint32_t size;
} GxmTextureLevel;

/* Bits per texel for a GXM base format (0 when the format is unknown). */
uint32_t gxm_texture_base_bits(SceGxmTextureFormat format);

/* Bytes per texel, rounded up (P4 returns 1). */
uint32_t gxm_texture_format_bytes(SceGxmTextureFormat format);

/*
 * Number of mip levels actually stored. The 0xF "no mip chain" encoding maps to
 * one level, matching Vita3K's true_mip_count().
 */
uint32_t gxm_texture_true_mip_count(const SceGxmTextureInner *texture);

/*
 * Computes the layout of `level` for the given cube `face` (0 for 2D
 * textures). Returns 0 on success, -1 for an out-of-range face/level.
 */
int gxm_texture_get_level(const SceGxmTextureInner *texture, uint32_t face, uint32_t level,
                          GxmTextureLevel *out);

/*
 * Decodes one mip level (and cube face) into tightly packed RGBA8. `dest` must
 * hold level.width * level.height * 4 bytes. Paletted formats read the palette
 * from texture->palette_addr. Returns 0 on success, -1 for unsupported formats.
 */
int gxm_texture_decode_level_rgba8(uint8_t *dest, const SceGxmTextureInner *texture, uint32_t face,
                                   uint32_t level);

/* Same as above, but with an explicit base data pointer. */
int gxm_texture_decode_level_at(uint8_t *dest, const SceGxmTextureInner *texture, const void *data,
                                uint32_t face, uint32_t level);

/*
 * True when the texture stores RGBA4444 texels that can be decoded straight to
 * a GL_RGBA4 upload without a lossy conversion (gamma-corrected textures are
 * excluded because the gamma table is applied to 8-bit channels).
 */
bool gxm_texture_can_decode_rgba4(const SceGxmTextureInner *texture);

/*
 * Decodes one U4U4U4U4 mip level (and cube face) into packed RGBA4444 words in
 * the GL_UNSIGNED_SHORT_4_4_4_4 order (R in the high nibble). `dest` must hold
 * level.width * level.height * 2 bytes. Returns 0 on success, -1 when the
 * format is not U4U4U4U4.
 */
int gxm_texture_decode_level_rgba4(uint16_t *dest, const SceGxmTextureInner *texture, uint32_t face,
                                   uint32_t level);

/* Same as above, but with an explicit base data pointer. */
int gxm_texture_decode_level_rgba4_at(uint16_t *dest, const SceGxmTextureInner *texture,
                                      const void *data, uint32_t face, uint32_t level);

/*
 * Decodes one F32/F32M level into a tightly packed float buffer (one value per
 * texel). Used to upload shadow/depth maps without losing precision.
 */
int gxm_texture_decode_level_f32(float *dest, const SceGxmTextureInner *texture, const void *data,
                                 uint32_t face, uint32_t level);

/*
 * Compatibility entry point: decode a single linear/swizzled level from an
 * explicit pointer. `dest` must hold width * height * 4 bytes.
 */
int gxm_texture_decode_rgba8(uint8_t *dest, const void *src, uint32_t width, uint32_t height,
                             SceGxmTextureFormat format, SceGxmTextureType type);

/*
 * Compatibility entry point for the RGBA4444-native decoder. `dest` must hold
 * width * height * 2 bytes. Only U4U4U4U4 formats are supported.
 */
int gxm_texture_decode_rgba4(uint16_t *dest, const void *src, uint32_t width, uint32_t height,
                             SceGxmTextureFormat format, SceGxmTextureType type);

#endif
