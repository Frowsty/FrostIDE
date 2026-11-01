// SPDX-License-Identifier: Apache-2.0
//
// The shaping seam of the display map (PLAN.md section 2, "Editor rendering"; ADR 0026): ITextShaper
// measures characters for soft wrap, hit testing and caret x positions, and MonospaceTextShaper is
// its deterministic, Qt-free implementation for tests and headless use. The Qt shaper arrives with
// m1/editor-view. Also the display-width classes of code points and a decoder for well-formed UTF-8.
/*
 * Units. TextWidth is a horizontal distance in the shaper's units. The Qt shaper uses 1/64 px
 * (26.6 fixed point, as QFixed and FreeType do), so wrap decisions are exact integer comparisons
 * that come out the same on every run; MonospaceTextShaper uses multiples of its cell width.
 *
 * Every display row lays out from x = 0, soft-wrapped continuation rows included, so a tab
 * advances to the next tab stop measured from the start of its row.
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace frost::text {

// A horizontal distance or x position, in the shaper's units (see above). Never negative.
using TextWidth = std::int64_t;

// How wide a code point displays, in cells of a monospace grid.
enum class CharWidth : std::uint8_t {
    Zero,   // Combining marks, joiners, variation selectors, other invisible format characters.
    Normal, // One cell (also tabs and control characters; the shaper measures tabs itself).
    Wide,   // Two cells: East Asian Wide and Fullwidth characters, emoji with emoji presentation.
};

/*
 * The width class of `cp`. The tables in text_shaper.cpp approximate the Unicode Character
 * Database (UAX #11 East_Asian_Width W and F for Wide; general categories Mn, Me and Cf, and the
 * conjoining Hangul vowels and finals, for Zero), written from the published property values.
 * They cover the common scripts, CJK and emoji, not every assigned code point.
 * TODO(M1): generate the tables from the UCD files pinned for grapheme navigation.
 * Pure and thread-safe; O(log ranges).
 */
[[nodiscard]] CharWidth charWidth(char32_t cp) noexcept;

// One code point decoded from UTF-8, with its encoded length in bytes (1 to 4).
struct DecodedChar {
    char32_t cp = 0;
    std::uint32_t bytes = 1;
};

// Decodes the code point at the start of `utf8`. Precondition: `utf8` starts with a complete,
// well-formed UTF-8 sequence (frost::text's content and its chunks always do). O(1); pure.
[[nodiscard]] constexpr DecodedChar decodeUtf8(std::string_view utf8) noexcept {
    const auto byte = [&](std::size_t i) { return static_cast<char32_t>(static_cast<unsigned char>(utf8[i])); };
    const char32_t b0 = byte(0);
    if (b0 < 0x80) {
        return {b0, 1};
    }
    if (b0 < 0xE0) {
        return {((b0 & 0x1F) << 6) | (byte(1) & 0x3F), 2};
    }
    if (b0 < 0xF0) {
        return {((b0 & 0x0F) << 12) | ((byte(1) & 0x3F) << 6) | (byte(2) & 0x3F), 3};
    }
    return {((b0 & 0x07) << 18) | ((byte(1) & 0x3F) << 12) | ((byte(2) & 0x3F) << 6) | (byte(3) & 0x3F), 4};
}

/*
 * Measures characters for the display map. The display map asks for one code point at a time, at
 * the x position where it is laid out, and adds the advances up; it never measures line breaks.
 *
 * Contract: deterministic while installed (the same cp and x always give the same advance), every
 * advance >= 0, and Zero-class code points (charWidth) advance 0 so they stay with the character
 * before them. A shaper whose metrics change (font, zoom) is swapped through DisplayMap::setShaper,
 * which wraps again. Lifetime: the DisplayMap holding it borrows it; it must outlive the map or be
 * replaced first. Thread safety: the map calls it on the map's owner thread only; implementations
 * that are immutable values (MonospaceTextShaper) are safe on any thread.
 */
class ITextShaper {
public:
    virtual ~ITextShaper() = default;

    // The advance of one ordinary character cell (a space), > 0. Wrapping by column multiplies it.
    [[nodiscard]] virtual TextWidth cellWidth() const noexcept = 0;
    // The advance of `cp` laid out at `x` (>= 0). A tab advances to the next tab stop after x.
    [[nodiscard]] virtual TextWidth advance(char32_t cp, TextWidth x) const noexcept = 0;

protected:
    ITextShaper() = default;
    ITextShaper(const ITextShaper&) = default;
    ITextShaper& operator=(const ITextShaper&) = default;
};

/*
 * The deterministic width double: a monospace grid of `cellWidth` units per cell. Normal code
 * points take one cell, Wide two, Zero none; a tab advances to the next multiple of tabSize cells.
 * An immutable value, safe on any thread. Precondition (FROST_PRE): cellWidth > 0, tabSize >= 1.
 */
class MonospaceTextShaper final : public ITextShaper {
public:
    explicit MonospaceTextShaper(TextWidth cellWidth = 1, std::uint32_t tabSize = 4) noexcept;

    [[nodiscard]] TextWidth cellWidth() const noexcept override { return m_cellWidth; }
    [[nodiscard]] TextWidth advance(char32_t cp, TextWidth x) const noexcept override;
    // The tab size in cells.
    [[nodiscard]] std::uint32_t tabSize() const noexcept { return m_tabSize; }

private:
    TextWidth m_cellWidth;
    std::uint32_t m_tabSize;
};

} // namespace frost::text
