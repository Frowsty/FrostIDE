// SPDX-License-Identifier: Apache-2.0
//
// Helpers for the ui_model tests: a SplitMix64 generator for seeded property tests, a valid
// ThemeData builder, a random valid ThemeData generator and small byte helpers. Header-only,
// Qt-free.
#pragma once

#include "frost/ui/model/theme.hpp"
#include "frost/ui/model/theme_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::ui::test {

/*
 * SplitMix64 (Steele, Lea and Flood, "Fast splittable pseudorandom number generators",
 * OOPSLA 2014; the constants are those of the public-domain reference implementation by
 * Sebastiano Vigna, https://prng.di.unimi.it/splitmix64.c). Deterministic for a seed.
 */
class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) noexcept : m_state(seed) {}

    std::uint64_t next() noexcept {
        std::uint64_t z = (m_state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    // Uniform in [0, bound) for bound > 0 (modulo bias is irrelevant for tests).
    std::uint64_t below(std::uint64_t bound) noexcept { return next() % bound; }

    // Uniform in [lo, hi].
    double uniform(double lo, double hi) noexcept {
        return lo + (hi - lo) * static_cast<double>(next() >> 11) / static_cast<double>(1ULL << 53);
    }

    bool coin() noexcept { return (next() & 1U) != 0; }

private:
    std::uint64_t m_state;
};

// A valid ThemeData: every colour opaque mid-grey, every metric at its minimum (compact equal),
// icon roles black.
inline ThemeData sampleThemeData(std::string id = "sample-theme", std::string name = "Sample Theme") {
    ThemeData data;
    data.id = std::move(id);
    data.name = std::move(name);
    data.dark = true;
    for (Rgba& c : data.colors) {
        c = Rgba{0xff808080U};
    }
    for (const MetricKeyInfo& key : kMetricKeys) {
        const auto i = static_cast<std::size_t>(key.key);
        data.metrics[i] = key.min;
        data.compactMetrics[i] = key.min;
    }
    for (Rgba& c : data.iconColors) {
        c = Rgba{0xff000000U};
    }
    return data;
}

// A random ThemeData that passes Theme::fromData: random id and UTF-8 name (with multi-byte
// characters), random colours (opaque where required), metrics inside their ranges with
// compact <= normal where allowed.
inline ThemeData randomThemeData(SplitMix64& rng) {
    static constexpr std::string_view kIdChars = "abcdefghijklmnopqrstuvwxyz0123456789-";
    static constexpr std::string_view kNamePieces[] = {"Glacier", " ", "\xc3\xa9", "\xe2\x9d\x84", "Dark", "7"};
    ThemeData data;
    const std::size_t idLength = 1 + rng.below(64);
    data.id.push_back(kIdChars[rng.below(36)]);
    while (data.id.size() < idLength) {
        data.id.push_back(kIdChars[rng.below(kIdChars.size())]);
    }
    while (data.name.empty() || rng.below(4) != 0) {
        const std::string_view piece = kNamePieces[rng.below(std::size(kNamePieces))];
        if (data.name.size() + piece.size() > kMaxThemeNameBytes) {
            break;
        }
        data.name += piece;
    }
    data.dark = rng.coin();
    data.highContrast = rng.coin();
    for (const ColorKeyInfo& key : kColorKeys) {
        auto argb = static_cast<std::uint32_t>(rng.next());
        if (key.opaque) {
            argb |= 0xff000000U;
        }
        data.colors[static_cast<std::size_t>(key.key)] = Rgba{argb};
    }
    for (const MetricKeyInfo& key : kMetricKeys) {
        const auto i = static_cast<std::size_t>(key.key);
        const auto normal = static_cast<float>(rng.uniform(key.min, key.max));
        data.metrics[i] = normal;
        data.compactMetrics[i] =
            key.hasCompact ? static_cast<float>(rng.uniform(key.min, key.unit == MetricUnit::Px ? normal : key.max))
                           : normal;
        if (data.compactMetrics[i] > normal && key.unit == MetricUnit::Px) {
            data.compactMetrics[i] = normal; // Float rounding of the upper bound.
        }
    }
    for (Rgba& c : data.iconColors) {
        c = Rgba{static_cast<std::uint32_t>(rng.next())};
    }
    return data;
}

inline std::span<const std::byte> asBytes(std::string_view text) noexcept { return std::as_bytes(std::span(text)); }

inline std::string toString(std::span<const std::byte> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

} // namespace frost::ui::test
