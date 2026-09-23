#pragma once
#include "paper/core/pixel_format.hpp"
#include <cstdint>
namespace paper {
struct Color {
    std::uint8_t r, g, b, a = pixelFormat::maximumChannel;
};
inline constexpr Color untinted{255, 255, 255, 255};
struct Rect {
    float x, y, w, h;
    [[nodiscard]] constexpr bool contains(float px, float py) const noexcept {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
};
} // namespace paper
