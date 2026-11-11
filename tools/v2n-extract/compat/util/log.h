/*
 * Minimal replacement for Vita3K's <util/log.h>, so the vendored SELF
 * decryption translation unit can be linked without spdlog or the emulator's
 * logging state.
 *
 * Only the macros that sce_utils.cpp uses are provided, with fmt-style
 * placeholder support so the messages keep their values.
 */
#ifndef V2N_COMPAT_UTIL_LOG_H
#define V2N_COMPAT_UTIL_LOG_H

#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>

#include <fmt/format.h>

/* Vita3K aliases fs:: in util/fs.h; only the std::filesystem part is needed
   by the SELF decryption code. */
namespace fs = std::filesystem;

/* Same as util/fs.h's FOPEN on POSIX. */
#ifndef FOPEN
#define FOPEN(filename, params) fopen(filename, params)
#endif

namespace v2n_compat {

template <typename... Args>
void log_line(const char *level, const char *format, Args &&...args)
{
    std::string text;

    if constexpr (sizeof...(Args) > 0) {
        try {
            text = fmt::format(fmt::runtime(format), std::forward<Args>(args)...);
        } catch (const std::exception &) {
            text = format;
        }
    } else {
        text = format;
    }
    std::fprintf(stderr, "v2n-extract: %s: %s\n", level, text.c_str());
}

} // namespace v2n_compat

#define LOG_DEBUG(...) v2n_compat::log_line("debug", __VA_ARGS__)
#define LOG_INFO(...) v2n_compat::log_line("info", __VA_ARGS__)
#define LOG_WARN(...) v2n_compat::log_line("warn", __VA_ARGS__)
#define LOG_ERROR(...) v2n_compat::log_line("error", __VA_ARGS__)
#define LOG_CRITICAL(...) v2n_compat::log_line("critical", __VA_ARGS__)

#endif
