#include "config.h"

#include <toml++/toml.hpp>
#include <system_error>

namespace launcher
{
namespace fs = std::filesystem;

ConfigResult load_config()
{
    ConfigResult result;
    std::error_code ec;
    const fs::path base = fs::current_path(ec);
    if (ec) {
        result.error = "Cannot determine the launcher directory: " + ec.message();
        return result;
    }
    result.config.games_path = (base / "games").lexically_normal();
    result.config.loader_path = (base / "v2n").lexically_normal();
    result.config.storage_path = (base / "storage").lexically_normal();
    const fs::path path = base / "launcher.toml";
    const fs::file_status status = fs::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory)
        return result;
    if (ec) {
        result.error = "Cannot inspect launcher.toml: " + ec.message();
        return result;
    }
    if (!fs::exists(status))
        return result;

    try {
        const toml::table root = toml::parse_file(path.string());
        auto section = [&](const char *name) -> const toml::table * {
            const toml::node *node = root.get(name);
            if (!node)
                return nullptr;
            if (const auto *table = node->as_table())
                return table;
            result.error = std::string("launcher.toml: [") + name + "] must be a table.";
            return nullptr;
        };
        auto read_path = [&](const toml::table *table, const char *section_name, const char *key,
                             fs::path &dest) {
            if (!table || !result.error.empty())
                return;
            const toml::node *node = table->get(key);
            if (!node)
                return;
            const auto value = node->value<std::string>();
            if (!value || value->empty() || value->find('\0') != std::string::npos) {
                result.error = std::string("launcher.toml: ") + section_name + "." + key +
                               " must be a nonempty path string.";
                return;
            }
            dest = (base / fs::path(*value)).lexically_normal();
        };
        const auto *paths = section("paths");
        if (paths && paths->contains("extract")) {
            result.error = "launcher.toml: paths.extract was renamed to paths.games.";
            return result;
        }
        read_path(paths, "paths", "games", result.config.games_path);
        read_path(paths, "paths", "loader", result.config.loader_path);
        read_path(paths, "paths", "storage", result.config.storage_path);
        if (!result.error.empty())
            return result;
        const auto *appearance = section("appearance");
        read_path(appearance, "appearance", "logo", result.config.logo_path);
    } catch (const toml::parse_error &error) {
        const auto position = error.source().begin;
        result.error = "launcher.toml:" + std::to_string(position.line) + ":" +
                       std::to_string(position.column) + ": " + std::string(error.description());
    } catch (const std::exception &error) {
        result.error = "Cannot read launcher.toml: " + std::string(error.what());
    }
    return result;
}
} // namespace launcher
