#include "paper/render/lighting.hpp"
#include <stdexcept>

namespace paper {
namespace {
constexpr ShadowQualityInfo lowQuality{512, 0}, mediumQuality{1024, 1}, highQuality{2048, 2};
constexpr float maximumRangeMeters = 1000;
constexpr float maximumIntensity = 64;
constexpr float minimumSpotHalfAngle = units::radians(.5f);
constexpr float maximumShadowBias = .01f;
constexpr float minimumDirectionLength = .00001f;
bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
} // namespace
ShadowQualityInfo shadowQualityInfo(ShadowQuality quality) {
    switch (quality) {
    case ShadowQuality::Off:
        return {0, 0};
    case ShadowQuality::Low:
        return lowQuality;
    case ShadowQuality::Medium:
        return mediumQuality;
    case ShadowQuality::High:
        return highQuality;
    }
    throw std::invalid_argument("Unknown shadow quality");
}
void validateLighting(std::span<const Light> lights, const ShadowSettings& shadows) {
    (void)shadowQualityInfo(shadows.quality);
    if (lights.size() > maxSceneLights || !std::isfinite(shadows.nearPlaneMeters) ||
        shadows.nearPlaneMeters <= 0 || !std::isfinite(shadows.depthBias) ||
        !std::isfinite(shadows.slopeBias) || shadows.depthBias < 0 || shadows.slopeBias < 0 ||
        shadows.depthBias > maximumShadowBias || shadows.slopeBias > maximumShadowBias)
        throw std::invalid_argument("Invalid light count or shadow settings");
    size_t shadowCount = 0;
    for (const auto& light : lights) {
        if (!finite(light.position) || !finite(light.direction) || !finite(light.color) ||
            light.color.x < 0 || light.color.y < 0 || light.color.z < 0 || light.color.x > 1 ||
            light.color.y > 1 || light.color.z > 1 || !std::isfinite(light.intensity) ||
            light.intensity < 0 || light.intensity > maximumIntensity ||
            !std::isfinite(light.rangeMeters) || light.rangeMeters <= 0 ||
            light.rangeMeters > maximumRangeMeters ||
            !std::isfinite(light.attenuationPerMeterSquared) ||
            light.attenuationPerMeterSquared < 0)
            throw std::invalid_argument("Invalid light color, intensity, position or range");
        switch (light.type) {
        case LightType::Point:
            if (light.castsShadow)
                throw std::invalid_argument("Point-light shadows are not supported");
            break;
        case LightType::Spot:
            if (!std::isfinite(length(light.direction)) ||
                length(light.direction) <= minimumDirectionLength ||
                !std::isfinite(light.innerHalfAngleRadians) ||
                !std::isfinite(light.outerHalfAngleRadians) || light.innerHalfAngleRadians < 0 ||
                light.innerHalfAngleRadians >= light.outerHalfAngleRadians ||
                light.outerHalfAngleRadians < minimumSpotHalfAngle ||
                light.outerHalfAngleRadians >=
                    pi3 * .5f) // numbers: a spotlight half-angle must be less than a right angle.
                throw std::invalid_argument("Invalid spot direction or cone angles");
            break;
        default:
            throw std::invalid_argument("Unknown light type");
        }
        if (light.castsShadow) {
            if (++shadowCount > maxShadowLights)
                throw std::invalid_argument("Shadow light budget exceeded");
            if (light.rangeMeters <= shadows.nearPlaneMeters)
                throw std::invalid_argument("Shadow light range must exceed its near plane");
        }
    }
}
} // namespace paper
