#include "logo.h"

#include <lunasvg.h>
#include <stb_image.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace launcher
{
namespace
{
void init_argb8888_from_rgba(ImageBuffer &out, int width, int height, const uint8_t *rgba,
                              int stride)
{
    out.pixels.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        const uint8_t *src = rgba + static_cast<size_t>(y) * stride;
        uint8_t *dst = out.pixels.data() + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    out.dsc = {};
    out.dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    out.dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    out.dsc.header.w = static_cast<uint32_t>(width);
    out.dsc.header.h = static_cast<uint32_t>(height);
    out.dsc.header.stride = static_cast<uint32_t>(width * 4);
    out.dsc.data_size = static_cast<uint32_t>(out.pixels.size());
    out.dsc.data = out.pixels.data();
}
} // namespace

bool load_logo(const std::filesystem::path &path, ImageBuffer &out)
{
    out.reset();
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".png") {
        int width = 0, height = 0, channels = 0;
        stbi_uc *rgba = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
        if (!rgba || width <= 0 || height <= 0) {
            if (rgba)
                stbi_image_free(rgba);
            return false;
        }
        init_argb8888_from_rgba(out, width, height, rgba, width * 4);
        stbi_image_free(rgba);
        return true;
    }
    if (extension != ".svg")
        return false;
    auto document = lunasvg::Document::loadFromFile(path.string());
    if (!document || !std::isfinite(document->width()) || !std::isfinite(document->height()) ||
        document->width() <= 0 || document->height() <= 0)
        return false;
    constexpr float raster_size = 256.0f;
    const float largest = std::max(document->width(), document->height());
    const int width =
        std::max(1, static_cast<int>(std::round(document->width() / largest * raster_size)));
    const int height =
        std::max(1, static_cast<int>(std::round(document->height() / largest * raster_size)));
    auto bitmap = document->renderToBitmap(width, height);
    if (bitmap.isNull())
        return false;
    bitmap.convertToRGBA();
    init_argb8888_from_rgba(out, width, height, bitmap.data(), bitmap.stride());
    return true;
}

} // namespace launcher
