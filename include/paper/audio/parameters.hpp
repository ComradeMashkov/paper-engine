#pragma once
#include <cstddef>

namespace paper::audioParameters {
inline constexpr std::size_t maximumBarriers = 48;
inline constexpr std::size_t echoLeftFrames = 1601, echoRightFrames = 2137;
inline constexpr std::size_t loopFadeFrames = 4096, loopFadeDivisor = 4;
inline constexpr unsigned maximumSourceBytes = 32u * 1024u * 1024u;
inline constexpr int maximumSourceSeconds = 60;
inline constexpr int channels = 2, callbackFrames = 512, minimumSampleFrames = 2;
inline constexpr float defaultRangeMeters = 12, defaultSpatialReverbSend = .13f;
inline constexpr float maximumCooldownSeconds = 60, loadedPeakCeiling = .55f;
inline constexpr float panLimit = .92f;
inline constexpr float listenerClearanceMeters = .12f, sourceClearanceMeters = .18f;
inline constexpr float obstructionPerWall = .55f, obstructionGainLoss = .76f;
inline constexpr float rangeFadeRatio = .3f, attenuationPerMeterSquared = .14f;
inline constexpr unsigned duplicateVoiceLimit = 1, duplicateEffectLimit = 3;
inline constexpr unsigned foregroundPriority = 5;
inline constexpr std::size_t reservedForegroundVoices = 8;
inline constexpr float minimumPitch = .99f, pitchVariation = .02f;
inline constexpr float minimumVariationGain = .94f, variationGainRange = .06f;
inline constexpr float openLowPassHertz = 12000, obstructionLowPassReductionHertz = 11350;
inline constexpr float speakingThresholdGain = .015f, silenceThresholdGain = .00001f;
inline constexpr float controlSlewSeconds = .02f, spatialSlewSeconds = .012f;
inline constexpr float pauseWorldGain = .12f, pauseWorldSlewSeconds = .18f,
                       pauseEventSlewSeconds = .05f;
inline constexpr float duckedGain = .64f, duckAttackSeconds = .12f, duckReleaseSeconds = 1.2f;
inline constexpr float loopCrossfadeSeconds = 1.5f, eventAttackSeconds = .002f;
// The device clock is continuous. Correct only real timeline discontinuities,
// not the sub-frame jitter between the game thread and audio callbacks.
inline constexpr double timelineDriftSeconds = .08;
inline constexpr float echoFeedbackGain = .27f, dcRemovalPerSample = .0014f;
inline constexpr float limiterCeiling = .72f, limiterReleaseSeconds = .25f;
} // namespace paper::audioParameters
