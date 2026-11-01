// SPDX-License-Identifier: Apache-2.0
//
// Theme: one resolved, validated FrostIDE theme (every registry colour, metric and icon role
// with a value), as decoded from a compiled theme blob (theme_blob.hpp) at start-up
// (ADR 0015 decision 4). ThemeData is the writable form the .theme.json compiler fills in.
// Qt-free.
#pragma once

#include "frost/core/error.hpp"
#include "frost/ui/model/rgba.hpp"
#include "frost/ui/model/theme_registry.hpp"
#include "frost/ui/model/ui_scale.hpp"

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace frost::ui {

// The longest theme display name, in UTF-8 bytes.
inline constexpr std::size_t kMaxThemeNameBytes = 64;

/*
 * Every value of a theme, indexed by key. compactMetrics holds the compact-mode values; for
 * keys without hasCompact it must equal metrics. A plain aggregate: no invariants until
 * Theme::fromData checks it.
 */
struct ThemeData {
    std::string id;   // [a-z0-9][a-z0-9-]{0,63}, the file name stem ("glacier-dark").
    std::string name; // Display name, valid UTF-8, 1..kMaxThemeNameBytes bytes.
    bool dark = false;
    bool highContrast = false;
    std::array<Rgba, kColorKeyCount> colors{};
    std::array<float, kMetricKeyCount> metrics{};
    std::array<float, kMetricKeyCount> compactMetrics{};
    std::array<Rgba, kIconRoleCount> iconColors{};

    friend bool operator==(const ThemeData&, const ThemeData&) = default;
};

// The summary listInstalledThemes() returns for the theme picker.
struct ThemeInfo {
    std::string id, name;
    bool dark = false, highContrast = false;

    friend bool operator==(const ThemeInfo&, const ThemeInfo&) = default;
};

// True when `id` matches [a-z0-9][a-z0-9-]{0,63}. Pure, O(n).
[[nodiscard]] bool isValidThemeId(std::string_view id) noexcept;

/*
 * An immutable theme value: 456 bytes of arrays plus the id and name strings. Concurrent
 * const access from any number of threads is safe. Copyable; FrostStyle and AppearanceManager
 * share one through std::shared_ptr<const Theme>.
 */
class Theme {
public:
    /*
     * Validates `data` with exactly the decoder's rules and wraps a copy: a valid id; a name
     * that is valid UTF-8 of 1..64 bytes; every metric finite and inside its registry range,
     * normal and compact; compact equal to normal for keys without hasCompact and not above
     * normal for Px keys that have one; alpha 255 for every opaque colour key.
     * Errors: InvalidArgument naming the first offending field. O(keys).
     */
    [[nodiscard]] static Result<Theme> fromData(const ThemeData& data);

    // The colour of `key`. O(1).
    [[nodiscard]] Rgba color(ColorKey key) const noexcept { return m_data.colors[static_cast<std::size_t>(key)]; }

    // The metric `key` in `density` (compact equals normal for keys without hasCompact). O(1).
    [[nodiscard]] float metric(MetricKey key, Density density = Density::Normal) const noexcept {
        const auto index = static_cast<std::size_t>(key);
        return density == Density::Compact ? m_data.compactMetrics[index] : m_data.metrics[index];
    }

    // The tint of icon role `role`. O(1).
    [[nodiscard]] Rgba iconColor(IconRole role) const noexcept {
        return m_data.iconColors[static_cast<std::size_t>(role)];
    }

    [[nodiscard]] std::string_view id() const noexcept { return m_data.id; }
    [[nodiscard]] std::string_view name() const noexcept { return m_data.name; }
    [[nodiscard]] bool isDark() const noexcept { return m_data.dark; }
    [[nodiscard]] bool isHighContrast() const noexcept { return m_data.highContrast; }

    // The validated values (for re-encoding and tests).
    [[nodiscard]] const ThemeData& data() const noexcept { return m_data; }

    friend bool operator==(const Theme& a, const Theme& b) { return a.m_data == b.m_data; }

private:
    explicit Theme(ThemeData data) : m_data(std::move(data)) {}

    ThemeData m_data;
};

} // namespace frost::ui
