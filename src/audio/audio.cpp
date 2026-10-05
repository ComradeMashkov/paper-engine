#include "paper/audio/audio.hpp"
#include "paper/audio/bank.hpp"
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
SoundBank loadBank(const std::filesystem::path& root, std::span<const SoundDefinition> definitions,
                   bool strict = false) {
    constexpr size_t maximumBankBytes = 256u * 1024u * 1024u;
    size_t loadedBytes = 0;
    SoundBank bank(definitions.size());
    for (size_t id = 0; id < definitions.size(); ++id) {
        const auto& definition = definitions[id];
        for (unsigned variant = 1; variant <= definition.variants; ++variant) {
            const auto path =
                root / "audio" /
                (std::string(definition.file) + "-" + std::to_string(variant) + ".wav");
            auto failure = [&](const char* reason) {
                if (strict)
                    throw std::invalid_argument(std::string(reason) + ": " + path.string());
            };
            if (strict) {
                const auto audioRoot = std::filesystem::canonical(root / "audio");
                const auto canonical = std::filesystem::canonical(path);
                const auto relative = canonical.lexically_relative(audioRoot);
                if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." ||
                    std::filesystem::file_size(canonical) > maximumSourceBytes)
                    failure("Invalid audio path/size");
            }
            SDL_AudioSpec source{};
            Uint8* bytes = nullptr;
            Uint32 length = 0;
            if (!SDL_LoadWAV(path.string().c_str(), &source, &bytes, &length)) {
                SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Missing sound %s: %s", path.string().c_str(),
                            SDL_GetError());
                failure("Invalid or missing WAV");
                continue;
            }
            sdl::Owner<Uint8, SDL_free> sourceData(bytes);
            if (length > maximumSourceBytes) {
                SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Sound too large: %s", path.string().c_str());
                failure("Invalid or missing WAV");
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
                failure("Invalid or missing WAV");
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
                failure("Invalid or missing WAV");
                continue;
            }
            // Replacements retain quieter mastering; unexpectedly loud files cannot spike.
            if (peak > loadedPeakCeiling)
                for (float& value : samples)
                    value *= loadedPeakCeiling / peak;
            if (samples.size() * sizeof(float) > maximumBankBytes - loadedBytes)
                throw std::invalid_argument("Decoded audio bank exceeds PCM budget");
            loadedBytes += samples.size() * sizeof(float);
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
    auto bank = loadBank(assets, definitions);
    if (std::all_of(bank.begin(), bank.end(),
                    [](const auto& variants) { return variants.empty(); })) {
        stream_.reset();
        mixer_.reset();
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "No usable audio assets; audio disabled");
        return;
    }
    openMixer(std::make_unique<AudioMixer>(definitions, std::move(bank)));
}
void Audio::openMixer(std::unique_ptr<AudioMixer> mixer) {
    stream_.reset();
    mixer_ = std::move(mixer);
    mixer_->setMuted(muted());
    mixer_->setMix(master_, volumes_);
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
SoundBank loadSoundBank(const std::filesystem::path& assets, const AudioBankDefinition& bank) {
    validateAudioBank(bank);
    return loadBank(assets, bank.sounds, true);
}
void Audio::openBank(const std::filesystem::path& assets, const AudioBankDefinition& bank) {
    auto pcm = loadSoundBank(assets, bank);
    openMixer(std::make_unique<AudioMixer>(bank.sounds, std::move(pcm)));
}
void Audio::replaceBank(const std::filesystem::path& assets, const AudioBankDefinition& bank) {
    auto candidate = std::make_unique<AudioMixer>(bank.sounds, loadSoundBank(assets, bank));
    candidate->setMuted(muted());
    candidate->setMix(master_, volumes_);
    if (stream_) {
        StreamLock lock(stream_.get());
        if (!lock)
            throw std::runtime_error("Cannot lock audio stream for bank replacement");
        mixer_.swap(candidate);
    } else
        mixer_.swap(candidate);
    // Destroy the previous PCM/mixer after releasing the stream lock.
}
void Audio::apply(const AudioFrame& frame) noexcept {
    if (callbackFailed_) {
        SDL_LogWarn(SDL_LOG_CATEGORY_AUDIO, "Audio callback could not queue PCM; audio disabled");
        stream_.reset();
        return;
    }
    if (StreamLock lock(stream_.get()); lock) {
        mixer_->setScene(frame.scene);
        for (size_t slot = 0; slot < frame.loops.size(); ++slot)
            mixer_->loop(slot, frame.loops[slot].sound, frame.loops[slot].placement);
    }
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
    master_ = master;
    volumes_ = {ambience, effects, interfaceVolume, voices};
    if (StreamLock lock(stream_.get()); lock)
        mixer_->setMix(master_, volumes_);
}
} // namespace paper
