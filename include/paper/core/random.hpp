#pragma once
#include <cstdint>

namespace paper {
// Numerical Recipes 32-bit LCG. Keep the recurrence and high-24-bit conversion stable:
// recorded audio variation and paper distress depend on this sequence.
inline constexpr std::uint32_t lcgMultiplier = 1664525u, lcgIncrement = 1013904223u;
inline constexpr unsigned randomDiscardBits = 8;
inline constexpr float randomMantissaRange = 16777216.f;
inline std::uint32_t nextRandom(std::uint32_t& state) noexcept {
    state = state * lcgMultiplier + lcgIncrement;
    return state;
}
inline float randomRatio(std::uint32_t& state) noexcept {
    return static_cast<float>(nextRandom(state) >> randomDiscardBits) / randomMantissaRange;
}
} // namespace paper
