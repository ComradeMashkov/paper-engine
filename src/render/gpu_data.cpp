#include "paper/render/gpu_data.hpp"
#include <cmath>
#include <stdexcept>

namespace paper::gpu {
namespace {
constexpr float grainTickPeriod = 65536; // Keeps the integer noise phase exactly representable.
Float4 packed(Vec3 value, float w) {
    return {value.x, value.y, value.z, w};
}
bool finite(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
float progress(float value, float start, float range) {
    return std::clamp((value - start) / range, 0.f, 1.f);
}
} // namespace
FrameUniforms frameUniforms(const Camera& camera, const RenderOptions& options,
                            const RenderConfig& config, const RenderStyle& style) {
    if (!finite(camera.position) || !std::isfinite(camera.yaw) || !std::isfinite(camera.pitch) ||
        !std::isfinite(camera.fov) || camera.fov <= 0 || camera.fov >= pi3 ||
        !std::isfinite(options.aspect) || options.aspect <= 0 ||
        !std::isfinite(options.elapsedSeconds) || !std::isfinite(options.distortion) ||
        !std::isfinite(options.emissiveScale) || options.emissiveScale < 0 ||
        config.nearPlaneMeters <= 0 || !std::isfinite(config.nearPlaneMeters) ||
        !std::isfinite(config.farPlaneMeters) || config.farPlaneMeters <= config.nearPlaneMeters ||
        !config.width || !config.height)
        throw std::invalid_argument("Invalid camera, viewport, clip range or emission scale");
    const auto values = std::array{style.fogStart,
                                   style.fogDistance,
                                   style.fogMaximum,
                                   style.colorStep,
                                   style.ambient,
                                   style.normalBias,
                                   style.minimumLight,
                                   style.maximumLight,
                                   style.emissive,
                                   style.faceStart,
                                   style.faceRange,
                                   style.faceFeather,
                                   style.rippleStart,
                                   style.rippleRange,
                                   style.rippleAmplitude,
                                   style.rippleFrequency,
                                   style.pulseFrequency,
                                   style.pulseThreshold,
                                   style.rippleSpatialFrequency,
                                   style.noiseStart,
                                   style.noiseRange,
                                   style.noiseFrequency,
                                   style.noiseAmplitude,
                                   style.noiseBandAmplitude};
    if (!std::ranges::all_of(values, [](float value) { return std::isfinite(value); }) ||
        !finite(style.fogColor) || style.fogDistance <= 0 || style.colorStep <= 0 ||
        style.faceRange <= 0 || style.rippleRange <= 0 || style.noiseRange <= 0 ||
        style.pulseThreshold < 0 || style.pulseThreshold >= 1 || style.minimumLight < 0 ||
        style.minimumLight > style.maximumLight || style.ambient < 0 || style.emissive < 0 ||
        style.normalBias < 0 || style.normalBias > 1)
        throw std::invalid_argument("Invalid render style");
    validateLighting(options.lights, options.shadows);
    FrameUniforms result{};
    result.cameraPosition =
        packed(camera.position,
               1 / std::tan(camera.fov *
                            .5f)); // numbers: perspective projection uses half the field of view.
    result.cameraRight = packed(camera.right(), options.aspect);
    result.cameraUp = packed(camera.up(), config.nearPlaneMeters);
    result.cameraForward = packed(camera.forward(), config.farPlaneMeters);
    result.fogColor = packed(style.fogColor, alphaCutoff);
    result.fog = {style.fogStart, style.fogDistance, style.fogMaximum, style.colorStep};
    const float time = options.elapsedSeconds;
    result.lighting = {style.ambient, style.normalBias, style.emissive * options.emissiveScale,
                       static_cast<float>(options.lights.size())};
    result.limits = {style.minimumLight, style.maximumLight, 0, 0};
    result.shadowForward.w = -1; // No shadow caster unless selected below.
    const float distortion = std::clamp(options.distortion, 0.f, 1.f);
    const float blend = progress(distortion, style.faceStart, style.faceRange);
    const float pulse =
        std::max(0.f, std::sin(time * style.pulseFrequency) - style.pulseThreshold) /
        (1 - style.pulseThreshold);
    result.effects = {
        blend * blend * (3 - 2 * blend), // numbers: smoothstep polynomial t squared times (3 - 2t).
        progress(distortion, style.rippleStart, style.rippleRange) * pulse *
            std::sin(time * style.rippleFrequency) * style.rippleAmplitude,
        progress(distortion, style.noiseStart, style.noiseRange),
        std::floor(std::fmod(std::max(0.f, time) * style.noiseFrequency, grainTickPeriod))};
    result.face = {style.faceFeather, style.rippleSpatialFrequency, 0, 0};
    result.noise = {style.noiseAmplitude, style.noiseBandAmplitude, 0, 0};
    const auto quality = shadowQualityInfo(options.shadows.quality);
    for (size_t i = 0; i < options.lights.size(); ++i) {
        const auto& light = options.lights[i];
        const bool spot = light.type == LightType::Spot;
        const auto direction = spot ? normalized(light.direction) : Vec3{0, 0, 1};
        result.lights[i] = {packed(light.position, light.rangeMeters),
                            packed(direction, spot ? std::cos(light.outerHalfAngleRadians) : 0),
                            packed(light.color, light.intensity),
                            {light.attenuationPerMeterSquared,
                             spot ? std::cos(light.innerHalfAngleRadians) : 1, spot ? 1.f : 0.f,
                             0}};
        if (light.castsShadow && light.intensity > 0 && quality.mapSize) {
            // Choose a reference axis away from the pole; works for vertical spotlights too.
            constexpr float poleThreshold = .99f;
            const Vec3 reference =
                std::abs(direction.y) > poleThreshold ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
            const Vec3 right = normalized(cross(reference, direction));
            const Vec3 up = cross(direction, right);
            result.shadowPosition =
                packed(light.position, 1 / std::tan(light.outerHalfAngleRadians));
            result.shadowRight = packed(right, options.shadows.nearPlaneMeters);
            result.shadowUp = packed(up, light.rangeMeters);
            result.shadowForward = packed(direction, static_cast<float>(i));
            result.shadowParams = {1.f / quality.mapSize, options.shadows.depthBias,
                                   options.shadows.slopeBias,
                                   static_cast<float>(quality.filterRadius)};
        }
    }
    return result;
}
ModelUniforms modelUniforms(const MeshTransform& t) {
    if (!finite(t.position) || !std::isfinite(t.scale) || t.scale <= 0 ||
        !std::isfinite(t.rotation.w) || !std::isfinite(t.rotation.x) ||
        !std::isfinite(t.rotation.y) || !std::isfinite(t.rotation.z))
        throw std::invalid_argument("Invalid mesh transform");
    const auto r = t.rotation.unit();
    return {{r.x, r.y, r.z, r.w}, packed(t.position, t.scale)};
}
Float4 project(Vec3 position, const FrameUniforms& f) {
    const auto xyz = [](Float4 value) { return Vec3{value.x, value.y, value.z}; };
    const Vec3 relative = position - xyz(f.cameraPosition);
    const float depth = dot(relative, xyz(f.cameraForward));
    const float nearPlaneMeters = f.cameraUp.w, farPlaneMeters = f.cameraForward.w;
    return {dot(relative, xyz(f.cameraRight)) * f.cameraPosition.w,
            dot(relative, xyz(f.cameraUp)) * f.cameraPosition.w * f.cameraRight.w,
            (depth - nearPlaneMeters) * farPlaneMeters / (farPlaneMeters - nearPlaneMeters), depth};
}
Float4 projectShadow(Vec3 position, const FrameUniforms& frame) {
    FrameUniforms projection{};
    projection.cameraPosition = frame.shadowPosition;
    projection.cameraRight = frame.shadowRight;
    projection.cameraRight.w = 1;
    projection.cameraUp = frame.shadowUp;
    projection.cameraUp.w = frame.shadowRight.w;
    projection.cameraForward = frame.shadowForward;
    projection.cameraForward.w = frame.shadowUp.w;
    return project(position, projection);
}
} // namespace paper::gpu
