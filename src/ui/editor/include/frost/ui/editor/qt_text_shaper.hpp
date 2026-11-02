// SPDX-License-Identifier: Apache-2.0
//
// QtTextShaper: the Qt implementation of frost::text::ITextShaper (ADR 0026's seam; ADR 0037), which
// measures the editor font for the display map's soft wrap, hit testing and caret positions, plus
// the conversions between shaper units (1/64 logical pixel) and pixels.
#pragma once

#include "frost/text/text_shaper.hpp"

#include <QFont>
#include <QFontMetricsF>

#include <array>
#include <cstdint>
#include <unordered_map>

namespace frost::ui {

// Shaper units per logical pixel: 26.6 fixed point, as QFixed (text_shaper.hpp).
inline constexpr text::TextWidth kShaperUnitsPerPixel = 64;

// A shaper distance in logical pixels. O(1).
[[nodiscard]] constexpr double toPixels(text::TextWidth width) noexcept {
    return static_cast<double>(width) / static_cast<double>(kShaperUnitsPerPixel);
}
// A logical pixel distance in shaper units, rounded to nearest; negative input gives 0. O(1).
[[nodiscard]] text::TextWidth toShaperUnits(double pixels) noexcept;

/*
 * Measures `font` one code point at a time:
 *   - a tab advances to the next multiple of tabSize cells after x;
 *   - other control characters (below U+0020, U+007F) take one cell (they draw nothing);
 *   - Zero-class code points (text::charWidth) advance 0, so combining marks and joiners stay
 *     with the character before them;
 *   - everything else takes QFontMetricsF::horizontalAdvance of the code point (font fallback
 *     included), rounded to 1/64 px; a Wide character never takes less than two cells.
 * ASCII advances are measured once at construction; other code points are measured on first use
 * and cached, so the same code point always gives the same advance (the ITextShaper contract).
 * Kerning plays no part (each code point is measured alone), and the view disables it in the
 * font it paints with, so painted glyphs line up with these advances for monospace text.
 *
 * Not thread-safe: the cache is mutated by the const advance(); the GUI thread only (the display
 * map's owner). A new font or tab size means a new shaper (DisplayMap::setShaper).
 */
class QtTextShaper final : public text::ITextShaper {
public:
    // Precondition: tabSize >= 1 (values below count as 1).
    QtTextShaper(const QFont& font, std::uint32_t tabSize);

    [[nodiscard]] text::TextWidth cellWidth() const noexcept override { return m_cell; }
    [[nodiscard]] text::TextWidth advance(char32_t cp, text::TextWidth x) const noexcept override;

    [[nodiscard]] const QFont& font() const noexcept { return m_font; }
    [[nodiscard]] std::uint32_t tabSize() const noexcept { return m_tabSize; }
    // The font's ascent and height in logical pixels.
    [[nodiscard]] double ascent() const noexcept { return m_metrics.ascent(); }
    [[nodiscard]] double height() const noexcept { return m_metrics.height(); }

private:
    [[nodiscard]] text::TextWidth measure(char32_t cp) const noexcept;

    QFont m_font;
    QFontMetricsF m_metrics;
    std::uint32_t m_tabSize;
    text::TextWidth m_cell = 1;
    std::array<text::TextWidth, 128> m_ascii{};
    mutable std::unordered_map<char32_t, text::TextWidth> m_cache;
};

} // namespace frost::ui
