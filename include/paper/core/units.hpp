#pragma once
#include "paper/core/pixel_format.hpp"
#include <cstdint>
#include <numbers>

namespace paper::units {
inline constexpr float degreesPerHalfTurn = 180;
inline constexpr float percentPerWhole = 100;
inline constexpr float byteChannelMaximum = pixelFormat::maximumChannel;
inline constexpr double millisecondsPerSecond = 1000;
inline constexpr std::uint64_t nanosecondsPerSecond = 1'000'000'000;
inline constexpr double nanosecondsPerMillisecond = nanosecondsPerSecond / millisecondsPerSecond;
[[nodiscard]] constexpr float radians(float degrees) {
    return degrees * std::numbers::pi_v<float> / degreesPerHalfTurn;
}
[[nodiscard]] constexpr float ratio(float percent) {
    return percent / percentPerWhole;
}
} // namespace paper::units
