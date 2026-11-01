// SPDX-License-Identifier: Apache-2.0
//
// WCAG contrast arithmetic and FrostIDE's contrast policy for themes (ADR 0015 decision 6):
// relative luminance, contrast ratio, the minimum ratio for each kind of contrast pair, and
// the contrast of a registry pair in a theme with translucent colours composited. Qt-free.
//
/*
 * Definitions from the W3C Web Content Accessibility Guidelines 2.2, "Relative luminance" and
 * "Contrast ratio" (https://www.w3.org/TR/WCAG22/#dfn-relative-luminance and
 * #dfn-contrast-ratio): each sRGB channel c in [0, 1] is linearised as c / 12.92 when
 * c <= 0.04045, else ((c + 0.055) / 1.055)^2.4; L = 0.2126 R + 0.7152 G + 0.0722 B; the
 * contrast of two colours is (L1 + 0.05) / (L2 + 0.05) with L1 the lighter, in [1, 21].
 * (WCAG 2.2 prints 0.04045; WCAG 2.0 printed 0.03928. No 8-bit channel value lies between
 * the two (10/255 = 0.0392, 11/255 = 0.0431), so both give the same result here.)
 *
 * All functions are pure, thread-safe and O(1).
 */
#pragma once

#include "frost/ui/model/rgba.hpp"
#include "frost/ui/model/theme_registry.hpp"

#include <optional>

namespace frost::ui {

class Theme;

// The WCAG relative luminance of an opaque colour (alpha is ignored), in [0, 1].
[[nodiscard]] double relativeLuminance(Rgba opaque) noexcept;

// The WCAG contrast ratio of two opaque colours: symmetric, in [1, 21].
[[nodiscard]] double contrastRatio(Rgba a, Rgba b) noexcept;

/*
 * The minimum ratio a pair of `kind` must reach, or nullopt when it has none:
 *   IslandSeparation  1.20 in every theme (PLAN.md section 2, "Theming");
 *   Text, SecondaryText  4.5 (WCAG AA, 1.4.3) in normal themes, 7.0 (AAA, 1.4.6) in High Contrast;
 *   DisabledText  none in normal themes (WCAG exempts inactive components), 4.5 in High Contrast;
 *   NonText  3.0 in every theme (WCAG 1.4.11, non-text contrast);
 *   Border  none in normal themes, 3.0 in High Contrast.
 */
[[nodiscard]] std::optional<double> minimumContrast(ContrastKind kind, bool highContrastTheme) noexcept;

// The contrast of `pair` in `theme`: a translucent background is first composited over its
// underlay, then a translucent foreground over the (now opaque) background.
[[nodiscard]] double pairContrast(const Theme& theme, const ContrastPair& pair) noexcept;

// The same for an icon role's tint against its background.
[[nodiscard]] double pairContrast(const Theme& theme, const IconContrastPair& pair) noexcept;

} // namespace frost::ui
