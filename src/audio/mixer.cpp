#include "paper/audio/mixer.hpp"
#include "paper/core/random.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace paper {
using namespace audioParameters;
namespace {
constexpr size_t index(AudioBus bus) {
    return static_cast<size_t>(bus);
}
float unit(float value) {
    return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f;
}
bool finite(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
} // namespace
SpatialMix spatialMix(const AudioScene& scene, const SoundPlacement& sound) noexcept {
    if (!sound.spatial) {
        const float pan = std::isfinite(sound.pan) ? std::clamp(sound.pan, -1.f, 1.f) : 0.f;
        return {std::sqrt((1 - pan) * .5f), std::sqrt((1 + pan) * .5f),
                0}; // numbers: equal-power stereo uses half the normalized pan range.
    }
    if (!finite(sound.position) || !finite(scene.listener) || !finite(scene.right) ||
        !std::isfinite(sound.rangeMeters) || sound.rangeMeters <= 0)
        return {0, 0, 0};
    const Vec3 delta = sound.position - scene.listener;
    const float distance = length(delta);
    if (distance >= sound.rangeMeters)
        return {0, 0, 0};
    const Vec3 direction = normalized(delta);
    const float pan = std::clamp(dot(direction, scene.right), -panLimit, panLimit);
    float obstruction = 0;
    for (size_t i = 0; i < std::min(scene.barrierCount, scene.barriers.size()); ++i) {
        const float hit = rayBox(scene.listener, direction, scene.barriers[i]);
        // Endpoints can lie on the sounding door or a wall-mounted switch.
        if (hit > listenerClearanceMeters && hit < distance - sourceClearanceMeters)
            obstruction = std::min(1.f, obstruction + obstructionPerWall);
    }
    const float edge = unit((sound.rangeMeters - distance) / (sound.rangeMeters * rangeFadeRatio));
    const float attenuation = edge * edge / (1 + attenuationPerMeterSquared * distance * distance) *
                              (1 - obstructionGainLoss * obstruction);
    return {std::sqrt((1 - pan) * .5f) * attenuation,
            std::sqrt((1 + pan) * .5f) *
                attenuation, // numbers: equal-power stereo uses half the normalized pan range.
            obstruction};
}
AudioMixer::AudioMixer(std::span<const SoundDefinition> definitions, SoundBank bank)
    : definitions_(definitions.begin(), definitions.end()), bank_(std::move(bank)),
      nextPlay_(definitions.size()), lastVariant_(definitions.size(), static_cast<size_t>(-1)) {
    if (definitions_.size() > maximumSounds || bank_.size() != definitions_.size())
        throw std::invalid_argument("Invalid audio bank size");
    std::set<std::string> ids;
    for (const auto& definition : definitions_)
        if (definition.id.empty() || !ids.insert(definition.id).second ||
            index(definition.bus) >= busCount || !std::isfinite(definition.gain) ||
            definition.gain < 0 || definition.gain > 1 ||
            !std::isfinite(definition.cooldownSeconds) || definition.cooldownSeconds < 0 ||
            definition.cooldownSeconds > maximumCooldownSeconds ||
            !std::isfinite(definition.spatialReverbSend) || definition.spatialReverbSend < 0 ||
            definition.spatialReverbSend > 1 || !std::isfinite(definition.nonSpatialReverbSend) ||
            definition.nonSpatialReverbSend < 0 || definition.nonSpatialReverbSend > 1)
            throw std::invalid_argument("Invalid audio definition: " + definition.id);
}
size_t AudioMixer::find(SoundId id) const noexcept {
    for (size_t i = 0; i < definitions_.size(); ++i)
        if (definitions_[i].id == id)
            return i;
    return definitions_.size();
}
float AudioMixer::random() noexcept {
    return randomRatio(seed_);
}
void AudioMixer::setScene(const AudioScene& scene) noexcept {
    if (scene_.active && !scene.active)
        stopWorld();
    scene_ = scene;
    scene_.barrierCount = std::min(scene.barrierCount, scene.barriers.size());
}
void AudioMixer::setMix(float master, const std::array<float, busCount>& volumes) noexcept {
    master_ = unit(master);
    for (size_t i = 0; i < busCount; ++i)
        volumes_[i] = unit(volumes[i]);
}
void AudioMixer::setMuted(bool muted) noexcept {
    if (muted && !muted_)
        for (auto& voice : voices_)
            voice.stopping = true;
    muted_ = muted;
}
bool AudioMixer::play(SoundId effect, SoundPlacement placement) noexcept {
    const size_t id = find(effect);
    if (id >= definitions_.size() || muted_ || !scene_.focused || bank_[id].empty() ||
        definitions_[id].loop || frames_ < nextPlay_[id])
        return false;
    placement.gain = unit(placement.gain);
    if (placement.gain == 0)
        return false;
    Voice* free = nullptr;
    unsigned duplicates = 0;
    for (auto& voice : voices_) {
        if (!voice.sample) {
            if (!free)
                free = &voice;
        } else if (voice.effect == id && !voice.stopping)
            ++duplicates;
    }
    // Reserve eight voices for foreground feedback; ambient events never evict an action.
    if (!free ||
        duplicates >= (definitions_[id].bus == AudioBus::Voices ? duplicateVoiceLimit
                                                                : duplicateEffectLimit) ||
        (definitions_[id].priority < foregroundPriority &&
         activeVoices() >= maxVoices - reservedForegroundVoices))
        return false;
    const size_t count = bank_[id].size();
    size_t variant = static_cast<size_t>(random() * static_cast<float>(count));
    if (count > 1 && variant == lastVariant_[id])
        variant =
            (variant + 1 + static_cast<size_t>(random() * static_cast<float>(count - 1))) % count;
    if (bank_[id][variant].size() < minimumSampleFrames)
        return false;
    lastVariant_[id] = variant;
    *free = {};
    free->sample = &bank_[id][variant];
    free->effect = id;
    free->placement = placement;
    // Preserve each recorded speaker's pitch and formants.
    free->pitch =
        definitions_[id].bus == AudioBus::Voices ? 1.f : minimumPitch + pitchVariation * random();
    const auto spatial = spatialMix(scene_, placement);
    free->left = spatial.left;
    free->right = spatial.right;
    free->variation = minimumVariationGain + variationGainRange * random();
    nextPlay_[id] =
        frames_ + static_cast<std::uint64_t>(definitions_[id].cooldownSeconds * sampleRate);
    return true;
}
void AudioMixer::loop(size_t slot, SoundId effect, SoundPlacement placement) noexcept {
    const size_t id = find(effect);
    if (slot >= loops_.size() || id >= definitions_.size() || !definitions_[id].loop ||
        bank_[id].empty() || bank_[id][0].size() < minimumSampleFrames)
        return;
    auto& voice = loops_[slot];
    if (!voice.sample || voice.effect != id) {
        voice = {};
        voice.effect = id;
        voice.sample = &bank_[id][0];
        voice.cursor =
            random() *
            static_cast<double>(
                voice.sample->size() /
                2); // numbers: random loop offset starts in the first half of the sample.
    }
    placement.gain = unit(placement.gain);
    voice.placement = placement;
}
void AudioMixer::stop(SoundId effect) noexcept {
    const size_t id = find(effect);
    for (auto& voice : voices_)
        if (voice.effect == id)
            voice.stopping = true;
    for (auto& voice : loops_)
        if (voice.effect == id)
            voice.placement.gain = 0;
    // Replacing a dialogue beat must not be suppressed by the previous beat's cooldown.
    if (find(effect) < definitions_.size())
        nextPlay_[find(effect)] = frames_;
}
void AudioMixer::stopWorld() noexcept {
    for (auto& voice : voices_)
        if (voice.sample && definitions_[voice.effect].bus != AudioBus::Interface)
            voice.stopping = true;
    for (auto& voice : loops_)
        voice.placement.gain = 0;
    for (auto& voice : timeline_)
        voice = {};
    timelineBound_.fill(false);
    std::fill(nextPlay_.begin(), nextPlay_.end(), frames_);
}
void AudioMixer::timeline(size_t slot, SoundId effect, SoundPlacement placement, double seconds,
                          bool paused) noexcept {
    if (slot >= timeline_.size())
        return;
    auto& voice = timeline_[slot];
    const auto id = find(effect);
    if (id >= definitions_.size() || !std::isfinite(seconds) || seconds < 0 || bank_[id].empty() ||
        bank_[id][0].size() < minimumSampleFrames) {
        voice = {};
        timelineBound_[slot] = false;
        return;
    }
    const auto& pcm = bank_[id][0];
    const double frames = seconds * sampleRate;
    if (!std::isfinite(frames) ||
        (!definitions_[id].loop && frames >= static_cast<double>(pcm.size()))) {
        voice = {};
        timelineBound_[slot] = false;
        return;
    }
    const bool rebind = !timelineBound_[slot] || voice.effect != id;
    const bool seek = rebind || seconds < timelineSeconds_[slot] || paused || timelinePaused_[slot];
    if (rebind || (seek && !voice.sample)) {
        voice = {};
        voice.effect = id;
        voice.sample = &pcm;
    }
    placement.gain = unit(placement.gain);
    voice.placement = placement;
    const double cursor =
        definitions_[id].loop ? std::fmod(frames, static_cast<double>(pcm.size())) : frames;
    if (seek ||
        (voice.sample && std::abs(voice.cursor - cursor) > timelineDriftSeconds * sampleRate))
        voice.cursor = cursor;
    timelineBound_[slot] = true;
    timelineSeconds_[slot] = seconds;
    timelinePaused_[slot] = paused;
}
AudioMixer::Voice& AudioMixer::voiceAt(size_t index) noexcept {
    if (index < maxVoices)
        return voices_[index];
    index -= maxVoices;
    return index < maxLoops ? loops_[index] : timeline_[index - maxLoops];
}
size_t AudioMixer::activeVoices() const noexcept {
    return static_cast<size_t>(
        std::count_if(voices_.begin(), voices_.end(),
                      [](const Voice& voice) { return voice.sample != nullptr; }));
}
float AudioMixer::sample(Voice& voice, bool looped) noexcept {
    const auto& data = *voice.sample;
    const size_t size = data.size(), at = static_cast<size_t>(voice.cursor);
    const float fraction = static_cast<float>(voice.cursor - static_cast<double>(at));
    const auto read = [&](size_t i) {
        return data[i] + (data[std::min(i + 1, size - 1)] - data[i]) * fraction;
    };
    float value = read(std::min(at, size - 1));
    const size_t fade = std::min<size_t>(loopFadeFrames, size / loopFadeDivisor);
    if (looped && fade > 0 && at >= size - fade) {
        const float blend = static_cast<float>(voice.cursor - static_cast<double>(size - fade)) /
                            static_cast<float>(fade);
        value = value * (1 - blend) + read(at - (size - fade)) * blend;
    }
    voice.cursor += voice.pitch;
    if (voice.cursor >= static_cast<double>(size)) {
        if (looped)
            voice.cursor += static_cast<double>(fade) - static_cast<double>(size);
        else
            voice.sample = nullptr;
    }
    return value;
}
void AudioMixer::render(std::span<float> stereo) noexcept {
    std::fill(stereo.begin(), stereo.end(), 0.f);
    struct Targets {
        float left = 0, right = 0, gain = 0, filter = 1;
    };
    std::array<Targets, maxVoices + maxLoops + maxTimelineVoices> targets{};
    bool speaking = false;
    for (size_t i = 0; i < targets.size(); ++i) {
        auto& voice = voiceAt(i);
        if (!voice.sample)
            continue;
        const auto& definition = definitions_[voice.effect];
        const auto spatial = spatialMix(scene_, voice.placement);
        const float gain =
            voice.stopping ? 0 : definition.gain * voice.placement.gain * voice.variation;
        targets[i] = {
            spatial.left, spatial.right, gain,
            1 - std::exp(
                    -2 *
                    pi3 * // numbers: one-pole low-pass coefficient uses angular frequency, 2 pi Hz.
                    (openLowPassHertz - obstructionLowPassReductionHertz * spatial.obstruction) /
                    sampleRate)};
        // Background whispers must not push the room down and expose themselves.
        const bool pausedTimeline =
            i >= maxVoices + maxLoops && timelinePaused_[i - maxVoices - maxLoops];
        if (definition.bus == AudioBus::Voices && definition.triggersDucking && !voice.stopping &&
            !pausedTimeline &&
            (spatial.left + spatial.right) * gain * volumes_[index(AudioBus::Voices)] >
                speakingThresholdGain)
            speaking = true;
    }
    // 20 ms controls, 12 ms spatial moves, 1.5 s environment crossfades.
    constexpr float controlSlew = 1.f / (sampleRate * controlSlewSeconds),
                    spatialSlew = 1.f / (sampleRate * spatialSlewSeconds);
    for (size_t frame = 0; frame + 1 < stereo.size(); frame += channels) {
        const float targetMaster = muted_ || !scene_.focused ? 0 : master_;
        currentMaster_ += (targetMaster - currentMaster_) * controlSlew;
        if (targetMaster == 0 && currentMaster_ < silenceThresholdGain)
            currentMaster_ = 0;
        const float worldTarget = scene_.paused ? pauseWorldGain : 1.f;
        worldGain_ += (worldTarget - worldGain_) / (sampleRate * pauseWorldSlewSeconds);
        eventGain_ +=
            ((scene_.paused ? 0.f : 1.f) - eventGain_) / (sampleRate * pauseEventSlewSeconds);
        if (scene_.paused && eventGain_ < silenceThresholdGain)
            eventGain_ = 0;
        duck_ += ((speaking ? duckedGain : 1.f) - duck_) /
                 (sampleRate * (speaking ? duckAttackSeconds : duckReleaseSeconds));
        for (size_t b = 0; b < busCount; ++b) {
            currentVolumes_[b] += (volumes_[b] - currentVolumes_[b]) * controlSlew;
            if (volumes_[b] == 0 && currentVolumes_[b] < silenceThresholdGain)
                currentVolumes_[b] = 0;
        }
        float left = 0, right = 0, wetLeft = 0, wetRight = 0;
        for (size_t i = 0; i < targets.size(); ++i) {
            const bool timeline = i >= maxVoices + maxLoops;
            const bool background = i >= maxVoices && !timeline;
            auto& voice = voiceAt(i);
            if (!voice.sample)
                continue;
            const auto& definition = definitions_[voice.effect];
            const bool looped = background || (timeline && definition.loop);
            if (timeline &&
                (timelinePaused_[i - maxVoices - maxLoops] || !scene_.focused || !scene_.active))
                continue;
            const auto bus = definition.bus;
            // Fast attacks preserve recorded impacts; releases stay click-free.
            const float slew = looped           ? 1.f / (sampleRate * loopCrossfadeSeconds)
                               : voice.stopping ? spatialSlew
                                                : 1.f / (sampleRate * eventAttackSeconds);
            voice.level += (targets[i].gain - voice.level) * slew;
            voice.left += (targets[i].left - voice.left) * spatialSlew;
            voice.right += (targets[i].right - voice.right) * spatialSlew;
            voice.filter += (targets[i].filter - voice.filter) * spatialSlew;
            if (voice.stopping && voice.level < silenceThresholdGain) {
                voice.sample = nullptr;
                continue;
            }
            // Freeze foreground events on pause after the mix has faded down.
            if (!looped && scene_.paused && bus != AudioBus::Interface && !voice.stopping &&
                !voice.placement.preview && eventGain_ == 0)
                continue;
            const float raw = sample(voice, looped);
            voice.filtered += voice.filter * (raw - voice.filtered);
            float volume = currentVolumes_[index(bus)] * voice.level;
            if (bus != AudioBus::Interface && !voice.placement.preview)
                volume *= background ? worldGain_ : eventGain_;
            if (bus == AudioBus::Ambience || definition.receivesDucking)
                volume *= duck_;
            const float l = voice.filtered * voice.left * volume;
            const float r = voice.filtered * voice.right * volume;
            left += l;
            right += r;
            if (voice.placement.spatial || definition.nonSpatialReverbSend > 0) {
                const float send = voice.placement.spatial ? definition.spatialReverbSend
                                                           : definition.nonSpatialReverbSend;
                wetLeft += l * send;
                wetRight += r * send;
            }
        }
        const size_t lpos = echoPosition_ % echoLeft_.size(),
                     rpos = echoPosition_ % echoRight_.size();
        const float echoL = echoLeft_[lpos], echoR = echoRight_[rpos];
        echoLeft_[lpos] = wetLeft + echoR * echoFeedbackGain;
        echoRight_[rpos] = wetRight + echoL * echoFeedbackGain;
        ++echoPosition_;
        left += echoL;
        right += echoR;
        dcLeft_ += dcRemovalPerSample * (left - dcLeft_);
        dcRight_ += dcRemovalPerSample * (right - dcRight_);
        left = (left - dcLeft_) * currentMaster_;
        right = (right - dcRight_) * currentMaster_;
        const float peak = std::max(std::abs(left), std::abs(right));
        const float ceiling = peak > limiterCeiling ? limiterCeiling / peak : 1.f;
        limiter_ = ceiling < limiter_
                       ? ceiling
                       : limiter_ + (ceiling - limiter_) / (sampleRate * limiterReleaseSeconds);
        stereo[frame] = left * limiter_;
        stereo[frame + 1] = right * limiter_;
        ++frames_;
    }
}
} // namespace paper
