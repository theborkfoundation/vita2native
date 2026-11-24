#pragma once

#include <filesystem>
#include <string>

namespace launcher
{
struct State;
namespace ui
{
bool initialize(const std::filesystem::path &logo_path, std::string &warning);
void shutdown();
void begin_frame(int drawable_width);
void draw(const State &state);
void end_frame();
} // namespace ui
} // namespace launcher
