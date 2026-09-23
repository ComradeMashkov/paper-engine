#pragma once
#include "paper/render/scene.hpp"
#include <filesystem>
namespace paper {
struct LoadedSprite {
    PaintedTexture texture;
    SpriteLayout layout;
};
[[nodiscard]] LoadedSprite loadSprite(const std::filesystem::path& path);
} // namespace paper
