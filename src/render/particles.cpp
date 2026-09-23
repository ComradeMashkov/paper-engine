#include "paper/render/particles.hpp"
#include "paper/core/random.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace paper {
namespace {
bool finite(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool unit(float value) {
    return std::isfinite(value) && value >= 0 && value <= 1;
}
bool color(Vec3 value) {
    return unit(value.x) && unit(value.y) && unit(value.z);
}
bool validStyle(ParticleStyle style) {
    return style == ParticleStyle::Soft || style == ParticleStyle::Faceted;
}
} // namespace
bool validEmitter(const SmokeEmitter& e) {
    return std::isfinite(e.ratePerSecond) && e.ratePerSecond > 0 &&
           e.ratePerSecond <= particleLimits::maximumRate && std::isfinite(e.lifeSeconds) &&
           e.lifeSeconds > 0 && e.lifeSeconds <= particleLimits::maximumLifeSeconds &&
           std::ceil(e.ratePerSecond * e.lifeSeconds) + 1 <= particleLimits::perEmitter &&
           std::isfinite(e.radiusMeters) && e.radiusMeters > 0 && std::isfinite(e.growthMeters) &&
           e.growthMeters >= 0 &&
           e.radiusMeters + e.growthMeters <= particleLimits::maximumRadiusMeters &&
           std::isfinite(e.riseMetersPerSecond) && e.riseMetersPerSecond >= 0 &&
           e.riseMetersPerSecond <= particleLimits::maximumSpeedMetersPerSecond &&
           std::isfinite(e.spreadMeters) && e.spreadMeters >= 0 &&
           e.spreadMeters <= particleLimits::maximumRadiusMeters && unit(e.opacity) &&
           color(e.color) && finite(e.velocityMetersPerSecond) &&
           length(e.velocityMetersPerSecond) <= particleLimits::maximumSpeedMetersPerSecond &&
           validStyle(e.style);
}
bool validParticle(const Particle& p) {
    return finite(p.position) && color(p.color) && unit(p.opacity) &&
           std::isfinite(p.radiusMeters) && p.radiusMeters > 0 &&
           p.radiusMeters <= particleLimits::maximumRadiusMeters && validStyle(p.style) &&
           std::isfinite(p.rotationRadians);
}
std::vector<Particle> smokeParticles(const SmokeEmitter& e, Vec3 origin, double elapsed,
                                     double duration, std::uint32_t seed) {
    if (!validEmitter(e) || !finite(origin) || !std::isfinite(elapsed) || elapsed < 0 ||
        !std::isfinite(duration) || duration < e.lifeSeconds ||
        duration > particleLimits::maximumTimelineSeconds)
        throw std::invalid_argument("Invalid smoke emitter/time");
    std::vector<Particle> result;
    if (elapsed >= duration)
        return result;
    const auto last = static_cast<std::int64_t>(
        std::floor(std::min(elapsed, duration - e.lifeSeconds) * e.ratePerSecond));
    const auto first = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(std::floor((elapsed - e.lifeSeconds) * e.ratePerSecond)) + 1);
    for (auto index = first; index <= last; ++index) {
        const double age = elapsed - static_cast<double>(index) / e.ratePerSecond;
        const float fraction = static_cast<float>(std::clamp(age / e.lifeSeconds, 0.0, 1.0));
        auto random = seed ^ (static_cast<std::uint32_t>(index) * lcgMultiplier);
        const auto signedRatio = [&] {
            return randomRatio(random) * 2 - 1;
        }; // numbers: convert unit ratio to signed displacement.
        const float x = signedRatio(), z = signedRatio();
        const Vec3 offset{x * e.spreadMeters * fraction,
                          static_cast<float>(age) * e.riseMetersPerSecond,
                          z * e.spreadMeters * fraction};
        const float envelope =
            4 * fraction * (1 - fraction); // numbers: unit parabola, zero at birth/death, peak one.
        const auto opacity = e.opacity * envelope;
        if (opacity > 0) {
            result.push_back({origin + offset + e.velocityMetersPerSecond * static_cast<float>(age),
                              e.color, e.radiusMeters + e.growthMeters * fraction, opacity});
            auto& particle = result.back();
            particle.style = e.style;
            constexpr float turnRadians = 2 * std::numbers::pi_v<float>;
            particle.rotationRadians = randomRatio(random) * turnRadians + fraction;
        }
    }
    return result;
}
std::vector<Particle> sortedParticles(std::span<const Particle> particles, const Camera& camera) {
    if (particles.size() > particleLimits::perFrame || !finite(camera.position) ||
        !finite(camera.forward()) || !std::ranges::all_of(particles, validParticle))
        throw std::invalid_argument("Invalid particle frame");
    std::vector<Particle> result(particles.begin(), particles.end());
    std::stable_sort(result.begin(), result.end(), [&](const Particle& a, const Particle& b) {
        return dot(a.position - camera.position, camera.forward()) >
               dot(b.position - camera.position, camera.forward());
    });
    return result;
}
} // namespace paper
