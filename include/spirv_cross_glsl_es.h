#ifndef SPIRV_CROSS_GLSL_ES_H
#define SPIRV_CROSS_GLSL_ES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Translate SPIR-V (as emitted by Vita3K's GXP recompiler) to GLSL ES 3.10.
 * Returns 0 on success and stores a malloc()ed NUL-terminated string in
 * *glsl_out (caller frees it). Returns -1 on failure.
 *
 * With framebuffer_fetch the shader reads its own colour attachment, which is
 * how GXM expresses in-shader blending: the fragment output is turned into an
 * "inout" variable whose initial value is the destination pixel. With
 * arm_framebuffer_fetch the ARM variant is emitted instead, since some Mali
 * drivers only expose GL_ARM_shader_framebuffer_fetch (gl_LastFragColorARM).
 *
 * vertex_draw_index_location >= 0 rewrites a vertex shader's default uniform
 * container into a flat array indexed by a per-vertex attribute, so draws with
 * different uniforms can share one GL draw call. *draw_uniforms_applied is set
 * to true when the rewrite happened; the caller must then supply that
 * attribute. Pass -1 (and NULL) for fragment shaders.
 */
int spirv_to_glsl_es(const uint32_t *spirv, size_t num_words, bool framebuffer_fetch,
                     bool arm_framebuffer_fetch, int vertex_draw_index_location,
                     bool *draw_uniforms_applied, char **glsl_out);

#ifdef __cplusplus
}
#endif

#endif
