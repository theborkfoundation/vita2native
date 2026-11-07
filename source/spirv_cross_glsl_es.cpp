#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <spirv_glsl.hpp>

#include "spirv_cross_glsl_es.h"

namespace {

enum {
    SpvOpDecoration = 71,
    SpvOpTypePointer = 32,
    SpvOpVariable = 59,
    SpvStorageClassUniform = 2,
    SpvStorageClassStorageBuffer = 12,
    SpvDecorationNonWritable = 24,
};

/*
 * GLES 3.1 only *requires* MAX_FRAGMENT_SHADER_STORAGE_BLOCKS >= 4; the vertex
 * stage minimum is 0, and drivers such as Panfrost on Mali report exactly 0.
 * The GXP recompiler feeds the default/named uniform container through a
 * read-only storage buffer, which therefore has to become a uniform block for
 * the vertex stage to link at all.
 *
 * The container is laid out as consecutive `vec4[N]` members with 16-byte
 * strides and offsets, which is identical under std140 and std430, so the
 * retag does not change the CPU-side packing.
 */
void rewrite_readonly_storage_buffers(std::vector<uint32_t> &words)
{
    if (words.size() < 5)
        return;

    std::vector<uint32_t> pointer_types;
    std::vector<uint32_t> readonly_vars;
    size_t i = 5;

    while (i < words.size()) {
        const uint32_t instruction = words[i];
        const uint16_t opcode = instruction & 0xffffu;
        const uint16_t count = instruction >> 16;

        if (count == 0 || i + count > words.size())
            break;

        if (opcode == SpvOpVariable && count >= 4 &&
            words[i + 3] == SpvStorageClassStorageBuffer) {
            pointer_types.push_back(words[i + 1]);
            readonly_vars.push_back(words[i + 2]);
        }

        i += count;
    }

    if (readonly_vars.empty())
        return;

    std::vector<uint32_t> is_readonly(readonly_vars.size(), 0);

    for (size_t v = 0; v < readonly_vars.size(); v++) {
        const uint32_t var = readonly_vars[v];

        for (size_t j = 5; j < words.size();) {
            const uint32_t instruction = words[j];
            const uint16_t opcode = instruction & 0xffffu;
            const uint16_t count = instruction >> 16;

            if (count == 0 || j + count > words.size())
                break;

            if (opcode == SpvOpDecoration && count >= 3 && words[j + 1] == var &&
                words[j + 2] == SpvDecorationNonWritable) {
                is_readonly[v] = 1;
                break;
            }

            j += count;
        }
    }

    /* Only read-only blocks are rewritten: a uniform block cannot be written. */
    for (size_t v = 0; v < readonly_vars.size(); v++) {
        if (!is_readonly[v])
            continue;

        for (size_t j = 5; j < words.size();) {
            const uint32_t instruction = words[j];
            const uint16_t opcode = instruction & 0xffffu;
            const uint16_t count = instruction >> 16;

            if (count == 0 || j + count > words.size())
                break;

            if (opcode == SpvOpTypePointer && count >= 4 &&
                words[j + 1] == pointer_types[v] &&
                words[j + 2] == SpvStorageClassStorageBuffer) {
                words[j + 2] = SpvStorageClassUniform;
            } else if (opcode == SpvOpVariable && count >= 4 &&
                       words[j + 2] == readonly_vars[v] &&
                       words[j + 3] == SpvStorageClassStorageBuffer) {
                words[j + 3] = SpvStorageClassUniform;
            }

            j += count;
        }
    }
}

} // namespace

namespace {

void replace_all(std::string &text, const std::string &needle, const std::string &replacement)
{
    size_t pos = 0;

    while ((pos = text.find(needle, pos)) != std::string::npos) {
        text.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
}

/*
 * SPIRV-Cross lowers OpImageQueryLod to textureQueryLOD() and requires
 * GL_EXT_texture_query_lod for ES targets. Panfrost (Mali-G31) does not expose
 * that extension, so any shader using SMP.info or SMP gather4.uv fails to
 * compile. Compute the implicit LOD from screen-space derivatives instead,
 * which is what the extension does under the hood.
 */
void lower_texture_query_lod(std::string &glsl)
{
    static const char extension_require[] = "#extension GL_EXT_texture_query_lod : require\n";
    static const char extension_enable[] = "#extension GL_EXT_texture_query_lod : enable\n";

    if (glsl.find("textureQueryLOD(") == std::string::npos)
        return;

    replace_all(glsl, extension_require, "");
    replace_all(glsl, extension_enable, "");
    replace_all(glsl, "textureQueryLOD(", "v2n_textureQueryLOD(");

    static const char helpers[] =
        "highp vec2 v2n_textureQueryLOD(highp sampler2D s, highp vec2 p)\n"
        "{\n"
        "    highp vec2 size = vec2(textureSize(s, 0));\n"
        "    highp vec2 dx = dFdx(p * size);\n"
        "    highp vec2 dy = dFdy(p * size);\n"
        "    highp float lod = 0.5 * log2(max(dot(dx, dx), dot(dy, dy)));\n"
        "    highp float max_lod = log2(max(size.x, size.y));\n"
        "    return vec2(clamp(lod, 0.0, max_lod), p);\n"
        "}\n"
        "highp vec2 v2n_textureQueryLOD(highp sampler3D s, highp vec3 p)\n"
        "{\n"
        "    highp vec3 size = vec3(textureSize(s, 0));\n"
        "    highp vec3 dx = dFdx(p * size);\n"
        "    highp vec3 dy = dFdy(p * size);\n"
        "    highp float lod = 0.5 * log2(max(dot(dx, dx), dot(dy, dy)));\n"
        "    highp float max_lod = log2(max(max(size.x, size.y), size.z));\n"
        "    return vec2(clamp(lod, 0.0, max_lod), p.x);\n"
        "}\n"
        "highp vec2 v2n_textureQueryLOD(highp samplerCube s, highp vec3 p)\n"
        "{\n"
        "    highp vec2 size = vec2(textureSize(s, 0));\n"
        "    highp vec3 dx = dFdx(p * vec3(size, size.x));\n"
        "    highp vec3 dy = dFdy(p * vec3(size, size.x));\n"
        "    highp float lod = 0.5 * log2(max(dot(dx, dx), dot(dy, dy)));\n"
        "    highp float max_lod = log2(max(size.x, size.y));\n"
        "    return vec2(clamp(lod, 0.0, max_lod), p.x);\n"
        "}\n"
        "highp vec2 v2n_textureQueryLOD(highp sampler2DArray s, highp vec3 p)\n"
        "{\n"
        "    highp vec2 size = vec2(textureSize(s, 0).xy);\n"
        "    highp vec2 dx = dFdx(p.xy * size);\n"
        "    highp vec2 dy = dFdy(p.xy * size);\n"
        "    highp float lod = 0.5 * log2(max(dot(dx, dx), dot(dy, dy)));\n"
        "    highp float max_lod = log2(max(size.x, size.y));\n"
        "    return vec2(clamp(lod, 0.0, max_lod), p.x);\n"
        "}\n";

    /* Extension directives must precede every other statement, so the helpers
       go right after the last #extension line (or the #version line). */
    size_t insert_at = std::string::npos;
    size_t line_start = 0;

    while (line_start < glsl.size()) {
        size_t line_end = glsl.find('\n', line_start);

        if (line_end == std::string::npos)
            line_end = glsl.size();
        else
            line_end++;

        const std::string line = glsl.substr(line_start, line_end - line_start);

        if (line.compare(0, 11, "#extension ") == 0 ||
            line.compare(0, 8, "#version") == 0)
            insert_at = line_end;

        line_start = line_end;
    }

    if (insert_at == std::string::npos)
        insert_at = 0;

    glsl.insert(insert_at, helpers);
}

} // namespace

extern "C" int spirv_to_glsl_es(const uint32_t *spirv, size_t num_words, char **glsl_out)
{
    if (!spirv || num_words == 0 || !glsl_out)
        return -1;

    *glsl_out = nullptr;

    try {
        std::vector<uint32_t> words(spirv, spirv + num_words);
        rewrite_readonly_storage_buffers(words);

        spirv_cross::CompilerGLSL compiler(std::move(words));

        spirv_cross::CompilerGLSL::Options options;
        options.es = true;
        options.version = 310;
        options.enable_420pack_extension = false;
        compiler.set_common_options(options);

        std::string glsl = compiler.compile();
        lower_texture_query_lod(glsl);

        char *copy = static_cast<char *>(malloc(glsl.size() + 1));
        if (!copy)
            return -1;
        memcpy(copy, glsl.c_str(), glsl.size() + 1);
        *glsl_out = copy;

        return 0;
    } catch (...) {
        return -1;
    }
}
