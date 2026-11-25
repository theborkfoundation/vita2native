#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace launcher
{
struct GameEntry {
    std::string title_id;
    std::string name;
    std::string description;
    std::string category;
    std::string rating;
    std::filesystem::path dir;
    std::filesystem::path eboot;
    std::filesystem::path art;
    std::filesystem::path boxart;
    std::vector<std::filesystem::path> screenshots;
};

inline constexpr int recent_game_limit = 8;

// Scans <games_path>/*/eboot.bin and loads optional companion v2n.toml metadata.
std::vector<GameEntry> scan_games(const std::filesystem::path &games_path);
} // namespace launcher
