#ifndef VITA3K_SHADER_RECOMPILER_IFACE_C_H
#define VITA3K_SHADER_RECOMPILER_IFACE_C_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t guest_index;  /* GXM uniform buffer index (14 is the default buffer) */
    uint32_t host_index;   /* member index in the SSBO container (0 is default) */
    uint32_t offset;       /* byte offset in the SSBO container */
    uint32_t guest_size;   /* bytes the guest buffer occupies */
    uint32_t member_size;  /* bytes reserved in the container (16-byte aligned) */
} Vita3kUniformBufferLayout;

bool convert_gxp_to_spirv_c(uint32_t **spirv, uint32_t *num_instr, const SceGxmProgram *program,
                            const char *shader_name, bool support_shader_interlock,
                            bool support_texture_barrier, bool direct_fragcolor, bool spirv_shader,
                            const SceGxmVertexAttribute *hint_attributes,
                            uint32_t num_hint_attributes, bool maskupdate, bool force_shader_debug,
                            bool (*dumper)(const char *ext, const char *dump));

bool convert_gxp_to_glsl_c(char **glsl, const SceGxmProgram *program, const char *shader_name,
                           bool support_shader_interlock, bool support_texture_barrier,
                           bool direct_fragcolor, bool spirv_shader,
                           const SceGxmVertexAttribute *hint_attributes,
                           uint32_t num_hint_attributes, bool maskupdate, bool force_shader_debug,
                           bool (*dumper)(const char *ext, const char *dump));

/*
 * Returns the storage-buffer layout Vita3K's SPIR-V recompiler uses for the
 * program's uniform buffers (default uniforms and named buffers share one
 * container per stage). Returns the total container size in bytes and fills up
 * to `max` entries in host-index order.
 */
uint32_t vita3k_gxm_get_uniform_buffer_layout(const SceGxmProgram *program,
                                              Vita3kUniformBufferLayout *out, uint32_t max);

#ifdef __cplusplus
}
#endif

#endif
