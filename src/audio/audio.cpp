#include "paper/audio/audio.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

namespace paper {
using namespace audioParameters;
namespace {
class StreamLock {
  public:
    explicit StreamLock(SDL_AudioStream* stream) : stream_(stream) {
        if (stream_ && !SDL_LockAudioStream(stream_))
            stream_ = nullptr;
    }
    ~StreamLock() {
        if (stream_)
            SDL_UnlockAudioStream(stream_);
    }
    explicit operator bool() const { return stream_ != nullptr; }

  private:
    SDL_AudioStream* stream_;
};
SoundBank loadBank(const std::filesystem::path& root,
                   std::span<const SoundDefinition> definitions) {
    SoundBank bank(definitions.size());
    for (size_t id = 0; id < definitions.size(); ++id) {
        const auto& definition = definitions[id];
        for (unsigned variant = 1; variant <= definition.variants; ++variant) {
            const auto path =
                root / "audio" /
                (std::string(definition.file) + "-" + std::to_string(variant) + ".wav");
            SDL_AudioSpec source{};
            Uint8* bytes = nullptr;
            Uint32 length = 0;
            if (!SDL_LoadWAV(path.string().c_str(), &source, &bytes, &length)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Missing sound %s: %s", path.string().c_str(),
                            SDL_GetError());
                continue;
            }
            sdl::Owner<Uint8, SDL_free> sourceData(bytes);
            if (length > maximumSourceBytes) {
                SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Sound too large: %s", path.string().c_str());
                continue;
            }
            SDL_AudioSpec target{};
            target.format = SDL_AUDIO_F32;
            target.channels = 1;
            target.freq = AudioMixer::sampleRate;
            Uint8* converted = nullptr;
            int size = 0;
            const bool okay = SDL_ConvertAudioSamples(&source, bytes, static_cast<int>(length),
                                                      &target, &converted, &size);
            sdl::Owner<Uint8, SDL_free> convertedData(converted);
            sourceData.reset();
            if (!okay || size < minimumSampleFrames * static_cast<int>(sizeof(float)) ||
                size > AudioMixer::sampleRate * maximumSourceSeconds *
                           static_cast<int>(sizeof(float))) {
                SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Invalid sound: %s", path.string().c_str());
                continue;
            }
            std::vector<float> samples(static_cast<size_t>(size) / sizeof(float));
            std::memcpy(samples.data(), converted, samples.size() * sizeof(float));
            convertedData.reset();
            float peak = 0;
            bool valid = true;
            for (float value : samples) {
                valid &= std::isfinite(value);
                peak = std::max(peak, std::abs(value));
            }
            if (!valid) {
                SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Non-finite sound: %s", path.string().c_str());
                continue;
            }
            // Replacements retain quieter mastering; unexpectedly loud files cannot spike.
            if (peak > loadedPeakCeiling)
                for (float& value : samples)
                    value *= loadedPeakCeiling / peak;
            bank[id].push_back(std::move(samples));
        }
    }
    return bank;
}
} // namespace
void Audio::disable(const char* operation) {
    SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "%s: %s; audio disabled", operation, SDL_GetError());
    stream_.reset();
}
void Audio::open(const std::filesystem::path& assets,
                 std::span<const SoundDefinition> definitions) {
    stream_.reset();
    mixer_.reset();
    auto bank = loadBank(assets, definitions);
    if (std::all_of(bank.begin(), bank.end(),
                    [](const auto& variants) { return variants.empty(); })) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "No usable audio assets; audio disabled");
        return;
    }
    mixer_ = std::make_unique<AudioMixer>(definitions, std::move(bank));
    mixer_->setMuted(muted());
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        disable("Initialize audio");
        return;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = channels;
    spec.freq = AudioMixer::sampleRate;
    callbackFailed_ = false;
    stream_.reset(SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, this));
    if (!stream_ || !SDL_ResumeAudioStreamDevice(stream_.get()))
        disable("Open audio device");
}
void SDLCALL Audio::feed(void* userdata, SDL_AudioStream* stream, int additional, int) {
    auto& audio = *static_cast<Audio*>(userdata);
    if (audio.callbackFailed_ || additional <= 0)
        return;
    constexpr int bytesPerFrame = channels * static_cast<int>(sizeof(float));
    std::array<float, callbackFrames * channels> samples{};
    int frames = additional / bytesPerFrame + (additional % bytesPerFrame != 0 ? 1 : 0);
    while (frames > 0) {
        const int count = std::min(frames, callbackFrames);
        audio.mixer_->render(std::span(samples.data(), static_cast<size_t>(count) * channels));
        if (!SDL_PutAudioStreamData(stream, samples.data(), count * bytesPerFrame)) {
            audio.callbackFailed_ = true;
            return;
        }
        frames -= count;
    }
}
void Audio::play(SoundId effect, SoundPlacement placement) noexcept {
    if (StreamLock lock(stream_.get()); lock)
        mixer_->play(effect, placement);
}
void Audio::stop(SoundId effect) noexcept {
    if (StreamLock lock(stream_.get()); lock)
        mixer_->stop(effect);
}
void Audio::stopWorld() noexcept {
    if (StreamLock lock(stream_.get()); lock)
        mixer_->stopWorld();
}
void Audio::loop(size_t slot, SoundId effect, SoundPlacement placement) noexcept {
    if (StreamLock lock(stream_.get()); lock)
        mixer_->loop(slot, effect, placement);
}
void Audio::timeline(size_t slot, SoundId effect, SoundPlacement placement, double seconds,
                     bool paused) noexcept {
    if (StreamLock lock(stream_.get()); lock)
        mixer_->timeline(slot, effect, placement, seconds, paused);
}
void Audio::update(const AudioScene& scene) {
    if (callbackFailed_) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Audio callback could not queue PCM; audio disabled");
        stream_.reset();
        return;
    }
    if (StreamLock lock(stream_.get()); lock)
        mixer_->setScene(scene);
}
void Audio::setMuted(bool value) {
    muted_ = value;
    if (StreamLock lock(stream_.get()); lock)
        mixer_->setMuted(muted());
}
void Audio::setMix(float master, float ambience, float effects, float interfaceVolume,
                   float voices) noexcept {
    if (StreamLock lock(stream_.get()); lock)
        mixer_->setMix(master, {ambience, effects, interfaceVolume, voices});
}
} // namespace paper
