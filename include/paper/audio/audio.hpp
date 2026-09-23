#pragma once
#include "paper/audio/mixer.hpp"
#include "paper/sdl/resources.hpp"
#include <atomic>
#include <filesystem>
#include <memory>

namespace paper {
// Main-thread facade. The SDL stream lock serializes mixer commands with its callback.
class Audio {
  public:
    explicit Audio(bool muted) noexcept : forcedMute_(muted) {}
    void open(const std::filesystem::path& assets, std::span<const SoundDefinition> definitions);
    void play(SoundId effect, SoundPlacement placement = {}) noexcept;
    void stop(SoundId effect) noexcept;
    void stopWorld() noexcept;
    void update(const AudioScene& scene);
    void loop(size_t slot, SoundId effect, SoundPlacement placement) noexcept;
    void timeline(size_t slot, SoundId effect, SoundPlacement placement, double seconds,
                  bool paused) noexcept;
    void setMuted(bool value);
    void setMix(float master, float ambience, float effects, float interfaceVolume,
                float voices) noexcept;
    [[nodiscard]] bool muted() const noexcept { return muted_ || forcedMute_; }
    [[nodiscard]] bool forcedMute() const noexcept { return forcedMute_; }
    [[nodiscard]] bool available() const noexcept { return bool(stream_); }

  private:
    static void SDLCALL feed(void* userdata, SDL_AudioStream* stream, int additional, int total);
    void disable(const char* operation);
    bool muted_ = false, forcedMute_ = false;
    std::atomic<bool> callbackFailed_{false};
    std::unique_ptr<AudioMixer> mixer_;
    // Destroy/join the callback before its mixer and error flag are destroyed.
    sdl::AudioStream stream_;
};
} // namespace paper
