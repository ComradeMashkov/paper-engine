#include "paper/render/gpu_renderer.hpp"
#include "paper/sdl/resources.hpp"
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace paper;
namespace {
void check(bool okay, const char* message) {
    if (!okay)
        throw std::runtime_error(message);
}
PaintedTexture solid(Pixel color) {
    PaintedTexture texture;
    texture.width = texture.height = 1;
    texture.pixels = {color};
    return texture;
}
MeshHandle plane(float depth, float half, MaterialId material) {
    Mesh3 mesh;
    quad(mesh, {-half, half, depth}, {half, half, depth}, {half, -half, depth},
         {-half, -half, depth}, material);
    return makeMesh(std::move(mesh));
}
} // namespace
int main(int, char**) {
    try {
        sdl::Session session(true);
#if defined(__APPLE__)
        constexpr auto formats = SDL_GPU_SHADERFORMAT_MSL;
#elif defined(_WIN32)
        constexpr auto formats = SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV;
#else
        constexpr auto formats = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
        sdl::GPUDevice device(SDL_CreateGPUDevice(formats, true, nullptr));
        sdl::check(bool(device), "Create GPU test device");
        sdl::Renderer presentation(SDL_CreateGPURenderer(device.get(), nullptr));
        sdl::check(bool(presentation), "Create offscreen GPU test renderer");
        constexpr RenderConfig config{160, 100};
        sdl::Texture target(SDL_CreateTexture(presentation.get(), SDL_PIXELFORMAT_RGBA32,
                                              SDL_TEXTUREACCESS_TARGET, config.width,
                                              config.height));
        sdl::check(bool(target) && SDL_SetRenderTarget(presentation.get(), target.get()),
                   "Create GPU test target");
        GpuRenderer renderer(*device, *presentation, PAPER_TEST_SHADER_DIR, config);
        MaterialLibrary materials(
            {solid({255, 0, 0, 255}), solid({0, 255, 0, 255}), solid({255, 255, 255, 0})});
        constexpr auto red = static_cast<MaterialId>(0), green = static_cast<MaterialId>(1),
                       transparent = static_cast<MaterialId>(2);
        RenderStyle style;
        style.fogMaximum = 0;
        Camera camera;
        camera.position = {};
        RenderOptions options;
        const auto draw = [&](RenderScene scene) {
            renderer.render(scene, materials, camera, options, style);
            sdl::check(SDL_RenderTexture(presentation.get(), renderer.image(), nullptr, nullptr),
                       "Compose GPU test image");
            sdl::Surface read(SDL_RenderReadPixels(presentation.get(), nullptr));
            sdl::check(bool(read), "Read GPU test image");
            sdl::Surface rgba(SDL_ConvertSurface(read.get(), SDL_PIXELFORMAT_RGBA32));
            sdl::check(bool(rgba), "Convert GPU test image");
            std::vector<Pixel> pixels(config.width * config.height);
            for (Uint32 y = 0; y < config.height; ++y)
                std::memcpy(pixels.data() + y * config.width,
                            static_cast<const std::byte*>(rgba->pixels) + y * rgba->pitch,
                            config.width * sizeof(Pixel));
            sdl::check(SDL_RenderPresent(presentation.get()), "Complete GPU test frame");
            return pixels;
        };
        const auto center = [&](const std::vector<Pixel>& image) {
            return image[config.height / 2 * config.width + config.width / 2];
        };
        const auto near = plane(2, 1, red), far = plane(4, 2, green);
        std::array<MeshInstance, 2> instances{{{near, {}}, {far, {}}}};
        const auto first = center(draw(instances));
        check(first.r > 240 && first.g < 10, "foreground wins depth test");
        std::swap(instances[0], instances[1]);
        const auto second = center(draw(instances));
        check(first.r == second.r && first.g == second.g,
              "depth does not depend on submission order");
        check(renderer.stats().meshUploads == 0 && renderer.stats().textureUploads == 0,
              "unchanged geometry and textures are not reuploaded");
        auto* originalImage = renderer.image();
        renderer.resize({config.width, config.height});
        check(renderer.image() == originalImage, "the active resolution does not recreate targets");
        bool rejected = false;
        try {
            renderer.resize({0, config.height});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected && renderer.image() == originalImage &&
                  renderer.size() == RenderSize{config.width, config.height},
              "invalid resolution leaves the current render targets intact");
        for (const RenderSize size :
             {RenderSize{80, 50}, RenderSize{320, 200}, RenderSize{config.width, config.height}}) {
            // Exercise changing a setting after the background has been queued in SDL.
            sdl::check(SDL_RenderTexture(presentation.get(), renderer.image(), nullptr, nullptr),
                       "Queue the old scene before resizing");
            renderer.resize(size);
            sdl::Surface queued(SDL_RenderReadPixels(presentation.get(), nullptr));
            sdl::check(bool(queued), "Old scene remains composable during resize");
            sdl::Surface rgba(SDL_ConvertSurface(queued.get(), SDL_PIXELFORMAT_RGBA32));
            sdl::check(bool(rgba), "Convert queued frame after resize");
            Pixel queuedCenter;
            std::memcpy(&queuedCenter,
                        static_cast<const std::byte*>(rgba->pixels) +
                            config.height / 2 * rgba->pitch + config.width / 2 * sizeof(Pixel),
                        sizeof(Pixel));
            check(queuedCenter.r > 240 && queuedCenter.g < 10,
                  "resizing preserves SDL draws that reference the old target");
            sdl::check(SDL_RenderPresent(presentation.get()),
                       "Complete the old frame after resize");
            float imageWidth = 0, imageHeight = 0;
            sdl::check(SDL_GetTextureSize(renderer.image(), &imageWidth, &imageHeight),
                       "Query resized world texture");
            check(renderer.size() == size && imageWidth == size.width && imageHeight == size.height,
                  "both GPU targets and the SDL wrapper use the selected size");
            check(center(draw(instances)).r > 240 && renderer.stats().meshUploads == 0 &&
                      renderer.stats().textureUploads == 0,
                  "resizing preserves depth and cached meshes and materials");
        }
        instances[1].transform.rotation = Rotation3::axisAngle({0, 0, 1}, .2f);
        (void)draw(instances);
        check(renderer.stats().meshUploads == 0, "object rotation reuses the vertex buffer");
        const std::array<MeshInstance, 2> cutout{{{plane(2, 1, transparent), {}}, {far, {}}}};
        const auto through = center(draw(cutout));
        check(through.g > 240 && through.r < 10,
              "transparent pixels do not hide geometry or write depth");
        Mesh3 crossing;
        quad(crossing, {-.1f, .1f, -.1f}, {.1f, .1f, .3f}, {.1f, -.1f, .3f}, {-.1f, -.1f, -.1f},
             red);
        const MeshInstance clipped{makeMesh(std::move(crossing)), {}};
        check(std::ranges::any_of(draw(std::span(&clipped, 1)), [](Pixel p) { return p.r > 240; }),
              "GPU clips a triangle that crosses the near plane");
        const MeshInstance square{plane(2, .4f, red), {}};
        for (float aspect : {4.f / 3, 1.6f, 16.f / 9, 21.f / 9}) {
            options.aspect = aspect;
            const auto pixels = draw(std::span(&square, 1));
            int left = config.width, right = -1, top = config.height, bottom = -1;
            for (Uint32 y = 0; y < config.height; ++y)
                for (Uint32 x = 0; x < config.width; ++x)
                    if (pixels[y * config.width + x].r > 240) {
                        left = std::min(left, static_cast<int>(x));
                        right = std::max(right, static_cast<int>(x));
                        top = std::min(top, static_cast<int>(y));
                        bottom = std::max(bottom, static_cast<int>(y));
                    }
            check(right >= left && bottom >= top, "aspect probe visible");
            const float ratio = (right - left + 1.f) / (bottom - top + 1.f) * aspect /
                                (static_cast<float>(config.width) / config.height);
            check(std::abs(ratio - 1) < .1f, "GPU preserves display aspect");
        }
        options.aspect = 1.6f;
        Mesh3 box;
        boxMesh(box, {{0, 0, 0}, {.5f, .5f, .5f}}, red);
        const MeshInstance cube{makeMesh(std::move(box)), {}};
        for (Vec3 position : {Vec3{0, 0, -2}, Vec3{0, 0, 2}, Vec3{-2, 0, 0}, Vec3{2, 0, 0},
                              Vec3{0, 2, 0}, Vec3{0, -2, 0}}) {
            camera.position = position;
            const Vec3 direction = position * -1;
            camera.yaw = std::atan2(direction.x, direction.z);
            camera.pitch = std::atan2(
                direction.y, std::sqrt(direction.x * direction.x + direction.z * direction.z));
            check(center(draw(std::span(&cube, 1))).r > 240,
                  "GPU culling keeps each outward box face");
        }
        camera = Camera{};
        camera.position = {};
        const MeshInstance behind{plane(-2, 1, red), {}};
        check(std::ranges::all_of(draw(std::span(&behind, 1)),
                                  [](Pixel p) { return p.r == 0 && p.g == 0; }),
              "geometry behind the camera is clipped");
        const MeshInstance distant{plane(config.farPlaneMeters + 1, 20, red), {}};
        check(center(draw(std::span(&distant, 1))).r == 0,
              "geometry beyond the far plane is clipped");
        check(center(draw(std::span(&cube, 1))).r == 0,
              "back faces of a closed mesh are culled when viewed from inside");
        auto reverse = *near;
        for (auto& triangle : reverse)
            std::swap(triangle.v[1], triangle.v[2]);
        const MeshInstance reversed{makeMesh(std::move(reverse)), {}};
        check(center(draw(std::span(&reversed, 1))).r > 240,
              "two-sided artwork remains visible from the reverse side");
        materials.setArt(red, solid({0, 0, 255, 255}));
        check(center(draw(std::span(&square, 1))).b > 240 && renderer.stats().textureUploads == 1,
              "material replacement uploads and displays the new resource");
        PaintedTexture corners;
        corners.width = corners.height = 2;
        corners.pixels = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};
        materials.setArt(red, corners);
        const MeshInstance artwork{near, {}};
        const auto image = draw(std::span(&artwork, 1));
        const auto at = [&](Uint32 x, Uint32 y) { return image[y * config.width + x]; };
        check(at(60, 30).r > 240 && at(60, 30).g < 10 && at(100, 30).g > 240 &&
                  at(100, 30).r < 10 && at(60, 70).b > 240 && at(100, 70).r > 240 &&
                  at(100, 70).g > 240,
              "texture corners retain their UV orientation through both GPU passes");
        auto face = solid({255, 0, 0, 255});
        face.alteredFace = {{0, 0, 255, 255}};
        face.faceCenter = {.5f, .5f};
        face.faceRadius = {.3f, .3f};
        materials.setArt(red, face);
        options.distortion = 1;
        const auto altered = draw(std::span(&artwork, 1));
        check(center(altered).b > 230 && altered[20 * config.width + 50].r > 240,
              "perception replaces the face while preserving the surrounding artwork");
        face.alteredFace[0].a = 0;
        materials.setArt(red, face);
        check(center(draw(std::span(&artwork, 1))).r > 240,
              "transparent alternate face pixels preserve the original art");
        face.pixels[0].a = 0;
        face.alteredFace[0].a = 255;
        materials.setArt(red, face);
        const std::array<MeshInstance, 2> silhouette{{artwork, {far, {}}}};
        check(center(draw(silhouette)).g > 240,
              "alternate artwork cannot make the original transparent silhouette opaque");
        // The camera sees the receiver around the door; the offset light sees the door in front of
        // it.
        materials.setArt(red, solid({255, 255, 255, 255}));
        materials.setArt(green, solid({0, 255, 0, 255}));
        options.distortion = 0;
        style.ambient = 0;
        Light spot{.type = LightType::Spot,
                   .position = {1, 0, 0},
                   .intensity = .8f,
                   .rangeMeters = 10,
                   .attenuationPerMeterSquared = 0,
                   .innerHalfAngleRadians = 30 * pi3 / 180,
                   .outerHalfAngleRadians = 45 * pi3 / 180,
                   .castsShadow = true};
        options.lights = std::span(&spot, 1);
        const MeshInstance receiver{plane(4, 2, red), {}};
        const auto lit = center(draw(std::span(&receiver, 1)));
        check(lit.r > 80, "unobstructed receiver is lit without self-shadow acne");
        Mesh3 doorMesh;
        quad(doorMesh, {0, .6f, 0}, {.6f, .6f, 0}, {.6f, -.6f, 0}, {0, -.6f, 0}, green);
        std::array<MeshInstance, 2> doorScene{
            {receiver, {makeMesh(std::move(doorMesh)), {.position = {.2f, 0, 2}}}}};
        const auto blocked = center(draw(doorScene));
        check(blocked.r < lit.r * .3f && renderer.stats().shadowPasses == 1,
              "closed door blocks the light reaching a visible receiver");
        doorScene[1].transform.rotation = Rotation3::axisAngle({0, 1, 0}, pi3 * .5f);
        const auto opened = center(draw(doorScene));
        check(opened.r > lit.r * .8f && renderer.stats().meshUploads == 0,
              "opening the door moves its shadow without uploading geometry");
        doorScene[1].transform.rotation = {};
        spot.position.x = -1;
        check(center(draw(doorScene)).r > lit.r * .8f,
              "moving the light updates its shadow projection");
        spot.position.x = 1;
        options.shadows.quality = ShadowQuality::Off;
        check(center(draw(doorScene)).r > lit.r * .8f && renderer.stats().shadowPasses == 0 &&
                  renderer.stats().shadowDrawCalls == 0 &&
                  renderer.stats().shadowMapBytes == sizeof(float),
              "disabling shadows removes their pass and releases the map");
        for (const auto quality :
             {ShadowQuality::Low, ShadowQuality::Medium, ShadowQuality::High}) {
            options.shadows.quality = quality;
            const auto pixel = center(draw(doorScene));
            const auto size = shadowQualityInfo(quality).mapSize;
            check(pixel.r < lit.r * .3f &&
                      renderer.stats().shadowMapBytes == (size_t{size} * size + 1) * sizeof(float),
                  "each shadow quality uses its budget and still blocks the light");
        }
        PaintedTexture cutoutDoor;
        cutoutDoor.width = 3;
        cutoutDoor.height = 1;
        cutoutDoor.pixels = {{0, 255, 0, 255}, {0, 255, 0, 0}, {0, 255, 0, 255}};
        materials.setArt(green, cutoutDoor);
        const auto throughHole = draw(doorScene);
        check(center(throughHole).r > lit.r * .8f && throughHole[50 * config.width + 65].r < 20,
              "shadow alpha cutout transmits light through the hole but blocks at the silhouette");
        materials.setArt(green, solid({0, 255, 0, 255}));
        doorScene[1].castsShadow = false;
        check(center(draw(doorScene)).r > lit.r * .8f,
              "an instance can opt out of casting a shadow");
        // Several probes lie on the same large triangle: lighting must vary across its pixels.
        spot.position = {};
        spot.innerHalfAngleRadians = .07f;
        spot.outerHalfAngleRadians = .22f;
        const auto beam = draw(std::span(&receiver, 1));
        const auto inner = center(beam).r, edge = beam[50 * config.width + 96].r;
        check(inner > 80 && edge > 10 && edge < inner && beam[50 * config.width + 112].r < 10,
              "spotlight has a lit center, a smooth edge and darkness beyond its cone");
        Light point{.color = {1, 0, 0},
                    .intensity = .8f,
                    .rangeMeters = 10,
                    .attenuationPerMeterSquared = 0};
        options.lights = std::span(&point, 1);
        const auto tinted = center(draw(std::span(&receiver, 1)));
        check(tinted.r > 80 && tinted.g < 5 && renderer.stats().shadowPasses == 0,
              "point lights supply their own color without a shadow pass");
        point.rangeMeters = 2;
        check(center(draw(std::span(&receiver, 1))).r < 5,
              "light contribution ends at its configured range");
        point.rangeMeters = 10;
        point.color = {0, 1, 0};
        spot.position = {1, 0, 0};
        spot.color = {1, 0, 0};
        spot.innerHalfAngleRadians = 30 * pi3 / 180;
        spot.outerHalfAngleRadians = 45 * pi3 / 180;
        doorScene[1].castsShadow = true;
        const std::array<Light, 2> combined{point, spot};
        options.lights = combined;
        const auto combinedPixel = center(draw(doorScene));
        check(combinedPixel.r < 10 && combinedPixel.g > 80 && renderer.stats().lightCount == 2,
              "a shadow only blocks its own light, including a caster outside slot zero");
        options = {};
        style.ambient = 1;
        materials.setArt(red, solid({255, 0, 0, 255}));
        const MeshInstance background{plane(4, 2, red), {}};
        std::array<Particle, 2> puffs{
            {{{0, 0, 2}, {0, 1, 0}, .5f, .5f}, {{0, 0, 3}, {0, 0, 1}, .5f, .5f}}};
        options.particles = std::span(puffs).first(1);
        const auto smoke = center(draw(std::span(&background, 1)));
        check(smoke.r > 90 && smoke.r < 170 && smoke.g > 90 && smoke.g < 170 &&
                  renderer.stats().particles == 1,
              "particle opacity blends with the opaque world");
        puffs[0].style = ParticleStyle::Faceted;
        puffs[0].rotationRadians = .37f;
        const auto angular = center(draw(std::span(&background, 1)));
        check(angular.r > 90 && angular.r < 170 && angular.g > 70 && angular.g < 170 &&
                  renderer.stats().triangles == background.mesh->size() + 20,
              "faceted smoke draws a shaded icosahedron with a single front opacity layer");
        puffs[0].style = ParticleStyle::Soft;
        puffs[0].position.z = 5;
        const auto hidden = center(draw(std::span(&background, 1)));
        check(hidden.r > 240 && hidden.g < 10, "world depth occludes smoke behind a wall");
        puffs[0].position.z = 2;
        options.particles = puffs;
        const auto overlap = center(draw(std::span(&background, 1)));
        check(overlap.g > overlap.b && overlap.b > 30,
              "near smoke blends over far smoke without depth writes");
        std::swap(puffs[0], puffs[1]);
        const auto reordered = center(draw(std::span(&background, 1)));
        check(overlap.r == reordered.r && overlap.g == reordered.g && overlap.b == reordered.b,
              "transparent composition does not depend on emitter submission order");
        options.particles = {};
        check(center(draw(std::span(&background, 1))).r > 240 && renderer.stats().particles == 0,
              "retired emitter leaves no stale GPU draw");
        const std::array<MeshInstance, 2> handsLayer{
            {background, {plane(5, 2, green), {}, false, {}, true}}};
        check(center(draw(handsLayer)).g > 240,
              "camera hands have their own depth and remain visible near a wall");
        puffs[0] = {{0, 0, 6}, {0, 0, 1}, .5f, .5f, true};
        options.particles = std::span(puffs).first(1);
        check(center(draw(handsLayer)).g > 240 && center(draw(handsLayer)).b < 10,
              "foreground hands occlude socket smoke behind them");
        puffs[0].position.z = 3;
        const auto handSmoke = center(draw(handsLayer));
        check(handSmoke.b > 90 && handSmoke.g > 90,
              "socket smoke blends in front of hands within the same depth layer");
        options.particles = {};
        check(center(draw(std::span(&background, 1))).r > 240,
              "removing foreground layers restores unobscured world");
        std::cout << "GPU checks passed on " << SDL_GetGPUDeviceDriver(device.get()) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
