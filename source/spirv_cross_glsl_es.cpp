#include <cctype>
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

/*
 * Every draw carries its own GXM uniform container, and a batch may contain
 * dozens of draws with different matrices. SPIRV-Cross emits the container as
 * a uniform block (`vertexDataType { vec4 buffer0[N]; ... }`), which forces one
 * upload and one state flush per draw. Rewriting the accesses to a flat array
 * indexed by a per-vertex draw slot lets all of them share a single draw call.
 * The array size matches the 16 KiB minimum GLES 3.10 uniform block size.
 */
constexpr unsigned kDrawUniformVecs = 1024;
constexpr unsigned kDrawUniformBinding = 4;

size_t find_closing_bracket(const std::string &text, size_t open)
{
    int depth = 0;

    for (size_t i = open; i < text.size(); i++) {
        if (text[i] == '[') {
            depth++;
        } else if (text[i] == ']') {
            depth--;
            if (depth == 0)
                return i;
        }
    }

    return std::string::npos;
}

bool apply_draw_uniforms(std::string &glsl, int location)
{
    static const std::string block_marker = "uniform vertexDataType";
    static const std::string variable_marker = "} vertexData;";
    size_t block = glsl.find(block_marker);
    size_t open, close;

    if (location < 0 || block == std::string::npos)
        return false;

    open = glsl.find('{', block);
    close = open == std::string::npos ? std::string::npos : glsl.find(variable_marker, open);
    if (open == std::string::npos || close == std::string::npos)
        return false;

    struct Member {
        std::string name;
        unsigned base;
    };
    std::vector<Member> members;
    unsigned total = 0;

    for (size_t pos = open + 1; pos < close;) {
        size_t declaration = glsl.find("vec4 ", pos);
        size_t name_start, bracket, end;
        std::string name;
        unsigned count;

        if (declaration == std::string::npos || declaration >= close)
            break;

        name_start = declaration + 5;
        while (name_start < close && std::isspace(static_cast<unsigned char>(glsl[name_start])))
            name_start++;
        bracket = glsl.find('[', name_start);
        end = bracket == std::string::npos ? std::string::npos : glsl.find(']', bracket);
        if (bracket == std::string::npos || bracket >= close || end == std::string::npos ||
            end >= close)
            return false;

        name = glsl.substr(name_start, bracket - name_start);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
            name.pop_back();

        count = static_cast<unsigned>(std::strtoul(glsl.c_str() + bracket + 1, nullptr, 10));
        if (name.empty() || count == 0)
            return false;

        members.push_back({ name, total });
        total += count;
        pos = end + 1;
    }

    if (members.empty() || total == 0 || total > kDrawUniformVecs)
        return false;

    std::string rewritten = glsl;

    for (const Member &member : members) {
        const std::string needle = "vertexData." + member.name + "[";
        size_t at = 0;

        while ((at = rewritten.find(needle, at)) != std::string::npos) {
            size_t expression = at + needle.size();
            size_t end = find_closing_bracket(rewritten, expression - 1);
            std::string index;
            std::string replacement;

            if (end == std::string::npos)
                return false;

            index = rewritten.substr(expression, end - expression);
            replacement = "v2nDrawUniforms.data[v2nDrawBase + " + std::to_string(member.base) +
                          " + (" + index + ")]";
            rewritten.replace(at, end + 1 - at, replacement);
            at += replacement.size();
        }
    }

    /* A reference to a member the block parser did not see would keep reading
       the (now unbound) block. Leave the shader untouched instead. */
    if (rewritten.find("vertexData.") != std::string::npos)
        return false;

    size_t main_position = rewritten.find("void main()");
    size_t main_brace =
        main_position == std::string::npos ? std::string::npos : rewritten.find('{', main_position);
    if (main_brace == std::string::npos)
        return false;

    size_t line_start = rewritten.rfind('\n', block);
    size_t declaration_end;
    std::string declaration;

    line_start = line_start == std::string::npos ? 0 : line_start + 1;
    declaration_end = rewritten.find(variable_marker, close);
    if (declaration_end == std::string::npos)
        return false;
    declaration_end += variable_marker.size();
    while (declaration_end < rewritten.size() && rewritten[declaration_end] == '\n')
        declaration_end++;

    declaration = "layout(binding = " + std::to_string(kDrawUniformBinding) +
                  ", std140) uniform v2nDrawUniformBlock\n{\n    vec4 data[" +
                  std::to_string(kDrawUniformVecs) +
                  "];\n} v2nDrawUniforms;\nint v2nDrawBase;\nlayout(location = " +
                  std::to_string(location) + ") in highp float v2nDrawIndex;\n";
    rewritten.replace(line_start, declaration_end - line_start, declaration);

    main_position = rewritten.find("void main()");
    main_brace = main_position == std::string::npos
                     ? std::string::npos
                     : rewritten.find('{', main_position);
    if (main_brace == std::string::npos)
        return false;

    rewritten.insert(main_brace + 1, "\n    v2nDrawBase = int(v2nDrawIndex) * " +
                                         std::to_string(total) + ";");

    glsl.swap(rewritten);
    return true;
}

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

/*
 * GL_ARM_shader_framebuffer_fetch has no "inout" fragment outputs; the previous
 * colour is the read-only built-in gl_LastFragColorARM. Rewrite the EXT-style
 * output into a plain out and prime it at the start of main. SPIRV-Cross emits
 * a copy of that output into the subpass input at the same point, so the
 * shader body still reads the framebuffer contents it expects.
 */
void rewrite_arm_framebuffer_fetch(std::string &glsl)
{
    static const std::string ext_name = "GL_EXT_shader_framebuffer_fetch";
    static const std::string arm_name = "GL_ARM_shader_framebuffer_fetch";
    std::string output_name;
    size_t position = 0;

    while ((position = glsl.find(ext_name, position)) != std::string::npos) {
        const size_t after = position + ext_name.size();

        /* Leave GL_EXT_shader_framebuffer_fetch_non_coherent alone. */
        if (after < glsl.size() && glsl[after] == '_') {
            position = after;
            continue;
        }
        glsl.replace(position, ext_name.size(), arm_name);
        position += arm_name.size();
    }

    size_t line_start = 0;

    while (line_start < glsl.size()) {
        const size_t newline = glsl.find('\n', line_start);
        const size_t line_end = newline == std::string::npos ? glsl.size() : newline + 1;
        std::string line = glsl.substr(line_start, line_end - line_start);
        const size_t inout_pos = line.find("inout ");
        const size_t semicolon = line.rfind(';');

        if (inout_pos != std::string::npos && semicolon != std::string::npos &&
            line.find("layout(location") != std::string::npos) {
            size_t name_end = semicolon;
            size_t name_start;

            while (name_end > 0 && std::isspace(static_cast<unsigned char>(line[name_end - 1])))
                name_end--;
            name_start = name_end;
            while (name_start > 0 && !std::isspace(static_cast<unsigned char>(line[name_start - 1])))
                name_start--;
            output_name = line.substr(name_start, name_end - name_start);
            line.replace(inout_pos, 6, "out    ");
            glsl.replace(line_start, line_end - line_start, line);
            break;
        }

        line_start = line_end;
    }

    if (output_name.empty())
        return;

    const size_t main_pos = glsl.find("void main()");
    const size_t brace = main_pos == std::string::npos ? std::string::npos : glsl.find('{', main_pos);

    if (brace == std::string::npos)
        return;

    glsl.insert(brace + 1, "\n    " + output_name + " = gl_LastFragColorARM;");
}

} // namespace

extern "C" int spirv_to_glsl_es(const uint32_t *spirv, size_t num_words, bool framebuffer_fetch,
                                bool arm_framebuffer_fetch, int vertex_draw_index_location,
                                bool *draw_uniforms_applied, char **glsl_out)
{
    if (!spirv || num_words == 0 || !glsl_out)
        return -1;

    *glsl_out = nullptr;
    if (draw_uniforms_applied)
        *draw_uniforms_applied = false;

    try {
        std::vector<uint32_t> words(spirv, spirv + num_words);
        rewrite_readonly_storage_buffers(words);

        spirv_cross::CompilerGLSL compiler(std::move(words));

        spirv_cross::CompilerGLSL::Options options;
        options.es = true;
        options.version = 310;
        options.enable_420pack_extension = false;
        compiler.set_common_options(options);

        /* The recompiler declares the destination colour as a subpass input at
           attachment 0; GLES exposes the same thing as an inout fragment output
           initialised with the framebuffer contents. */
        if (framebuffer_fetch)
            compiler.remap_ext_framebuffer_fetch(0, 0, true);

        std::string glsl = compiler.compile();
        lower_texture_query_lod(glsl);
        if (framebuffer_fetch && arm_framebuffer_fetch)
            rewrite_arm_framebuffer_fetch(glsl);
        if (draw_uniforms_applied)
            *draw_uniforms_applied = apply_draw_uniforms(glsl, vertex_draw_index_location);

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
