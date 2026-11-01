// SPDX-License-Identifier: Apache-2.0
//
// WCAG 2.2 relative luminance and contrast ratio, and the theme contrast policy
// (frost/ui/model/contrast.hpp; the formulas are cited there).

#include "frost/ui/model/contrast.hpp"

#include "frost/ui/model/theme.hpp"

#include <algorithm>
#include <cmath>

namespace frost::ui {

namespace {

// sRGB channel (0..255) to linear light, WCAG 2.2 "relative luminance".
double linearChannel(std::uint8_t value) noexcept {
    const double c = value / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

} // namespace

double relativeLuminance(Rgba opaque) noexcept {
    return 0.2126 * linearChannel(opaque.r()) + 0.7152 * linearChannel(opaque.g()) + 0.0722 * linearChannel(opaque.b());
}

double contrastRatio(Rgba a, Rgba b) noexcept {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

std::optional<double> minimumContrast(ContrastKind kind, bool highContrastTheme) noexcept {
    switch (kind) {
    case ContrastKind::IslandSeparation: return 1.20;
    case ContrastKind::Text:
    case ContrastKind::SecondaryText: return highContrastTheme ? 7.0 : 4.5;
    case ContrastKind::DisabledText: return highContrastTheme ? std::optional<double>(4.5) : std::nullopt;
    case ContrastKind::NonText: return 3.0;
    case ContrastKind::Border: return highContrastTheme ? std::optional<double>(3.0) : std::nullopt;
    }
    return std::nullopt;
}

namespace {

double contrastOver(const Theme& theme, Rgba foreground, ColorKey background, ColorKey underlay) noexcept {
    Rgba surface = theme.color(background);
    if (!surface.opaque()) {
        // The registry guarantees an opaque underlay for every background that may be translucent.
        surface = compositeOver(surface, underlay != ColorKey::kCount ? theme.color(underlay) : Rgba{0xff000000U});
    }
    return contrastRatio(compositeOver(foreground, surface), surface);
}

} // namespace

double pairContrast(const Theme& theme, const ContrastPair& pair) noexcept {
    return contrastOver(theme, theme.color(pair.foreground), pair.background, pair.underlay);
}

double pairContrast(const Theme& theme, const IconContrastPair& pair) noexcept {
    return contrastOver(theme, theme.iconColor(pair.role), pair.background, pair.underlay);
}

} // namespace frost::ui
