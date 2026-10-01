#pragma once
#include "paper/audio/mixer.hpp"
#include "paper/content/document.hpp"
#include "paper/render/scene.hpp"
#include <functional>
#include <map>
#include <optional>
namespace paper {
struct AudioSourceDefinition {
    std::string id, sound, node;
    Vec3 offset;
    float gain = 1, rangeMeters = audioParameters::defaultRangeMeters;
};
struct AcousticZone {
    std::string id, ambience;
    Box3 bounds;
    int priority = 0;
    float fadeMeters = 1, wet = 1, feedback = audioParameters::echoFeedbackGain, damping = 0;
};
struct AudioBankDefinition {
    std::vector<SoundDefinition> sounds;
    std::vector<AudioSourceDefinition> sources;
    std::vector<AcousticZone> zones;
};
void validateAudioBank(const AudioBankDefinition& bank);
[[nodiscard]] AudioBankDefinition parseAudioBank(std::string_view text,
                                                 std::string_view name = "audio bank");
[[nodiscard]] AudioBankDefinition loadAudioBank(const std::filesystem::path& path);
[[nodiscard]] std::string writeAudioBank(const AudioBankDefinition& bank);
struct AudioLoopCommand {
    std::string sound;
    SoundPlacement placement;
};
struct AudioFrame {
    AudioScene scene;
    std::array<AudioLoopCommand, AudioMixer::maxLoops> loops{};
};
// Prepares a bounded audio-thread snapshot on the host thread. Owns all loop slots.
class AudioBindings {
  public:
    explicit AudioBindings(AudioBankDefinition bank);
    void bind(std::string instance, std::string_view source, std::string node = {});
    void unbind(std::string_view instance);
    using Resolve = std::function<std::optional<MeshTransform>(std::string_view)>;
    [[nodiscard]] AudioFrame frame(AudioScene scene, const Resolve& resolve, float seconds);
    [[nodiscard]] std::optional<AudioLoopCommand> event(std::string_view instance,
                                                        const Resolve& resolve) const;
    [[nodiscard]] const AudioBankDefinition& bank() const noexcept { return bank_; }

  private:
    struct Binding {
        size_t source;
        std::string node;
        std::optional<size_t> slot;
    };
    std::optional<SoundPlacement> placement(const Binding& binding, const Resolve& resolve) const;
    AudioBankDefinition bank_;
    std::map<std::string, Binding, std::less<>> bindings_;
    std::array<bool, AudioMixer::maxLoops> occupied_{};
    float wet_ = 1, feedback_ = audioParameters::echoFeedbackGain, damping_ = 0;
};
} // namespace paper
