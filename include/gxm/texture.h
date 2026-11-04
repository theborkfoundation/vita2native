#ifndef GXM_TEXTURE_H
#define GXM_TEXTURE_H

#include <stdint.h>

#include <psp2/gxm.h>

/*
 * Decode a GXM texture (linear or Morton-swizzled) into tightly packed RGBA8.
 * `dest` must hold width * height * 4 bytes.
 *
 * Returns 0 on success and -1 for formats that are not decoded yet.
 */
int gxm_texture_decode_rgba8(uint8_t *dest, const void *src, uint32_t width, uint32_t height,
                             SceGxmTextureFormat format, SceGxmTextureType type);

/* Bytes per pixel for a GXM base format (0 if unknown). */
uint32_t gxm_texture_format_bytes(SceGxmTextureFormat format);

#endif
