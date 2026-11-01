// SPDX-License-Identifier: Apache-2.0
//
// UiScale: compact mode (density) and IDE zoom, the two user scales FrostStyle applies on top
// of Qt's device pixel ratio (ADR 0015 decision 12; matrix row 244). Qt-free and pure.
//
/*
 * Compact mode changes metrics only (row heights, paddings, island arc and gap, layout
 * margins and spacing, the toolbar icon size), never fonts. Zoom multiplies every logical
 * metric, the UI font size and icon logical sizes. Both are independent of Qt's own
 * QT_SCALE_FACTOR, which Qt reads only at start-up; they change at run time through
 * AppearanceManager::apply().
 */
#pragma once

#include "frost/ui/model/theme_registry.hpp"

#include <array>
#include <cstdint>

namespace frost::ui {

class Theme;

enum class Density : std::uint8_t { Normal, Compact };

// The zoom range accepted by validate() (appearance_settings.hpp), in percent.
inline constexpr std::uint16_t kMinZoomPercent = 50;
inline constexpr std::uint16_t kMaxZoomPercent = 300;
// The steps Zoom In / Zoom Out move through, ascending.
inline constexpr std::array<std::uint16_t, 8> kZoomSteps{80, 90, 100, 110, 125, 150, 175, 200};

struct UiScale {
    Density density = Density::Normal;
    std::uint16_t zoomPercent = 100;

    // zoomPercent / 100.
    [[nodiscard]] constexpr double factor() const noexcept { return zoomPercent / 100.0; }

    // `logical` logical pixels times factor(), rounded half away from zero, never below 0.
    // Monotonic in `logical` and in zoomPercent. O(1).
    [[nodiscard]] int px(double logical) const noexcept;

    // px(theme.metric(key, density)): a Px metric in zoomed logical pixels. O(1).
    [[nodiscard]] int metricPx(const Theme& theme, MetricKey key) const noexcept;

    // theme.metric(key, density) * factor(), unrounded (font sizes). O(1).
    [[nodiscard]] double fontPx(const Theme& theme, MetricKey key) const noexcept;

    friend constexpr bool operator==(UiScale, UiScale) noexcept = default;
};

// The smallest kZoomSteps entry above `zoom`, or `zoom` itself when there is none
// (nextZoomStep(250) == 250, nextZoomStep(60) == 80). O(1).
[[nodiscard]] constexpr std::uint16_t nextZoomStep(std::uint16_t zoom) noexcept {
    for (const std::uint16_t step : kZoomSteps) {
        if (step > zoom) {
            return step;
        }
    }
    return zoom;
}

// The largest kZoomSteps entry below `zoom`, or `zoom` itself when there is none
// (previousZoomStep(250) == 200, previousZoomStep(60) == 60). O(1).
[[nodiscard]] constexpr std::uint16_t previousZoomStep(std::uint16_t zoom) noexcept {
    std::uint16_t result = zoom;
    for (const std::uint16_t step : kZoomSteps) {
        if (step < zoom) {
            result = step;
        }
    }
    return result;
}

} // namespace frost::ui
