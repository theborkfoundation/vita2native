#include "logo.h"

#include <nanovg.h>
#include <lunasvg.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

namespace launcher
{
int load_logo(NVGcontext *vg, const std::filesystem::path &path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".png")
        return nvgCreateImage(vg, path.string().c_str(), 0);
    if (extension != ".svg")
        return 0;
    auto document = lunasvg::Document::loadFromFile(path.string());
    if (!document || !std::isfinite(document->width()) || !std::isfinite(document->height()) ||
        document->width() <= 0 || document->height() <= 0)
        return 0;
    constexpr float raster_size = 256.0f;
    const float largest = std::max(document->width(), document->height());
    const int width = std::max(1, static_cast<int>(std::round(document->width() / largest * raster_size)));
    const int height = std::max(1, static_cast<int>(std::round(document->height() / largest * raster_size)));
    auto bitmap = document->renderToBitmap(width, height);
    if (bitmap.isNull())
        return 0;
    bitmap.convertToRGBA();
    if (bitmap.stride() == width * 4)
        return nvgCreateImageRGBA(vg, width, height, 0, bitmap.data());
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y)
        std::memcpy(pixels.data() + static_cast<size_t>(y) * width * 4,
                    bitmap.data() + static_cast<size_t>(y) * bitmap.stride(), width * 4);
    return nvgCreateImageRGBA(vg, width, height, 0, pixels.data());
}

} // namespace launcher
