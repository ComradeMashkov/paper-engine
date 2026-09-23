#pragma once
#include "paper/core/math3d.hpp"
#include "paper/render/shader_constants.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace paper {
inline constexpr size_t maxSceneLights = PAPER_MAX_SCENE_LIGHTS;
inline constexpr size_t maxShadowLights = 1;
enum class LightType { Point, Spot };
struct Light {
    LightType type = LightType::Point;
    Vec3 position;
    Vec3 direction{0, 0, 1}; // Normalized by the renderer; only used by spot lights.
    Vec3 color{1, 1, 1};
    float intensity = 1;
    float rangeMeters = 10;
    float attenuationPerMeterSquared = .1f;
    float innerHalfAngleRadians = units::radians(15);
    float outerHalfAngleRadians = units::radians(30);
    bool castsShadow = false; // One spot light per frame; point shadows need cubemaps.
};
enum class ShadowQuality { Off, Low, Medium, High };
struct ShadowSettings {
    ShadowQuality quality = ShadowQuality::Medium;
    float nearPlaneMeters = .05f;
    // Receiver offsets in projected [0,1] depth, independent of light intensity.
    float depthBias = .00008f;
    float slopeBias = .00025f;
};
struct ShadowQualityInfo {
    std::uint32_t mapSize;
    int filterRadius; // Square PCF kernel: (2 * radius + 1)^2 depth comparisons.
};
[[nodiscard]] ShadowQualityInfo shadowQualityInfo(ShadowQuality quality);
void validateLighting(std::span<const Light> lights, const ShadowSettings& shadows);
} // namespace paper
