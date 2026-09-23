#include "paper/assets/image.hpp"
#include "paper/core/pixel_format.hpp"
#include "stb_image.h"
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>
namespace paper {
LoadedSprite loadSprite(const std::filesystem::path& path) {
    int w = 0, h = 0, n = 0;
    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels(
        stbi_load(path.string().c_str(), &w, &h, &n, pixelFormat::channels), &stbi_image_free);
    if (!pixels)
        throw std::runtime_error("Cannot load image resource: " + path.string());
    PaintedTexture art;
    art.width = w;
    art.height = h;
    art.pixels.resize(static_cast<size_t>(w) * h);
    int firstRow = h, lastRow = -1;
    for (size_t i = 0; i < art.pixels.size(); ++i) {
        art.pixels[i] = {pixels.get()[i * pixelFormat::channels + pixelFormat::red],
                         pixels.get()[i * pixelFormat::channels + pixelFormat::green],
                         pixels.get()[i * pixelFormat::channels + pixelFormat::blue],
                         pixels.get()[i * pixelFormat::channels + pixelFormat::alpha]};
        if (pixels.get()[i * pixelFormat::channels + pixelFormat::alpha] >=
            pixelFormat::opaqueAlpha) {
            int row = static_cast<int>(i / static_cast<size_t>(w));
            firstRow = std::min(firstRow, row);
            lastRow = std::max(lastRow, row);
        }
    }
    if (lastRow < firstRow)
        throw std::runtime_error("Empty image resource: " + path.string());
    SpriteLayout layout{static_cast<float>(firstRow) / h, static_cast<float>(lastRow + 1) / h,
                        static_cast<float>(w) / (lastRow - firstRow + 1)};
    return {std::move(art), layout};
}
} // namespace paper
