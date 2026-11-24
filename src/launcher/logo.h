#pragma once

#include <filesystem>

struct NVGcontext;

namespace launcher
{
// Creates a NanoVG image; returns zero when the PNG or SVG cannot be loaded.
int load_logo(NVGcontext *vg, const std::filesystem::path &path);
} // namespace launcher
