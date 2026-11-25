#include "config.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <toml++/toml.hpp>

namespace
{
namespace fs = std::filesystem;

std::string g_storage_root = "storage";
bool g_config_loaded = false;

void load_storage_from_toml()
{
    g_config_loaded = true;
    std::error_code ec;
    const fs::path path("launcher.toml");
    if (!fs::exists(path, ec) || ec)
        return;

    try {
        const toml::table root = toml::parse_file(path.string());
        const toml::table *paths = root["paths"].as_table();
        if (!paths)
            return;
        const auto value = (*paths)["storage"].value<std::string>();
        if (!value || value->empty() || value->find('\0') != std::string::npos)
            return;

        std::string normalized = fs::path(*value).lexically_normal().string();
        while (normalized.size() > 1 && normalized.back() == '/')
            normalized.pop_back();
        if (!normalized.empty())
            g_storage_root = std::move(normalized);
    } catch (const std::exception &) {
        // Keep default "storage" if launcher.toml is malformed.
    }
}
} // namespace

extern "C" const char *v2n_storage_root(void)
{
    if (!g_config_loaded)
        load_storage_from_toml();
    return g_storage_root.c_str();
}

extern "C" int v2n_storage_path(char *out, size_t out_size, const char *subpath)
{
    const char *root = v2n_storage_root();
    if (!out || out_size == 0)
        return -1;
    if (!subpath || !subpath[0])
        return std::snprintf(out, out_size, "%s", root) >= static_cast<int>(out_size) ? -1 : 0;
    return std::snprintf(out, out_size, "%s/%s", root, subpath) >= static_cast<int>(out_size)
               ? -1
               : 0;
}
