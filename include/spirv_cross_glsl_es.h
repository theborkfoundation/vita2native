#ifndef SPIRV_CROSS_GLSL_ES_H
#define SPIRV_CROSS_GLSL_ES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Translate SPIR-V (as emitted by Vita3K's GXP recompiler) to GLSL ES 3.10.
 * Returns 0 on success and stores a malloc()ed NUL-terminated string in
 * *glsl_out (caller frees it). Returns -1 on failure.
 */
int spirv_to_glsl_es(const uint32_t *spirv, size_t num_words, char **glsl_out);

#ifdef __cplusplus
}
#endif

#endif
