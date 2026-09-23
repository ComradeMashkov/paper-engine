#include "paper/render/particles.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace paper;
namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
bool same(const std::vector<Particle>& a, const std::vector<Particle>& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].position.x != b[i].position.x || a[i].position.y != b[i].position.y ||
            a[i].position.z != b[i].position.z || a[i].radiusMeters != b[i].radiusMeters ||
            a[i].opacity != b[i].opacity || a[i].style != b[i].style ||
            a[i].rotationRadians != b[i].rotationRadians)
            return false;
    return true;
}
} // namespace
int main() {
    try {
        SmokeEmitter emitter{8, 1, .04f, .08f, .2f, .1f, .3f, {.5f, .5f, .5f}};
        check(validEmitter(emitter), "bounded smoke emitter");
        const auto sampled = smokeParticles(emitter, {1, 2, 3}, .5, 2, 42);
        check(sampled.size() == 4 && std::abs(sampled.front().position.y - 2.1f) < .00001f &&
                  sampled.front().opacity == .3f && sampled.front().radiusMeters == .08f,
              "emission rate, rise, size and fade are sampled in physical units");
        (void)smokeParticles(emitter, {1, 2, 3}, .1, 2, 42);
        check(same(sampled, smokeParticles(emitter, {1, 2, 3}, .5, 2, 42)),
              "sampling depends only on saved time and stable identity");
        check(!same(sampled, smokeParticles(emitter, {1, 2, 3}, .5, 2, 43)),
              "independent emitters do not share displacement");
        emitter.style = ParticleStyle::Faceted;
        const auto faceted = smokeParticles(emitter, {1, 2, 3}, .5, 2, 42);
        check(faceted.front().style == ParticleStyle::Faceted &&
                  faceted.front().rotationRadians != faceted.back().rotationRadians &&
                  same(faceted, smokeParticles(emitter, {1, 2, 3}, .5, 2, 42)),
              "faceted particles vary orientation and restore exactly from saved time");
        check(smokeParticles(emitter, {}, 1.75, 2, 42).size() == 2 &&
                  smokeParticles(emitter, {}, 2, 2, 42).empty(),
              "emission stops early enough for the final particles to expire");
        emitter.ratePerSecond = 128;
        emitter.lifeSeconds = 30;
        check(!validEmitter(emitter), "particle budget rejects dense emitter before sampling");
        bool rejected = false;
        try {
            (void)smokeParticles(emitter, {}, .1, 40, 1);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "invalid emitter cannot allocate an unbounded frame");
        std::vector<Particle> particles{{{0, 0, 1}, {1, 0, 0}, .2f, .5f},
                                        {{0, 0, 3}, {0, 1, 0}, .2f, .5f},
                                        {{0, 0, 3}, {0, 0, 1}, .2f, .5f}};
        const auto sorted = sortedParticles(particles, Camera{});
        check(sorted.front().position.z == 3 && sorted.front().color.y == 1 &&
                  sorted.back().position.z == 1,
              "transparent particles sort far to near with stable ties");
        particles.front().opacity = std::numeric_limits<float>::quiet_NaN();
        rejected = false;
        try {
            (void)sortedParticles(particles, Camera{});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected, "non-finite particle rejected before GPU encoding");
        std::cout << "Analytical smoke and particle ordering passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
