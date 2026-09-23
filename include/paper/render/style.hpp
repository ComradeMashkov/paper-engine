#pragma once
#include "paper/core/math3d.hpp"
#include <cstdint>

namespace paper {
struct RenderSize {
    std::uint32_t width, height;
    bool operator==(const RenderSize&) const = default;
};
// Technical limits are independent of the game's painted lighting profile.
struct RenderConfig {
    std::uint32_t width = 640, height = 400;
    float nearPlaneMeters = .075f, farPlaneMeters = 35.f;
};
struct RenderStyle {
    Vec3 fogColor;
    float fogStart = 0, fogDistance = 1, fogMaximum = 1,
          colorStep = 1.f / units::byteChannelMaximum;
    float ambient = 1, normalBias = 0;
    float minimumLight = 0, maximumLight = 1, emissive = 1;
    float faceStart = 0, faceRange = 1, faceFeather = 1;
    float rippleStart = 0, rippleRange = 1, rippleAmplitude = 0, rippleFrequency = 0;
    float pulseFrequency = 0, pulseThreshold = 0, rippleSpatialFrequency = 0;
    float noiseStart = 0, noiseRange = 1, noiseFrequency = 0;
    float noiseAmplitude = 0, noiseBandAmplitude = 0;
};
} // namespace paper
