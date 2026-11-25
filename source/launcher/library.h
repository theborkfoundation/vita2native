#pragma once

#include <array>

namespace launcher
{
// Placeholder data until title scanning is implemented.
inline constexpr std::array<const char *, 4> game_names = { "Horizon Drift", "Moonlit Valley",
                                                            "Neon Circuit", "Little Voyager" };
inline constexpr int game_count = static_cast<int>(game_names.size());
} // namespace launcher
