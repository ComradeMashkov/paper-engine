#pragma once
#include <cstdint>

namespace paper::pixelFormat {
// RGBA8 byte layout and common CPU/GPU cutout threshold.
inline constexpr int channels = 4, red = 0, green = 1, blue = 2, alpha = 3;
inline constexpr std::uint8_t opaqueAlpha = 128, maximumChannel = 255;
} // namespace paper::pixelFormat
