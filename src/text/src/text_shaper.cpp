// SPDX-License-Identifier: Apache-2.0
//
// charWidth's range tables and MonospaceTextShaper (frost/text/text_shaper.hpp).
/*
 * The ranges below are written from the Unicode Character Database's published property values
 * (EastAsianWidth.txt for W and F; the Mn, Me and Cf general categories; the conjoining Hangul
 * jamo), which the Unicode License v3 lets anyone use. They are an approximation, grouped by block:
 * a few narrow symbols inside the emoji blocks count as Wide, and marks of scripts not listed count
 * as Normal. No code was taken from any wcwidth implementation.
 */

#include "frost/text/text_shaper.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <span>

namespace frost::text {

namespace {

struct Range {
    char32_t first;
    char32_t last;
};

// Sorted, disjoint. Checked before kWide: the emoji skin-tone modifiers sit inside a Wide block.
constexpr std::array kZero{
    Range{0x00AD, 0x00AD},   // soft hyphen
    Range{0x0300, 0x036F},   // combining diacritical marks
    Range{0x0483, 0x0489},   // Cyrillic combining marks
    Range{0x0591, 0x05BD},   // Hebrew points and accents
    Range{0x05BF, 0x05BF},   //
    Range{0x05C1, 0x05C2},   //
    Range{0x05C4, 0x05C5},   //
    Range{0x05C7, 0x05C7},   //
    Range{0x0610, 0x061A},   // Arabic marks
    Range{0x061C, 0x061C},   // Arabic letter mark
    Range{0x064B, 0x065F},   //
    Range{0x0670, 0x0670},   //
    Range{0x06D6, 0x06DC},   //
    Range{0x06DF, 0x06E4},   //
    Range{0x06E7, 0x06E8},   //
    Range{0x06EA, 0x06ED},   //
    Range{0x0711, 0x0711},   // Syriac
    Range{0x0730, 0x074A},   //
    Range{0x07A6, 0x07B0},   // Thaana
    Range{0x07EB, 0x07F3},   // NKo
    Range{0x0900, 0x0902},   // Devanagari
    Range{0x093A, 0x093A},   //
    Range{0x093C, 0x093C},   //
    Range{0x0941, 0x0948},   //
    Range{0x094D, 0x094D},   //
    Range{0x0951, 0x0957},   //
    Range{0x0962, 0x0963},   //
    Range{0x0981, 0x0981},   // Bengali
    Range{0x09BC, 0x09BC},   //
    Range{0x09C1, 0x09C4},   //
    Range{0x09CD, 0x09CD},   //
    Range{0x0E31, 0x0E31},   // Thai
    Range{0x0E34, 0x0E3A},   //
    Range{0x0E47, 0x0E4E},   //
    Range{0x1160, 0x11FF},   // Hangul medial vowels and final consonants (conjoining)
    Range{0x1AB0, 0x1AFF},   // combining diacritical marks extended
    Range{0x1DC0, 0x1DFF},   // combining diacritical marks supplement
    Range{0x200B, 0x200F},   // zero-width space, non-joiner, joiner; LRM, RLM
    Range{0x202A, 0x202E},   // bidi embeddings and overrides
    Range{0x2060, 0x2064},   // word joiner, invisible operators
    Range{0x2066, 0x206F},   // bidi isolates, deprecated format characters
    Range{0x20D0, 0x20FF},   // combining marks for symbols
    Range{0xD7B0, 0xD7FF},   // Hangul jamo extended-B (conjoining)
    Range{0xFE00, 0xFE0F},   // variation selectors
    Range{0xFE20, 0xFE2F},   // combining half marks
    Range{0xFEFF, 0xFEFF},   // zero-width no-break space
    Range{0xFFF9, 0xFFFB},   // interlinear annotation controls
    Range{0x1F3FB, 0x1F3FF}, // emoji skin-tone modifiers (they attach to the emoji before them)
    Range{0xE0000, 0xE007F}, // tags
    Range{0xE0100, 0xE01EF}, // variation selectors supplement
};

// Sorted, disjoint.
constexpr std::array kWide{
    Range{0x1100, 0x115F},   // Hangul initial consonants
    Range{0x231A, 0x231B},   // watch, hourglass
    Range{0x2329, 0x232A},   // angle brackets
    Range{0x23E9, 0x23EC},   //
    Range{0x23F0, 0x23F0},   //
    Range{0x23F3, 0x23F3},   //
    Range{0x25FD, 0x25FE},   //
    Range{0x2614, 0x2615},   //
    Range{0x2648, 0x2653},   // zodiac
    Range{0x267F, 0x267F},   //
    Range{0x2693, 0x2693},   //
    Range{0x26A1, 0x26A1},   //
    Range{0x26AA, 0x26AB},   //
    Range{0x26BD, 0x26BE},   //
    Range{0x26C4, 0x26C5},   //
    Range{0x26CE, 0x26CE},   //
    Range{0x26D4, 0x26D4},   //
    Range{0x26EA, 0x26EA},   //
    Range{0x26F2, 0x26F3},   //
    Range{0x26F5, 0x26F5},   //
    Range{0x26FA, 0x26FA},   //
    Range{0x26FD, 0x26FD},   //
    Range{0x2705, 0x2705},   //
    Range{0x270A, 0x270B},   //
    Range{0x2728, 0x2728},   //
    Range{0x274C, 0x274C},   //
    Range{0x274E, 0x274E},   //
    Range{0x2753, 0x2755},   //
    Range{0x2757, 0x2757},   //
    Range{0x2795, 0x2797},   //
    Range{0x27B0, 0x27B0},   //
    Range{0x27BF, 0x27BF},   //
    Range{0x2B1B, 0x2B1C},   //
    Range{0x2B50, 0x2B50},   //
    Range{0x2B55, 0x2B55},   //
    Range{0x2E80, 0x303E},   // CJK and Kangxi radicals, CJK symbols and punctuation
    Range{0x3041, 0x33FF},   // kana, Bopomofo, Hangul compatibility jamo, enclosed CJK, CJK compatibility
    Range{0x3400, 0x4DBF},   // CJK extension A
    Range{0x4E00, 0x9FFF},   // CJK unified ideographs
    Range{0xA000, 0xA4CF},   // Yi
    Range{0xA960, 0xA97F},   // Hangul jamo extended-A
    Range{0xAC00, 0xD7A3},   // Hangul syllables
    Range{0xF900, 0xFAFF},   // CJK compatibility ideographs
    Range{0xFE10, 0xFE19},   // vertical forms
    Range{0xFE30, 0xFE6F},   // CJK compatibility forms, small form variants
    Range{0xFF00, 0xFF60},   // fullwidth forms
    Range{0xFFE0, 0xFFE6},   //
    Range{0x16FE0, 0x16FE4}, //
    Range{0x17000, 0x18CFF}, // Tangut, Khitan
    Range{0x1AFF0, 0x1B2FF}, // kana extended and supplement, Nushu
    Range{0x1F004, 0x1F004}, //
    Range{0x1F0CF, 0x1F0CF}, //
    Range{0x1F18E, 0x1F18E}, //
    Range{0x1F191, 0x1F19A}, //
    Range{0x1F200, 0x1F2FF}, // enclosed ideographic supplement
    Range{0x1F300, 0x1F64F}, // pictographs and emoticons (a few narrow ones count as Wide)
    Range{0x1F680, 0x1F6FF}, // transport and map symbols (likewise)
    Range{0x1F900, 0x1F9FF}, // supplemental symbols and pictographs
    Range{0x1FA70, 0x1FAFF}, // symbols and pictographs extended-A
    Range{0x20000, 0x2FFFD}, // CJK extensions B to F, compatibility supplement
    Range{0x30000, 0x3FFFD}, // CJK extension G and later
};

[[nodiscard]] bool contains(std::span<const Range> ranges, char32_t cp) noexcept {
    const auto it = std::upper_bound(ranges.begin(), ranges.end(), cp,
                                     [](char32_t value, const Range& range) { return value < range.first; });
    return it != ranges.begin() && cp <= std::prev(it)->last;
}

} // namespace

CharWidth charWidth(char32_t cp) noexcept {
    if (cp < 0x00AD) {
        return CharWidth::Normal; // ASCII and Latin-1 fast path.
    }
    if (contains(kZero, cp)) {
        return CharWidth::Zero;
    }
    if (contains(kWide, cp)) {
        return CharWidth::Wide;
    }
    return CharWidth::Normal;
}

MonospaceTextShaper::MonospaceTextShaper(TextWidth cellWidth, std::uint32_t tabSize) noexcept
    : m_cellWidth(cellWidth), m_tabSize(tabSize) {
    FROST_PRE(cellWidth > 0);
    FROST_PRE(tabSize >= 1);
}

TextWidth MonospaceTextShaper::advance(char32_t cp, TextWidth x) const noexcept {
    if (cp == U'\t') {
        const TextWidth tab = m_cellWidth * static_cast<TextWidth>(m_tabSize);
        return tab - x % tab;
    }
    switch (charWidth(cp)) {
    case CharWidth::Zero: return 0;
    case CharWidth::Wide: return 2 * m_cellWidth;
    case CharWidth::Normal: break;
    }
    return m_cellWidth;
}

} // namespace frost::text
