#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <spirv_glsl.hpp>

#include "spirv_cross_glsl_es.h"

extern "C" int spirv_to_glsl_es(const uint32_t *spirv, size_t num_words, char **glsl_out)
{
    if (!spirv || num_words == 0 || !glsl_out)
        return -1;

    *glsl_out = nullptr;

    try {
        std::vector<uint32_t> words(spirv, spirv + num_words);
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
