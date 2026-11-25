#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <lvgl.h>

namespace launcher
{
struct ImageBuffer {
    lv_image_dsc_t dsc{};
    std::vector<uint8_t> pixels;

    void reset()
    {
        dsc = {};
        pixels.clear();
    }
    bool valid() const { return !pixels.empty() && dsc.header.w > 0 && dsc.header.h > 0; }
};

// Loads a PNG or SVG logo into an LVGL ARGB8888 image descriptor; returns false on failure.
bool load_logo(const std::filesystem::path &path, ImageBuffer &out);
} // namespace launcher
