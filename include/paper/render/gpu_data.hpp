#pragma once
#include "paper/core/pixel_format.hpp"
#include "paper/render/scene.hpp"
#include "paper/render/style.hpp"

namespace paper::gpu {
// Shader ABI: float4 fields in this order, shared by the HLSL and Metal sources.
inline constexpr float alphaCutoff =
    static_cast<float>(pixelFormat::opaqueAlpha) / pixelFormat::maximumChannel;
inline constexpr size_t uniformAlignmentBytes = 16;
struct alignas(uniformAlignmentBytes) Float4 { // numbers: shader byte layout.
    float x = 0, y = 0, z = 0, w = 0;
};
struct LightUniforms {
    // xyz/w: position/range, direction/cos(outer), color/intensity.
    // parameters: attenuation, cos(inner), spot flag, reserved.
    Float4 positionRange, directionOuter, colorIntensity, parameters;
};
struct FrameUniforms {
    Float4 cameraPosition, cameraRight, cameraUp, cameraForward;
    // lighting: ambient, normal bias, emission, light count; limits: min/max light.
    Float4 fogColor, fog, lighting, limits, effects, face, noise;
    // xyz/w: position/focal, right/near, up/far, forward/caster index (-1 if disabled).
    // params: inverse map size, depth bias, slope bias, PCF radius.
    Float4 shadowPosition, shadowRight, shadowUp, shadowForward, shadowParams;
    std::array<LightUniforms, maxSceneLights> lights;
};
struct ModelUniforms {
    Float4 rotation, positionScale;
};
struct ParticleUniforms {
    // shape: faceted flag, rotation in radians, two reserved components.
    Float4 positionRadius, colorOpacity, shape;
};
struct MaterialUniforms {
    Float4 region, flags;
};
struct Vertex {
    Vec3 position;
    Vec2 uv;
    Vec3 normal;
};
static_assert(sizeof(Float4) == 16);                        // numbers: shader byte layout.
static_assert(sizeof(LightUniforms) == 4 * sizeof(Float4)); // numbers: shader byte layout.
static_assert(sizeof(FrameUniforms) ==
              (16 + 4 * maxSceneLights) * sizeof(Float4)); // numbers: shader byte layout.
static_assert(sizeof(ModelUniforms) == 32 &&
              sizeof(MaterialUniforms) == 32);      // numbers: shader byte layout.
static_assert(sizeof(Vertex) == 8 * sizeof(float)); // numbers: shader byte layout.
[[nodiscard]] FrameUniforms frameUniforms(const Camera& camera, const RenderOptions& options,
                                          const RenderConfig& config, const RenderStyle& style);
[[nodiscard]] ModelUniforms modelUniforms(const MeshTransform& transform);
// Same clip-space convention as the vertex shader; useful to verify picking/projection.
[[nodiscard]] Float4 project(Vec3 position, const FrameUniforms& frame);
[[nodiscard]] Float4 projectShadow(Vec3 position, const FrameUniforms& frame);
} // namespace paper::gpu
