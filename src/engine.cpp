#include "paper/engine.hpp"
#include "paper/assets/image.hpp"
#include "paper/core/pixel_format.hpp"
#include "paper/core/units.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace paper {
namespace {
constexpr int initialWidth = 1280, initialHeight = 800;
constexpr int minimumWidth = 960, minimumHeight = 600;
std::filesystem::path locateAssets(const EngineConfig& config) {
    if (config.exactAssetRoot) {
        if (!config.assetRoot.is_absolute() ||
            !std::filesystem::is_regular_file(config.assetRoot / config.fontFile))
            throw std::runtime_error("Invalid explicit asset root: " + config.assetRoot.string());
        return std::filesystem::canonical(config.assetRoot);
    }
    const char* basePath = SDL_GetBasePath();
    const auto base = basePath ? std::filesystem::path(basePath) : std::filesystem::path{};
    for (const auto& candidate :
         std::array{base / "assets", base / "../Resources/assets", config.assetRoot}) {
        if (std::filesystem::is_regular_file(candidate / config.fontFile))
            return candidate;
    }
    throw std::runtime_error("Cannot locate assets containing " + config.fontFile.string());
}
} // namespace
Engine::Engine(const EngineConfig& config)
    : session_(config.headless), audio_(config.muted), profile_(config.profile),
      headless_(config.headless) {
    window_.reset(SDL_CreateWindow(
        config.title.c_str(), initialWidth, initialHeight,
        headless_ ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
    sdl::check(bool(window_), "Create window");
#if defined(__APPLE__)
    constexpr auto formats = SDL_GPU_SHADERFORMAT_MSL;
#elif defined(_WIN32)
    constexpr auto formats = SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV;
#else
    constexpr auto formats = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
    device_.reset(SDL_CreateGPUDevice(formats, false, nullptr));
    sdl::check(bool(device_), "Create required GPU device (no CPU renderer fallback)");
    renderer_.reset(SDL_CreateGPURenderer(device_.get(), headless_ ? nullptr : window_.get()));
    sdl::check(bool(renderer_), "Create shared GPU presentation renderer");
    if (headless_) {
        offscreenFrame_.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_RGBA32,
                                                SDL_TEXTUREACCESS_TARGET, initialWidth,
                                                initialHeight));
        sdl::check(bool(offscreenFrame_), "Create offscreen GPU frame");
        sdl::check(SDL_SetRenderTarget(renderer_.get(), offscreenFrame_.get()),
                   "Select offscreen GPU frame");
    }
    sdl::check(SDL_SetWindowMinimumSize(window_.get(), minimumWidth, minimumHeight),
               "Set minimum window size");
    // Draw UI directly to native pixels; a low-resolution logical target blurs text on Retina.
    sdl::check(
        SDL_SetRenderLogicalPresentation(renderer_.get(), 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED),
        "Use native render target");
    sdl::check(SDL_SetRenderDrawBlendMode(renderer_.get(), SDL_BLENDMODE_BLEND), "Set UI blending");
    vsync_ = !headless_ && SDL_SetRenderVSync(renderer_.get(), 1);
    if (!headless_) {
        normal_.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT));
        hand_.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER));
        if (!normal_ || !hand_)
            SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "System cursor unavailable: %s", SDL_GetError());
    }
    assetPath_ = locateAssets(config);
    const auto deviceFormats = SDL_GetGPUShaderFormats(device_.get());
    const auto shaderName = (deviceFormats & SDL_GPU_SHADERFORMAT_MSL)    ? "scene.metal"
                            : (deviceFormats & SDL_GPU_SHADERFORMAT_DXIL) ? "world_vertex.dxil"
                                                                          : "world_vertex.spv";
    auto shaderRoot = assetPath_ / "shaders";
    if (!std::filesystem::is_regular_file(shaderRoot / shaderName))
        shaderRoot = config.shaderRoot;
    worldRenderer_ =
        std::make_unique<GpuRenderer>(*device_, *renderer_, shaderRoot, config.worldRender);
    text_ = std::make_unique<TextRenderer>(
        *renderer_, assetPath_ / config.fontFile,
        assetPath_ / (config.boldFontFile.empty() ? config.fontFile : config.boldFontFile),
        assetPath_ /
            (config.handwrittenFontFile.empty() ? config.fontFile : config.handwrittenFontFile));
    refreshCanvas();
    if (!headless_)
        audio_.open(assetPath_, config.sounds);
    diagnostics_.addCommand(
        "renderer", "Show backend, VSync and world resolution", [this](std::string_view) {
            const auto size = worldRenderSize();
            const char* driver = rendererName();
            return std::string(driver ? driver : "unknown") + "; VSync " + (vsync_ ? "on" : "off") +
                   "; world " + std::to_string(size.width) + "x" + std::to_string(size.height);
        });
    SDL_Log("Engine ready: renderer=%s, VSync=%s; F1 FPS, F2 diagnostics, F4 console",
            rendererName(), vsync_ ? "on" : "off");
}
void Engine::begin(Color color) {
    timings_ = {};
    worldDrawn_ = false;
    text_->setRasterScale(canvasScale_);
    sdl::check(SDL_SetRenderDrawColor(renderer_.get(), color.r, color.g, color.b, color.a),
               "Set clear color");
    sdl::check(SDL_RenderClear(renderer_.get()), "Clear frame");
}
void Engine::present() {
    debugOverlay_.draw(*this);
    sdl::check(SDL_RenderPresent(renderer_.get()), "Present frame");
}
void Engine::recordFrame(DebugFrame frame) {
    frame.encodeMs = timings_.world.encodeMs;
    frame.compositeMs = timings_.compositeMs;
    diagnostics_.record(frame);
    const auto now = SDL_GetTicksNS();
    if (diagnostics_.settings().spikes && frame.intervalMs > debugLimits::spikeMilliseconds &&
        now - lastSpikeLog_ >= units::nanosecondsPerSecond) {
        lastSpikeLog_ = now;
        diagnostics_.log(LogLevel::Warning, "frame",
                         "Slow interval: " + std::to_string(frame.intervalMs) +
                             " ms (includes pacing/present)");
    }
}
void Engine::refreshCanvas() {
    int outputWidth = 0, outputHeight = 0;
    if (!SDL_GetCurrentRenderOutputSize(renderer_.get(), &outputWidth, &outputHeight) ||
        outputWidth <= 0 || outputHeight <= 0)
        return;
    const float scale = std::min(outputWidth / static_cast<float>(width),
                                 outputHeight / static_cast<float>(height));
    const int wide = std::max(width, static_cast<int>(std::lround(outputWidth / scale)));
    const int tall = std::max(height, static_cast<int>(std::lround(outputHeight / scale)));
    if (wide == canvasWidth_ && tall == canvasHeight_ && scale == canvasScale_)
        return;
    sdl::check(SDL_SetRenderScale(renderer_.get(), scale, scale), "Adapt screen canvas");
    canvasWidth_ = wide;
    canvasHeight_ = tall;
    canvasScale_ = scale;
    canvasOffset_ = {(wide - width) * .5f,
                     (tall - height) * .5f}; // numbers: center/two-sided padding.
    input_.canvasChanged = true;
    input_.x = input_.y = -1;
}
void Engine::cover(Color color) {
    rect({-canvasOffset_.x, -canvasOffset_.y, static_cast<float>(canvasWidth_),
          static_cast<float>(canvasHeight_)},
         color);
}
Vec3 Engine::screenRay(const Camera& camera, float x, float y) const {
    const float nx = 2 * (x + canvasOffset_.x) / canvasWidth_ - 1;
    const float ny = 1 - 2 * (y + canvasOffset_.y) / canvasHeight_;
    const float horizontal = std::tan(camera.fov * .5f);
    const float aspect = static_cast<float>(canvasWidth_) / canvasHeight_;
    return normalized(camera.forward() + camera.right() * (nx * horizontal) +
                      camera.up() * (ny * horizontal / aspect));
}
void Engine::image(std::string_view asset, Rect box, Color tint, bool contain,
                   std::optional<Rect> atlasRegion) {
    auto found = images_.find(asset);
    if (found == images_.end()) {
        const auto loaded = loadSprite(assetPath_ / asset);
        const auto& pixels = loaded.texture;
        UiImage value;
        value.texture.reset(SDL_CreateTexture(renderer_.get(), SDL_PIXELFORMAT_RGBA32,
                                              SDL_TEXTUREACCESS_STATIC, pixels.width,
                                              pixels.height));
        sdl::check(bool(value.texture), "Create UI image");
        sdl::check(SDL_UpdateTexture(value.texture.get(), nullptr, pixels.pixels.data(),
                                     pixels.width * sizeof(Pixel)),
                   "Upload UI image");
        sdl::check(SDL_SetTextureBlendMode(value.texture.get(), SDL_BLENDMODE_BLEND),
                   "Blend UI image");
        sdl::check(SDL_SetTextureScaleMode(value.texture.get(), SDL_SCALEMODE_LINEAR),
                   "Sample UI image");
        int x0 = pixels.width, y0 = pixels.height, x1 = 0, y1 = 0;
        for (int y = 0; y < pixels.height; ++y)
            for (int x = 0; x < pixels.width; ++x)
                if (pixels.pixels[y * pixels.width + x].a >= pixelFormat::opaqueAlpha) {
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x + 1);
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y + 1);
                }
        value.source = {static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1 - x0),
                        static_cast<float>(y1 - y0)};
        found = images_.emplace(std::string(asset), std::move(value)).first;
    }
    auto& value = found->second;
    auto source = value.source;
    if (atlasRegion) {
        float textureWidth = 0, textureHeight = 0;
        sdl::check(SDL_GetTextureSize(value.texture.get(), &textureWidth, &textureHeight),
                   "Query sprite atlas size");
        const auto region = *atlasRegion;
        if (!std::isfinite(region.x) || !std::isfinite(region.y) || !std::isfinite(region.w) ||
            !std::isfinite(region.h) || region.x < 0 || region.y < 0 || region.w <= 0 ||
            region.h <= 0 || region.x + region.w > 1 || region.y + region.h > 1)
            throw std::invalid_argument("Invalid normalized sprite atlas region");
        source = {region.x * textureWidth, region.y * textureHeight, region.w * textureWidth,
                  region.h * textureHeight};
    }
    if (contain) {
        const float scale = std::min(box.w / source.w, box.h / source.h);
        const float w = source.w * scale, h = source.h * scale;
        box.x += (box.w - w) * .5f; // numbers: center/two-sided padding.
        box.y += (box.h - h) * .5f; // numbers: center/two-sided padding.
        box.w = w;
        box.h = h;
    }
    sdl::check(SDL_SetTextureColorMod(value.texture.get(), tint.r, tint.g, tint.b),
               "Tint UI image");
    sdl::check(SDL_SetTextureAlphaMod(value.texture.get(), tint.a), "Fade UI image");
    const SDL_FRect target{box.x + canvasOffset_.x, box.y + canvasOffset_.y, box.w, box.h};
    sdl::check(SDL_RenderTexture(renderer_.get(), value.texture.get(), &source, &target),
               "Draw UI image");
}
void Engine::rect(Rect b, Color c) {
    sdl::check(SDL_SetRenderDrawColor(renderer_.get(), c.r, c.g, c.b, c.a), "Draw UI primitive");
    SDL_FRect r{b.x + canvasOffset_.x, b.y + canvasOffset_.y, b.w, b.h};
    sdl::check(SDL_RenderFillRect(renderer_.get(), &r), "Draw UI primitive");
}
void Engine::outline(Rect b, Color c) {
    sdl::check(SDL_SetRenderDrawColor(renderer_.get(), c.r, c.g, c.b, c.a), "Draw UI primitive");
    SDL_FRect r{b.x + canvasOffset_.x, b.y + canvasOffset_.y, b.w, b.h};
    sdl::check(SDL_RenderRect(renderer_.get(), &r), "Draw UI primitive");
}
void Engine::line(float x1, float y1, float x2, float y2, Color c) {
    sdl::check(SDL_SetRenderDrawColor(renderer_.get(), c.r, c.g, c.b, c.a), "Draw UI primitive");
    sdl::check(SDL_RenderLine(renderer_.get(), x1 + canvasOffset_.x, y1 + canvasOffset_.y,
                              x2 + canvasOffset_.x, y2 + canvasOffset_.y),
               "Draw UI primitive");
}
void Engine::roundedRect(Rect b, float radius, Color c) {
    if (b.w <= 0 || b.h <= 0)
        return;
    radius =
        std::clamp(radius, 0.f, std::min(b.w, b.h) * .5f); // numbers: center/two-sided padding.
    if (radius == 0) {
        rect(b, c);
        return;
    }
    b.x += canvasOffset_.x;
    b.y += canvasOffset_.y;
    constexpr int segments = 8, cornerCount = 4, triangleCorners = 3;
    constexpr int boundary = cornerCount * (segments + 1);
    const SDL_FColor color{c.r / static_cast<float>(pixelFormat::maximumChannel),
                           c.g / static_cast<float>(pixelFormat::maximumChannel),
                           c.b / static_cast<float>(pixelFormat::maximumChannel),
                           c.a / static_cast<float>(pixelFormat::maximumChannel)};
    std::array<SDL_Vertex, boundary + 1> vertices{};
    std::array<int, boundary * triangleCorners> indices{};
    vertices[0] = {
        {b.x + b.w * .5f, b.y + b.h * .5f}, color, {0, 0}}; // numbers: center/two-sided padding.
    const std::array<Vec2, cornerCount> centers{{{b.x + b.w - radius, b.y + radius},
                                                 {b.x + b.w - radius, b.y + b.h - radius},
                                                 {b.x + radius, b.y + b.h - radius},
                                                 {b.x + radius, b.y + radius}}};
    for (int corner = 0; corner < cornerCount; ++corner)
        for (int step = 0; step <= segments; ++step) {
            const int i = corner * (segments + 1) + step;
            const float angle = (corner - 1 + step / static_cast<float>(segments)) * pi3 * .5f;
            vertices[i + 1] = {{centers[corner].x + std::cos(angle) * radius,
                                centers[corner].y + std::sin(angle) * radius},
                               color,
                               {0, 0}};
            indices[i * triangleCorners] = 0;
            indices[i * triangleCorners + 1] = i + 1;
            indices[i * triangleCorners + 2] =
                (i + 1) % boundary + 1; // numbers: triangle fan layout.
        }
    // Shared edges form one continuous surface, including at fractional canvas scales.
    sdl::check(SDL_RenderGeometry(renderer_.get(), nullptr, vertices.data(), boundary + 1,
                                  indices.data(), boundary * triangleCorners),
               "Draw rounded UI primitive");
}
void Engine::gradient(Rect b, Color c, bool horizontal, bool reverse) {
    b.x += canvasOffset_.x;
    b.y += canvasOffset_.y;
    // Interpolate one quad. Separate narrow rectangles produce seams at non-integer DPI scales.
    SDL_FColor transparent{c.r / static_cast<float>(pixelFormat::maximumChannel),
                           c.g / static_cast<float>(pixelFormat::maximumChannel),
                           c.b / static_cast<float>(pixelFormat::maximumChannel), 0};
    SDL_FColor opaque = transparent;
    opaque.a = c.a / static_cast<float>(pixelFormat::maximumChannel);
    SDL_FColor first = reverse ? opaque : transparent, last = reverse ? transparent : opaque;
    SDL_Vertex vertices[4] = {{{b.x, b.y}, first, {0, 0}},
                              {{b.x + b.w, b.y}, horizontal ? last : first, {0, 0}},
                              {{b.x + b.w, b.y + b.h}, last, {0, 0}},
                              {{b.x, b.y + b.h}, horizontal ? first : last, {0, 0}}};
    const int indices[] = {0, 1, 2, 0, 2, 3};
    sdl::check(SDL_RenderGeometry(renderer_.get(), nullptr, vertices, std::size(vertices), indices,
                                  std::size(indices)),
               "Draw UI primitive");
}
void Engine::setTitle(const std::string& title) {
    sdl::check(SDL_SetWindowTitle(window_.get(), title.c_str()), "Set window title");
}
bool Engine::setFullscreen(bool value) {
    if (fullscreen_ == value)
        return true;
    if (headless_)
        return false;
    if (!SDL_SetWindowFullscreen(window_.get(), value)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Set fullscreen: %s", SDL_GetError());
        return false;
    }
    // Fullscreen can be asynchronous (notably on macOS). Save the resulting mode.
    if (!SDL_SyncWindow(window_.get()))
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Wait for fullscreen: %s", SDL_GetError());
    fullscreen_ = (SDL_GetWindowFlags(window_.get()) & SDL_WINDOW_FULLSCREEN) != 0;
    return fullscreen_ == value;
}
bool Engine::screenshot(const std::filesystem::path& path) {
    sdl::Surface surface(SDL_RenderReadPixels(renderer_.get(), nullptr));
    return surface && SDL_SaveBMP(surface.get(), path.string().c_str());
}
void Engine::setHand(bool value) {
    SDL_Cursor* cursor = value && hand_ ? hand_.get() : normal_.get();
    if (!headless_ && !mouseCaptured_ && cursor && cursor != activeCursor_ && SDL_SetCursor(cursor))
        activeCursor_ = cursor;
}
void Engine::textInput(bool value) {
    if (headless_)
        return;
    if (!(value ? SDL_StartTextInput(window_.get()) : SDL_StopTextInput(window_.get())))
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Change text input: %s", SDL_GetError());
}
void Engine::captureMouse(bool value) {
    if (mouseCaptured_ == value)
        return;
    if (!headless_ && !SDL_SetWindowRelativeMouseMode(window_.get(), value)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Capture mouse: %s", SDL_GetError());
        return;
    }
    mouseCaptured_ = value;
    if (!headless_) {
        if (value)
            SDL_HideCursor();
        else
            SDL_ShowCursor();
    }
    input_.dx = input_.dy = 0;
}
bool Engine::setWorldRenderSize(RenderSize size) {
    try {
        worldRenderer_->resize(size);
        return true;
    } catch (const std::exception& error) {
        SDL_Log("Cannot change world resolution: %s", error.what());
        return false;
    }
}
void Engine::drawWorld(const MaterialLibrary& materials, RenderScene scene, const Camera& camera,
                       const RenderOptions& options, const RenderStyle& style) {
    if (worldDrawn_)
        throw std::logic_error("Only one world view may be composed per frame");
    worldDrawn_ = true;
    auto view = options;
    view.aspect = static_cast<float>(canvasWidth_) / canvasHeight_;
    const bool measure = profile_ || diagnostics_.settings().renderStats;
    worldRenderer_->render(scene, materials, camera, view, style, measure);
    timings_.world = worldRenderer_->stats();
    const Uint64 started = measure ? SDL_GetTicksNS() : 0;
    const SDL_FRect target{0, 0, static_cast<float>(canvasWidth_),
                           static_cast<float>(canvasHeight_)};
    sdl::check(SDL_RenderTexture(renderer_.get(), worldRenderer_->image(), nullptr, &target),
               "Compose GPU world with UI");
    if (measure)
        timings_.compositeMs =
            static_cast<double>(SDL_GetTicksNS() - started) / units::nanosecondsPerMillisecond;
}
} // namespace paper
