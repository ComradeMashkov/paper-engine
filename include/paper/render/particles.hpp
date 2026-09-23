#pragma once
#include "paper/core/math3d.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace paper {
namespace particleLimits {
inline constexpr size_t perEmitter = 256, perFrame = 2048;
inline constexpr double maximumRate = 128, maximumLifeSeconds = 30, maximumTimelineSeconds = 86400;
inline constexpr float maximumRadiusMeters = 3, maximumSpeedMetersPerSecond = 10;
} // namespace particleLimits
enum class ParticleStyle { Soft, Faceted };
struct Particle {
    Vec3 position, color;
    float radiusMeters = 0, opacity = 0;
    bool foreground = false;
    ParticleStyle style = ParticleStyle::Soft;
    float rotationRadians = 0;
};
// The author supplies every artistic parameter; the renderer only applies the contract.
struct SmokeEmitter {
    double ratePerSecond = 0, lifeSeconds = 0;
    float radiusMeters = 0, growthMeters = 0, riseMetersPerSecond = 0, spreadMeters = 0,
          opacity = 0;
    Vec3 color;
    Vec3 velocityMetersPerSecond;
    ParticleStyle style = ParticleStyle::Soft;
};
bool validEmitter(const SmokeEmitter& emitter);
bool validParticle(const Particle& particle);
// Analytical sampling is independent of frame subdivision and needs no particle save blob.
std::vector<Particle> smokeParticles(const SmokeEmitter& emitter, Vec3 origin,
                                     double elapsedSeconds, double durationSeconds,
                                     std::uint32_t seed);
std::vector<Particle> sortedParticles(std::span<const Particle> particles, const Camera& camera);
} // namespace paper
