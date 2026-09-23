#pragma once
#include "paper/audio/parameters.hpp"
#include "paper/core/math3d.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace paper {
enum class AudioBus { Ambience, Effects, Interface, Voices, Count };
// IDs are borrowed only for the duration of a command; voices store validated indices.
using SoundId = std::string_view;
inline constexpr size_t busCount = static_cast<size_t>(AudioBus::Count);
struct SoundDefinition {
    std::string id, file;
    AudioBus bus;
    unsigned variants;
    float gain, cooldownSeconds;
    unsigned priority;
    bool loop = false;
    bool triggersDucking = true, receivesDucking = false;
    float spatialReverbSend = audioParameters::defaultSpatialReverbSend, nonSpatialReverbSend = 0;
};
using SoundBank = std::vector<std::vector<std::vector<float>>>;
struct SoundPlacement {
    Vec3 position;
    float gain = 1, rangeMeters = audioParameters::defaultRangeMeters;
    bool spatial = false;
    float pan = 0;        // Listener-relative stereo width for non-spatial beds and foley.
    bool preview = false; // Settings auditions retain their bus but bypass world pause.
};
struct AudioScene {
    Vec3 listener, right{1, 0, 0};
    // Only walls and moving doors: furniture should not seal an entire room acoustically.
    std::array<Box3, audioParameters::maximumBarriers> barriers{};
    size_t barrierCount = 0;
    bool active = false, paused = false, focused = true;
};
struct SpatialMix {
    float left, right, obstruction;
};
[[nodiscard]] SpatialMix spatialMix(const AudioScene& scene, const SoundPlacement& sound) noexcept;

// Pure PCM mixer; callers serialize access. render() never allocates or performs I/O.
class AudioMixer {
  public:
    static constexpr int sampleRate = 44100;
    static constexpr size_t maxVoices = 32, maxLoops = 16;
    static constexpr size_t maxTimelineVoices = 8;
    static constexpr size_t maximumSounds = 256;
    AudioMixer(std::span<const SoundDefinition> definitions, SoundBank bank);
    AudioMixer(const AudioMixer&) = delete;
    AudioMixer& operator=(const AudioMixer&) = delete;
    AudioMixer(AudioMixer&&) = delete;
    AudioMixer& operator=(AudioMixer&&) = delete;
    void setScene(const AudioScene& scene) noexcept;
    void setMix(float master, const std::array<float, busCount>& volumes) noexcept;
    void setMuted(bool muted) noexcept;
    bool play(SoundId effect, SoundPlacement placement = {}) noexcept;
    void loop(size_t slot, SoundId effect, SoundPlacement placement) noexcept;
    // A caller-owned gameplay timeline; variant zero, original pitch, seekable and pausable.
    // Empty/invalid bindings clear the slot. No history or game objects enter the audio thread.
    void timeline(size_t slot, SoundId effect, SoundPlacement placement, double seconds,
                  bool paused) noexcept;
    void stop(SoundId effect) noexcept;
    void stopWorld() noexcept;
    void render(std::span<float> stereo) noexcept;
    [[nodiscard]] size_t activeVoices() const noexcept;

  private:
    struct Voice {
        const std::vector<float>* sample = nullptr;
        size_t effect = 0;
        SoundPlacement placement;
        double cursor = 0;
        float pitch = 1, variation = 1, left = 0, right = 0, level = 0, filtered = 0, filter = 1;
        bool stopping = false;
    };
    float random() noexcept;
    float sample(Voice& voice, bool looped) noexcept;
    [[nodiscard]] size_t find(SoundId id) const noexcept;
    std::vector<SoundDefinition> definitions_;
    SoundBank bank_;
    AudioScene scene_;
    std::array<Voice, maxVoices> voices_{};
    std::array<Voice, maxLoops> loops_{};
    std::array<Voice, maxTimelineVoices> timeline_{};
    std::array<bool, maxTimelineVoices> timelinePaused_{};
    std::array<bool, maxTimelineVoices> timelineBound_{};
    std::array<double, maxTimelineVoices> timelineSeconds_{};
    Voice& voiceAt(size_t index) noexcept;
    std::vector<std::uint64_t> nextPlay_;
    std::vector<size_t> lastVariant_;
    std::array<float, busCount> volumes_{1, 1, 1, 1}, currentVolumes_{};
    std::array<float, audioParameters::echoLeftFrames> echoLeft_{};
    std::array<float, audioParameters::echoRightFrames> echoRight_{};
    size_t echoPosition_ = 0;
    std::uint64_t frames_ = 0;
    std::uint32_t seed_ = 92831;
    float master_ = 1, currentMaster_ = 0, worldGain_ = 0, eventGain_ = 1, duck_ = 1, limiter_ = 1;
    float dcLeft_ = 0, dcRight_ = 0;
    bool muted_ = false;
};
} // namespace paper
