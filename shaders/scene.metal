#include "../include/paper/render/shader_constants.h"
// Native Metal, with the same resource ABI and algorithms as scene.hlsl.
#include <metal_stdlib>
using namespace metal;
struct LightData { float4 positionRange; float4 directionOuter; float4 colorIntensity; float4 parameters; };
struct Frame {
    float4 cameraPosition;
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    float4 fogColor;
    float4 fog;
    float4 lighting;
    float4 limits;
    float4 effects;
    float4 face;
    float4 noise;
    float4 shadowPosition;
    float4 shadowRight;
    float4 shadowUp;
    float4 shadowForward;
    float4 shadowParams;
    LightData lights[PAPER_MAX_SCENE_LIGHTS]; // ABI: maxSceneLights
};
struct Model { float4 rotation; float4 positionScale; };
struct Material { float4 region; float4 flags; };
float3 rotateVector(float3 value, float4 rotation) {
    float3 twice = 2 * cross(rotation.xyz, value);
    return value + rotation.w * twice + cross(rotation.xyz, twice);
}
float3 unitVector(float3 value) {
    const float minimumSquaredLength = PAPER_MINIMUM_SQUARED_DIRECTION;
    float squared = dot(value, value);
    return squared > minimumSquaredLength ? value / sqrt(squared) : float3(0, 0, 0);
}
float4 projectPosition(float3 relative, float3 right, float3 up, float3 forward,
    float focal, float aspect, float nearPlane, float farPlane) {
    float depth = dot(relative, forward);
    return float4(dot(relative, right) * focal, dot(relative, up) * focal * aspect,
        (depth - nearPlane) * farPlane / (farPlane - nearPlane), depth);
}

struct WorldInput {
    float3 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
    float3 normal [[attribute(2)]]; // numbers: map normalized clip coordinates to the centered texture range.
};
struct WorldOutput {
    float4 position [[position]];
    float2 uv;
    float3 worldPosition;
    float3 normal;
};
struct ShadowOutput { float4 position [[position]]; float2 uv; float depth; };
struct ScreenOutput { float4 position [[position]]; float2 uv; };
// Icosahedron: 12 vertices, 20 outward-facing triangles; no billboard texture.
float3 smokeVertex(uint id) {
    const float phi = 1.61803398875; // numbers: the golden ratio defines an icosahedron.
    const float3 vertices[12] = {float3(-1,phi,0),float3(1,phi,0),float3(-1,-phi,0),float3(1,-phi,0),
        float3(0,-1,phi),float3(0,1,phi),float3(0,-1,-phi),float3(0,1,-phi),
        float3(phi,0,-1),float3(phi,0,1),float3(-phi,0,-1),float3(-phi,0,1)};
    const uint3 faces[PAPER_SMOKE_FACE_COUNT] = {uint3(0,11,5),uint3(0,5,1),uint3(0,1,7),uint3(0,7,10),uint3(0,10,11),
        uint3(1,5,9),uint3(5,11,4),uint3(11,10,2),uint3(10,7,6),uint3(7,1,8),
        uint3(3,9,4),uint3(3,4,2),uint3(3,2,6),uint3(3,6,8),uint3(3,8,9),
        uint3(4,9,5),uint3(2,4,11),uint3(6,2,10),uint3(8,6,7),uint3(9,8,1)};
    return normalize(vertices[faces[id / 3][id % 3]]); // numbers: triangle corner indexing.
}
struct ParticleData { float4 positionRadius; float4 colorOpacity; float4 shape; };
struct ParticleOutput { float4 position [[position]]; float2 uv; float4 color; };
vertex ParticleOutput particle_vertex(uint id [[vertex_id]], constant Frame& frame [[buffer(0)]],
    constant ParticleData& particle [[buffer(1)]]) {
    const float2 corners[6] = {float2(-1,1),float2(1,1),float2(1,-1),float2(-1,1),float2(1,-1),float2(-1,-1)};
    float2 corner = corners[id % 6]; // numbers: six corners in the soft quad.
    float3 local = float3(corner, 0);
    float shade = 1, facing = 1;
    if (particle.shape.x > 0) {
        const float3 axis = normalize(float3(1,2,3)); // numbers: oblique spin axis avoids aligned facets.
        float halfAngle = particle.shape.y * 0.5; // numbers: quaternion half angle.
        float4 spin = float4(axis * sin(halfAngle), cos(halfAngle));
        uint face = (id / 3) * 3; // numbers: first triangle corner.
        float3 a = rotateVector(smokeVertex(face), spin);
        float3 b = rotateVector(smokeVertex(face + 1), spin);
        float3 c = rotateVector(smokeVertex(face + 2), spin); // numbers: third triangle corner.
        float3 normal = normalize(cross(b-a, c-a));
        local = rotateVector(smokeVertex(id), spin);
        const float ambient = 0.72, diffuse = 0.28;
        const float3 lightDirection = float3(-0.3,0.6,-0.74);
        shade = ambient + diffuse * saturate(dot(normal, normalize(lightDirection)));
        float3 relative = particle.positionRadius.xyz - frame.cameraPosition.xyz;
        float3 viewCenter = float3(dot(relative,frame.cameraRight.xyz),dot(relative,frame.cameraUp.xyz),dot(relative,frame.cameraForward.xyz));
        facing = dot(normal, viewCenter + a * particle.positionRadius.w) < 0 ? 1 : 0;
        corner = float2(0,0); // Flat face opacity, without the soft radial mask.
    }
    float3 position = particle.positionRadius.xyz + particle.positionRadius.w *
        (frame.cameraRight.xyz * local.x + frame.cameraUp.xyz * local.y + frame.cameraForward.xyz * local.z);
    ParticleOutput output;
    output.position = projectPosition(position - frame.cameraPosition.xyz,
        frame.cameraRight.xyz, frame.cameraUp.xyz, frame.cameraForward.xyz,
        frame.cameraPosition.w, frame.cameraRight.w, frame.cameraUp.w, frame.cameraForward.w);
    output.uv = corner;
    float depth = dot(particle.positionRadius.xyz - frame.cameraPosition.xyz, frame.cameraForward.xyz);
    float fog = clamp((depth - frame.fog.x) / frame.fog.y, 0.0, frame.fog.z);
    output.color = float4(mix(particle.colorOpacity.rgb, frame.fogColor.rgb, fog) * shade, particle.colorOpacity.a * facing);
    return output;
}
fragment float4 particle_fragment(ParticleOutput input [[stage_in]]) {
    float edge = saturate(1 - dot(input.uv, input.uv));
    return float4(input.color.rgb, input.color.a * edge * edge);
}
vertex WorldOutput world_vertex(WorldInput input [[stage_in]],
    constant Frame& frame [[buffer(0)]], constant Model& model [[buffer(1)]]) {
    WorldOutput output;
    output.worldPosition = model.positionScale.xyz +
        rotateVector(input.position * model.positionScale.w, model.rotation);
    output.normal = rotateVector(input.normal, model.rotation);
    output.position = projectPosition(output.worldPosition - frame.cameraPosition.xyz,
        frame.cameraRight.xyz, frame.cameraUp.xyz, frame.cameraForward.xyz,
        frame.cameraPosition.w, frame.cameraRight.w, frame.cameraUp.w, frame.cameraForward.w);
    output.uv = input.uv;
    return output;
}
vertex ShadowOutput shadow_vertex(WorldInput input [[stage_in]],
    constant Frame& frame [[buffer(0)]], constant Model& model [[buffer(1)]]) {
    ShadowOutput output;
    float3 position = model.positionScale.xyz +
        rotateVector(input.position * model.positionScale.w, model.rotation);
    output.position = projectPosition(position - frame.shadowPosition.xyz,
        frame.shadowRight.xyz, frame.shadowUp.xyz, frame.shadowForward.xyz,
        frame.shadowPosition.w, 1, frame.shadowRight.w, frame.shadowUp.w);
    output.uv = input.uv;
    output.depth = dot(position - frame.shadowPosition.xyz, frame.shadowForward.xyz);
    return output;
}
fragment float4 world_fragment(WorldOutput input [[stage_in]],
    constant Frame& frame [[buffer(0)]], constant Material& material [[buffer(1)]],
    texture2d<float> baseTexture [[texture(0)]], texture2d<float> alternateTexture [[texture(1)]],
    depth2d<float> shadowMap [[texture(2)]], sampler baseSampler [[sampler(0)]], // numbers: Metal buffer/texture binding slot matches the GPU pipeline ABI.
    sampler alternateSampler [[sampler(1)]], sampler shadowSampler [[sampler(2)]]) { // numbers: map normalized clip coordinates to the centered texture range.
    float depth = dot(input.worldPosition - frame.cameraPosition.xyz, frame.cameraForward.xyz);
    if (depth < frame.cameraUp.w || depth > frame.cameraForward.w) discard_fragment();
    float2 uv = fract(input.uv);
    float4 color = baseTexture.sample(baseSampler, uv);
    if (color.a < frame.fogColor.w) discard_fragment();
    if (material.flags.y > 0 && frame.effects.x > 0) {
        float2 delta = (uv - material.region.xy) / material.region.zw;
        float mask = saturate((1 - dot(delta, delta)) * frame.face.x);
        float2 shifted = float2(uv.x + sin(delta.y * frame.face.y) * frame.effects.y * mask, uv.y);
        float4 other = alternateTexture.sample(alternateSampler, saturate(shifted));
        if (other.a >= frame.fogColor.w)
            color.rgb = mix(color.rgb, other.rgb, mask * frame.effects.x);
    }
    float3 normal = unitVector(input.normal);
    float3 illumination = float3(frame.lighting.x, frame.lighting.x, frame.lighting.x);
    for (uint i = 0; i < uint(frame.lighting.w); ++i) {
        LightData light = frame.lights[i];
        float3 delta = light.positionRange.xyz - input.worldPosition;
        float distanceSquared = dot(delta, delta);
        float3 toLight = unitVector(delta);
        float fade = saturate(1 - distanceSquared / (light.positionRange.w * light.positionRange.w));
        fade *= fade;
        float cone = light.parameters.z > 0 ? smoothstep(light.directionOuter.w,
            light.parameters.y, dot(light.directionOuter.xyz, -toLight)) : 1;
        // Two-sided diffuse and a small normal bias retain the painted style.
        float diffuse = frame.lighting.y + (1 - frame.lighting.y) * abs(dot(normal, toLight));
        float strength = light.colorIntensity.w * diffuse * fade * cone /
            (1 + light.parameters.x * distanceSquared);
        float visibility = 1;
        if (int(i) == int(frame.shadowForward.w) && strength > 0) {
            float3 relative = input.worldPosition - frame.shadowPosition.xyz;
            float4 clip = projectPosition(relative, frame.shadowRight.xyz, frame.shadowUp.xyz,
                frame.shadowForward.xyz, frame.shadowPosition.w, 1, frame.shadowRight.w, frame.shadowUp.w);
            if (clip.w > frame.shadowRight.w && clip.w < frame.shadowUp.w) {
                float3 projected = clip.xyz / clip.w;
                float2 shadowUV = float2(projected.x * 0.5 + 0.5, 0.5 - projected.y * 0.5);
                if (all(shadowUV >= 0) && all(shadowUV <= 1)) {
                    float bias = frame.shadowParams.y + frame.shadowParams.z *
                        (1 - abs(dot(normal, toLight)));
                    int radius = int(frame.shadowParams.w);
                    visibility = 0;
                    for (int y = -radius; y <= radius; ++y)
                        for (int x = -radius; x <= radius; ++x) {
                            float2 tap = shadowUV + float2(x, y) * frame.shadowParams.x;
                            visibility += shadowMap.sample_compare(shadowSampler, tap, projected.z - bias);
                        }
                    int diameter = 2 * radius + 1;
                    visibility /= float(diameter * diameter);
                }
            }
        }
        illumination += light.colorIntensity.rgb * strength * visibility;
    }
    if (material.flags.x > 0)
        illumination = float3(frame.lighting.z, frame.lighting.z, frame.lighting.z);
    illumination = clamp(illumination, frame.limits.x, frame.limits.y);
    float fog = clamp((depth - frame.fog.x) / frame.fog.y, 0.0, frame.fog.z);
    float3 shaded = color.rgb * illumination * (1 - fog) + frame.fogColor.rgb * fog;
    shaded = floor(shaded / frame.fog.w + 0.5) * frame.fog.w; // numbers: map normalized clip coordinates to the centered texture range.
    return float4(saturate(shaded), 1);
}
fragment void shadow_fragment(ShadowOutput input [[stage_in]],
    constant Frame& frame [[buffer(0)]], texture2d<float> baseTexture [[texture(0)]],
    sampler baseSampler [[sampler(0)]]) {
    if (input.depth < frame.shadowRight.w || input.depth > frame.shadowUp.w) discard_fragment();
    // Only the original alpha defines the silhouette, exactly as in the color pass.
    if (baseTexture.sample(baseSampler, fract(input.uv)).a < frame.fogColor.w) discard_fragment();
}
vertex ScreenOutput screen_vertex(uint id [[vertex_id]]) {
    ScreenOutput output;
    float2 corner = float2(id == 2 ? 3 : -1, id == 1 ? 3 : -1);
    output.position = float4(corner, 0, 1);
    output.uv = float2((corner.x + 1) * 0.5, (1 - corner.y) * 0.5); // numbers: map normalized clip coordinates to the centered texture range.
    return output;
}
fragment float4 screen_fragment(ScreenOutput input [[stage_in]],
    constant Frame& frame [[buffer(0)]], texture2d<float> sceneTexture [[texture(0)]],
    sampler sceneSampler [[sampler(0)]]) {
    float3 color = sceneTexture.sample(sceneSampler, input.uv).rgb;
    if (frame.effects.z > 0) {
        uint2 pixel = uint2(input.position.xy);
        uint tick = uint(frame.effects.w);
        // Integer hash constants specify the repeatable noise pattern, not light tuning.
        const uint hashX = PAPER_NOISE_HASH_X, hashY = PAPER_NOISE_HASH_Y, hashMix = PAPER_NOISE_HASH_MIX;
        const uint bandPeriod = PAPER_NOISE_BAND_PERIOD, bandWidth = PAPER_NOISE_BAND_WIDTH;
        uint noise = pixel.x * hashX + pixel.y * hashY + tick * hashMix;
        noise = (noise ^ (noise >> PAPER_NOISE_HASH_SHIFT)) * hashMix;
        float grain = (float(noise % PAPER_NOISE_GRAIN_LEVELS) / PAPER_NOISE_GRAIN_HALF_RANGE - 1) * frame.noise.x;
        float band = ((pixel.y * PAPER_NOISE_BAND_Y + tick * PAPER_NOISE_BAND_TICK) % bandPeriod) < bandWidth ? frame.noise.y : 0;
        color += (grain + band) * frame.effects.z;
    }
    return float4(saturate(color), 1);
}
