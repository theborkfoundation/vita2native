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

        const std::string glsl = compiler.compile();

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
