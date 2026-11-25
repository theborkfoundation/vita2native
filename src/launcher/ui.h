#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace launcher
{
struct State;
namespace ui
{
bool initialize(const std::filesystem::path &logo_path, std::string &warning);
void shutdown();
void sync(const State &state);
bool render(bool force = false);
const uint8_t *framebuffer();
} // namespace ui
} // namespace launcher
