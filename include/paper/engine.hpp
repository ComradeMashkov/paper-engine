#pragma once
#include "paper/audio/audio.hpp"
#include "paper/debug/overlay.hpp"
#include "paper/debug/sdl_log.hpp"
#include "paper/input.hpp"
#include "paper/render/draw_types.hpp"
#include "paper/render/gpu_renderer.hpp"
#include "paper/sdl/resources.hpp"
#include "paper/text/parameters.hpp"
#include "paper/text/text_renderer.hpp"
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace paper {
struct EngineConfig {
    std::string title;
    std::filesystem::path assetRoot;
    std::filesystem::path shaderRoot;
    RenderConfig worldRender;
    std::filesystem::path fontFile;
    std::filesystem::path boldFontFile;
    std::filesystem::path handwrittenFontFile;
    std::span<const SoundDefinition> sounds;
    bool headless = false;
    bool muted = false;
    bool profile = false;
    // Explicit content snapshots must never fall back to bundled/source assets.
    bool exactAssetRoot = false;
};
struct FrameTimings {
    RenderStats world;
    double compositeMs = 0;
};
// Construct, use and destroy on the SDL main thread.
class Engine {
  public:
    static constexpr int width = 1440, height = 900;
    explicit Engine(const EngineConfig& config);
    ~Engine() = default;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;
    [[nodiscard]] bool poll();
    void requestQuit() noexcept { quit_ = true; }
    [[nodiscard]] const Input& input() const noexcept { return input_; }
    void begin(Color clearColor);
    void cover(Color color);
    [[nodiscard]] FrameTimings frameTimings() const noexcept { return timings_; }
    [[nodiscard]] const char* rendererName() const noexcept {
        return SDL_GetGPUDeviceDriver(device_.get());
    }
    void present();
    [[nodiscard]] Diagnostics& diagnostics() noexcept { return diagnostics_; }
    [[nodiscard]] bool debugModal() const noexcept { return debugOverlay_.modal(); }
    void configureDiagnostics(const std::filesystem::path& file) { debugOverlay_.configure(file); }
    // Called once after present by the host loop; interval includes VSync/fallback pacing.
    void recordFrame(DebugFrame frame);
    void rect(Rect box, Color color);
    void roundedRect(Rect box, float radius, Color color);
    void outline(Rect box, Color color);
    void line(float x1, float y1, float x2, float y2, Color color);
    void gradient(Rect box, Color color, bool horizontal, bool reverse = false);
    void image(std::string_view asset, Rect box, Color tint = untinted, bool contain = true,
               std::optional<Rect> atlasRegion = {});
    [[nodiscard]] Vec3 screenRay(const Camera& camera, float x, float y) const;
    void text(std::string_view value, float x, float y, int size, Color color,
              FontWeight weight = FontWeight::Regular) {
        text_->text(value, x + canvasOffset_.x, y + canvasOffset_.y, size, color, weight);
    }
    [[nodiscard]] float measure(std::string_view value, int size,
                                FontWeight weight = FontWeight::Regular) {
        return text_->measure(value, size, weight);
    }
    [[nodiscard]] float paragraphHeight(std::string_view value, float maxWidth, int size,
                                        float leading = textParameters::paragraphLeading,
                                        FontWeight weight = FontWeight::Regular) {
        return text_->paragraphHeight(value, maxWidth, size, leading, weight);
    }
    float paragraph(std::string_view value, float x, float y, float maxWidth, int size, Color color,
                    float leading = textParameters::paragraphLeading,
                    FontWeight weight = FontWeight::Regular) {
        return text_->paragraph(value, x + canvasOffset_.x, y + canvasOffset_.y, maxWidth, size,
                                color, leading, weight) -
               canvasOffset_.y;
    }
    void sound(SoundId effect, SoundPlacement placement = {}) noexcept {
        audio_.play(effect, placement);
    }
    void stopSound(SoundId effect) noexcept { audio_.stop(effect); }
    void stopWorldSounds() noexcept { audio_.stopWorld(); }
    void audio(const AudioScene& scene) { audio_.update(scene); }
    void audioLoop(size_t slot, SoundId effect, SoundPlacement placement) noexcept {
        audio_.loop(slot, effect, placement);
    }
    void audioTimeline(size_t slot, SoundId effect, SoundPlacement placement, double seconds,
                       bool paused) noexcept {
        audio_.timeline(slot, effect, placement, seconds, paused);
    }
    void setMuted(bool value) { audio_.setMuted(value); }
    void setAudioMix(float master, float ambience, float effects, float interfaceVolume,
                     float voices) noexcept {
        audio_.setMix(master, ambience, effects, interfaceVolume, voices);
    }
    [[nodiscard]] bool focused() const noexcept {
        // Offscreen previews have no OS input focus to lose.
        return headless_ || (SDL_GetWindowFlags(window_.get()) & SDL_WINDOW_INPUT_FOCUS) != 0;
    }
    [[nodiscard]] bool mutedAudio() const noexcept { return audio_.muted(); }
    [[nodiscard]] bool forcedMute() const noexcept { return audio_.forcedMute(); }
    [[nodiscard]] bool audioAvailable() const noexcept { return audio_.available(); }
    void setTitle(const std::string& title);
    [[nodiscard]] bool setFullscreen(bool value);
    [[nodiscard]] bool isFullscreen() const noexcept { return fullscreen_; }
    [[nodiscard]] bool setWorldRenderSize(RenderSize size);
    [[nodiscard]] RenderSize worldRenderSize() const noexcept { return worldRenderer_->size(); }
    [[nodiscard]] bool screenshot(const std::filesystem::path& path);
    void setHand(bool value);
    void captureMouse(bool value);
    void textInput(bool value);
    [[nodiscard]] bool mouseCaptured() const noexcept { return mouseCaptured_; }
    [[nodiscard]] bool vsyncEnabled() const noexcept { return vsync_; }
    void drawWorld(const MaterialLibrary& materials, RenderScene scene, const Camera& camera,
                   const RenderOptions& options, const RenderStyle& style);
    [[nodiscard]] const std::filesystem::path& assets() const noexcept { return assetPath_; }

  private:
    void refreshCanvas();
    struct UiImage {
        sdl::Texture texture;
        SDL_FRect source;
    };
    // Reverse destruction: all GPU resources -> SDL renderer -> GPU device -> window -> SDL.
    sdl::Session session_;
    Diagnostics diagnostics_;
    SdlLogCapture logCapture_{diagnostics_};
    DebugOverlay debugOverlay_{diagnostics_};
    sdl::Window window_;
    sdl::GPUDevice device_;
    sdl::Renderer renderer_;
    sdl::Cursor normal_, hand_;
    SDL_Cursor* activeCursor_ = nullptr;
    sdl::Texture offscreenFrame_;
    std::map<std::string, UiImage, std::less<>> images_;
    std::unique_ptr<TextRenderer> text_;
    std::unique_ptr<GpuRenderer> worldRenderer_;
    Audio audio_;
    std::filesystem::path assetPath_;
    Input input_;
    Vec2 canvasOffset_;
    int canvasWidth_ = width, canvasHeight_ = height;
    float canvasScale_ = 1;
    FrameTimings timings_;
    Uint64 lastSpikeLog_ = 0;
    bool profile_ = false, worldDrawn_ = false;
    bool headless_ = false, mouseCaptured_ = false, quit_ = false, vsync_ = false,
         fullscreen_ = false;
};
} // namespace paper
