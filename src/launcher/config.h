#pragma once

#include <filesystem>
#include <string>

namespace launcher
{
struct Config {
    std::filesystem::path extract_path;
    std::filesystem::path loader_path;
    std::filesystem::path logo_path;
};

struct ConfigResult {
    Config config;
    std::string error;
};

// launcher.toml and all relative paths are resolved against the startup directory.
ConfigResult load_config();
} // namespace launcher
