// SPDX-License-Identifier: Apache-2.0
//
// SplitMix64: a tiny, fast, seedable 64-bit pseudo-random generator, used for
// steal-victim selection in ThreadPool and for the seeded choices of
// SimExecutor. Algorithm from G. L. Steele Jr., D. Lea and C. H. Flood, "Fast
// Splittable Pseudorandom Number Generators" (OOPSLA 2014); the constants are
// those of Sebastiano Vigna's public-domain (CC0) splitmix64.c reference.
//
// It is implemented here rather than taken from <random> because the standard
// distributions are implementation-defined: SimExecutor traces must be
// identical for a seed on MSVC STL and libstdc++.
#pragma once

#include <cstdint>

namespace frost::sched::detail {

// Not thread-safe: each owner keeps its own generator. Every operation is O(1).
class SplitMix64 {
public:
    constexpr explicit SplitMix64(std::uint64_t seed) noexcept : m_state(seed) {}

    // The next 64 pseudo-random bits.
    [[nodiscard]] constexpr std::uint64_t next() noexcept {
        m_state += 0x9E37'79B9'7F4A'7C15ULL;
        std::uint64_t z = m_state;
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31);
    }

    // A value in [0, bound) by reduction modulo bound. The bias is below
    // bound / 2^64, irrelevant for scheduling. Precondition: bound > 0.
    [[nodiscard]] constexpr std::uint64_t below(std::uint64_t bound) noexcept { return next() % bound; }

private:
    std::uint64_t m_state;
};

} // namespace frost::sched::detail
