#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <shader/gxp_parser.h>
#include <shader/spirv_recompiler.h>
#include <shader/usse_program_analyzer.h>
#include <shader/usse_translator_types.h>

#include "vita3k_shader_recompiler_iface_c.h"

extern "C" {

int vita3k_gxm_get_attribute_location(const SceGxmProgram *program, uint16_t reg_index)
{
    shader::usse::AttributeInformationMap locations;
    shader::usse::get_attribute_informations(*program, locations);
    const auto it = locations.find(reg_index);
    return it == locations.end() ? -1 : static_cast<int>(it->second.location);
}

uint32_t vita3k_gxm_get_uniform_buffer_layout(const SceGxmProgram *program,
                                              Vita3kUniformBufferLayout *out, uint32_t max)
{
    const auto input = shader::get_program_input(*program);
    std::map<uint32_t, uint32_t> member_sizes;
    uint32_t offset = 0;
    uint32_t count = 0;

    if (getenv("VITA3K_TRACE_UNIFORMS")) {
        const SceGxmProgramParameter *params = program->program_parameters();

        for (uint32_t i = 0; i < program->container_count; i++) {
            const SceGxmProgramParameterContainer &c = program->container()[i];

            std::fprintf(stderr,
                         "GXP program %p container %u: index=%u base_sa=%u size_in_f32=%u\n",
                         (const void *)program, i, (unsigned)c.container_index,
                         (unsigned)c.base_sa_offset, (unsigned)c.size_in_f32);
        }

        for (uint32_t i = 0; i < program->parameter_count; i++) {
            const SceGxmProgramParameter &p = params[i];

            std::fprintf(stderr,
                         "GXP program %p param %u: cat=%u type=%u comps=%u container=%u res=%u "
                         "array=%u name=%s\n",
                         (const void *)program, i, (unsigned)p.category, (unsigned)p.type,
                         (unsigned)p.component_count, (unsigned)p.container_index,
                         p.resource_index, p.array_size, p.name());
        }
    }

    for (const auto &buffer : input.uniform_buffers) {
        if (buffer.index >= SCE_GXM_REAL_MAX_UNIFORM_BUFFER)
            continue;

        const uint32_t host_index =
            buffer.index < SCE_GXM_MAX_UNIFORM_BUFFERS
                ? buffer.index + SCE_GXM_UNIFORM_BUFFER_OFFSET
                : SCE_GXM_DEFAULT_UNIFORM_BUFFER_CONTAINER_INDEX;
        const uint32_t floats = buffer.size;

        member_sizes[host_index] = ((floats + 3) / 4) * 16;
    }

    for (const auto &[host_index, member_size] : member_sizes) {
        if (count < max) {
            out[count].host_index = host_index;
            out[count].guest_index =
                host_index == SCE_GXM_DEFAULT_UNIFORM_BUFFER_CONTAINER_INDEX
                    ? SCE_GXM_MAX_UNIFORM_BUFFERS
                    : host_index - SCE_GXM_UNIFORM_BUFFER_OFFSET;
            out[count].offset = offset;
            out[count].member_size = member_size;
            out[count].guest_size = member_size;
        }

        offset += member_size;
        count++;
    }

    return offset;
}


static shader::GeneratedShader
convert_gxp_internal(const SceGxmProgram *program, const char *shader_name,
                     bool support_shader_interlock, bool support_texture_barrier,
                     bool direct_fragcolor, bool spirv_shader,
                     const SceGxmVertexAttribute *hint_attributes, uint32_t num_hint_attributes,
                     bool maskupdate, bool force_shader_debug,
                     bool (*dumper)(const char *ext, const char *dump), shader::Target target)
{
    shader::GeneratedShader shader;
    shader::Hints hints;
    FeatureState features;
    features.support_shader_interlock = support_shader_interlock;
    features.support_texture_barrier = support_texture_barrier;
    features.direct_fragcolor = direct_fragcolor;
    features.spirv_shader = spirv_shader;
    features.use_mask_bit = false;

    std::vector<SceGxmVertexAttribute> hint_attribs;
    for (uint32_t i = 0; i < num_hint_attributes; i++)
        hint_attribs.push_back(hint_attributes[i]);

    hints.attributes = &hint_attribs;
    /* TODO: Fill this properly */
    hints.color_format = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR;
    std::fill_n(hints.vertex_textures, SCE_GXM_MAX_TEXTURE_UNITS,
                SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    std::fill_n(hints.fragment_textures, SCE_GXM_MAX_TEXTURE_UNITS,
                SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);

    std::function<bool(const std::string &ext, const std::string &dump)> dumper_f =
        [dumper](const std::string &ext, const std::string &dump) {
            if (dumper)
                return dumper(ext.c_str(), dump.c_str());
            return true;
        };

    return shader::convert_gxp(*program, std::string(shader_name), features, target, hints,
                               maskupdate, force_shader_debug, dumper_f);
}

bool convert_gxp_to_spirv_c(uint32_t **spirv, uint32_t *num_instr, const SceGxmProgram *program,
                            const char *shader_name, bool support_shader_interlock,
                            bool support_texture_barrier, bool direct_fragcolor, bool spirv_shader,
                            const SceGxmVertexAttribute *hint_attributes,
                            uint32_t num_hint_attributes, bool maskupdate, bool force_shader_debug,
                            bool (*dumper)(const char *ext, const char *dump))
{
    shader::GeneratedShader shader;

    shader = convert_gxp_internal(program, shader_name, support_shader_interlock,
                                  support_texture_barrier, direct_fragcolor, spirv_shader,
                                  hint_attributes, num_hint_attributes, maskupdate,
                                  force_shader_debug, dumper, shader::Target::SpirVOpenGL);

    *num_instr = shader.spirv.size();
    *spirv = (uint32_t *)malloc(sizeof(uint32_t) * shader.spirv.size());
    memcpy(*spirv, shader.spirv.data(), sizeof(uint32_t) * shader.spirv.size());

    return true;
}

bool convert_gxp_to_glsl_c(char **glsl, const SceGxmProgram *program, const char *shader_name,
                           bool support_shader_interlock, bool support_texture_barrier,
                           bool direct_fragcolor, bool spirv_shader,
                           const SceGxmVertexAttribute *hint_attributes,
                           uint32_t num_hint_attributes, bool maskupdate, bool force_shader_debug,
                           bool (*dumper)(const char *ext, const char *dump))
{
    shader::GeneratedShader shader;

    shader = convert_gxp_internal(program, shader_name, support_shader_interlock,
                                  support_texture_barrier, direct_fragcolor, spirv_shader,
                                  hint_attributes, num_hint_attributes, maskupdate,
                                  force_shader_debug, dumper, shader::Target::GLSLOpenGL);

    *glsl = (char *)malloc(shader.glsl.size() + 1);
    strcpy(*glsl, shader.glsl.c_str());

    return true;
}
}
