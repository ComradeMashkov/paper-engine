#include "paper/render/gpu_data.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace paper;
namespace {
void check(bool okay, const char* message) {
    if (!okay)
        throw std::runtime_error(message);
}
template <class F> void rejects(F operation, const char* message) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, message);
}
} // namespace
int main() {
    try {
        Camera camera;
        camera.position = {};
        RenderConfig config;
        RenderStyle style;
        RenderOptions options;
        const auto frame = gpu::frameUniforms(camera, options, config, style);
        const auto near = gpu::project({0, 0, config.nearPlaneMeters}, frame);
        const auto far = gpu::project({0, 0, config.farPlaneMeters}, frame);
        check(std::abs(near.z) < .00001f && std::abs(far.z / far.w - 1) < .00001f,
              "GPU projection maps the clip range to [0,1]");
        for (float aspect : {4.f / 3, 1.6f, 16.f / 9, 21.f / 9}) {
            options.aspect = aspect;
            const auto f = gpu::frameUniforms(camera, options, config, style);
            const auto corner = gpu::project({.4f, .4f, 2}, f);
            check(std::abs(corner.x / corner.y * aspect - 1) < .00001f,
                  "square remains square at each presentation aspect");
            const Vec3 ray = normalized(
                camera.forward() + camera.right() * (corner.x / corner.w / f.cameraPosition.w) +
                camera.up() * (corner.y / corner.w / f.cameraPosition.w / aspect));
            check(length(ray - normalized(Vec3{.4f, .4f, 2})) < .00001f,
                  "projection agrees with the screen picking ray");
        }
        options.aspect = 0;
        rejects([&] { (void)gpu::frameUniforms(camera, options, config, style); },
                "invalid aspect rejected");
        options.aspect = 1.6f;
        std::array<Light, maxSceneLights + 1> tooMany{};
        options.lights = tooMany;
        rejects([&] { (void)gpu::frameUniforms(camera, options, config, style); },
                "light overflow rejected");
        std::array<Light, 2> lights{Light{}, Light{.type = LightType::Spot,
                                                   .position = {2, 3, 4},
                                                   .direction = {0, 2, 0},
                                                   .castsShadow = true}};
        options.lights = lights;
        const auto shadow = gpu::frameUniforms(camera, options, config, style);
        check(shadow.shadowForward.w == 1 && shadow.shadowForward.y == 1,
              "shadow selection keeps the scene index and normalizes vertical directions");
        const Vec3 lightPosition = lights[1].position;
        const auto shadowNear =
            gpu::projectShadow(lightPosition + Vec3{0, options.shadows.nearPlaneMeters, 0}, shadow);
        const auto shadowFar =
            gpu::projectShadow(lightPosition + Vec3{0, lights[1].rangeMeters, 0}, shadow);
        check(std::abs(shadowNear.z) < .00001f && std::abs(shadowFar.z / shadowFar.w - 1) < .00001f,
              "shadow camera uses the light's clip range");
        const float coneEdge = std::tan(lights[1].outerHalfAngleRadians) * 2;
        const Vec3 right{shadow.shadowRight.x, shadow.shadowRight.y, shadow.shadowRight.z};
        const auto edge =
            gpu::projectShadow(lightPosition + Vec3{0, 2, 0} + right * coneEdge, shadow);
        check(std::abs(edge.x / edge.w - 1) < .00001f,
              "shadow coverage matches the outer spotlight cone");
        options.shadows.quality = ShadowQuality::Off;
        check(gpu::frameUniforms(camera, options, config, style).shadowForward.w < 0,
              "disabled shadows do not select a caster");
        options.shadows.quality = ShadowQuality::Medium;
        lights[1].direction = {};
        rejects([&] { (void)gpu::frameUniforms(camera, options, config, style); },
                "zero spot direction rejected");
        lights[1].direction = {0, 0, 1};
        lights[1].innerHalfAngleRadians = lights[1].outerHalfAngleRadians;
        rejects([&] { (void)gpu::frameUniforms(camera, options, config, style); },
                "empty cone falloff rejected");
        lights[1].innerHalfAngleRadians = 0;
        lights[0].castsShadow = true;
        rejects([&] { (void)gpu::frameUniforms(camera, options, config, style); },
                "unsupported point shadows rejected");
        lights[0] = lights[1];
        rejects([&] { (void)gpu::frameUniforms(camera, options, config, style); },
                "shadow budget overflow rejected");
        lights[0].castsShadow = false;
        lights[1].rangeMeters = options.shadows.nearPlaneMeters;
        rejects([&] { (void)gpu::frameUniforms(camera, options, config, style); },
                "empty shadow clip range rejected");
        lights[1].rangeMeters = 10;
        lights[1].intensity = std::numeric_limits<float>::quiet_NaN();
        rejects([&] { (void)gpu::frameUniforms(camera, options, config, style); },
                "non-finite light intensity rejected");
        lights[1].intensity = 1;
        lights[1].color.x = -1;
        rejects([&] { (void)gpu::frameUniforms(camera, options, config, style); },
                "negative light color rejected");
        lights[1].color.x = 1;
        options.shadows.slopeBias = -1;
        rejects([&] { (void)gpu::frameUniforms(camera, options, config, style); },
                "negative shadow bias rejected");
        options.shadows = {};
        options.shadows.quality = static_cast<ShadowQuality>(99);
        rejects([&] { (void)gpu::frameUniforms(camera, options, config, style); },
                "unknown shadow quality rejected");
        options = {};
        Mesh3 mesh;
        quad(mesh, {-1, 1, 0}, {1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, MaterialId{});
        for (auto& triangle : mesh)
            triangle.detail = static_cast<DetailTag>(7);
        const MeshInstance instance{makeMesh(std::move(mesh)), {{0, 0, 4}, {}, 2}};
        const auto hit = pickScene(std::span(&instance, 1), {}, {0, 0, 1});
        check(std::abs(hit.distance - 4) < .00001f && hit.detail == static_cast<DetailTag>(7),
              "scaled instance picking returns world-space distance and detail");
        auto transform = instance.transform;
        transform.scale = 0;
        rejects([&] { (void)gpu::modelUniforms(transform); }, "invalid transform rejected");
        PaintedTexture texture;
        texture.width = texture.height = 1;
        texture.pixels = {{255, 0, 0, 255}};
        MaterialLibrary first({texture}), second({texture});
        const auto revision = first.revision(MaterialId{});
        check(revision != second.revision(MaterialId{}),
              "libraries cannot alias cached material revisions");
        first.setArt(MaterialId{}, texture);
        check(first.revision(MaterialId{}) != revision,
              "resource edits invalidate GPU material cache");
        std::cout << "Render data checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
