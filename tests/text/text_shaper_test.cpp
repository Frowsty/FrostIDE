// SPDX-License-Identifier: Apache-2.0
//
// The display map's shaping seam (ADR 0026): charWidth classes, the UTF-8 decoder and the
// deterministic MonospaceTextShaper (cells, wide and zero-width characters, tab stops).

#include "frost/text/text_shaper.hpp"

#include <gtest/gtest.h>

namespace {

using frost::text::CharWidth;
using frost::text::charWidth;
using frost::text::decodeUtf8;
using frost::text::MonospaceTextShaper;

TEST(TextShaper, CharWidthClasses) {
    EXPECT_EQ(charWidth(0x61), CharWidth::Normal);     // a
    EXPECT_EQ(charWidth(0x20), CharWidth::Normal);     // space
    EXPECT_EQ(charWidth(0x09), CharWidth::Normal);     // tab (the shaper measures it)
    EXPECT_EQ(charWidth(0xE9), CharWidth::Normal);     // e acute, precomposed
    EXPECT_EQ(charWidth(0x0410), CharWidth::Normal);   // Cyrillic A
    EXPECT_EQ(charWidth(0x0301), CharWidth::Zero);     // combining acute accent
    EXPECT_EQ(charWidth(0x00AD), CharWidth::Zero);     // soft hyphen
    EXPECT_EQ(charWidth(0x200D), CharWidth::Zero);     // zero-width joiner
    EXPECT_EQ(charWidth(0xFE0F), CharWidth::Zero);     // variation selector 16
    EXPECT_EQ(charWidth(0x1160), CharWidth::Zero);     // Hangul medial vowel filler
    EXPECT_EQ(charWidth(0x1F3FB), CharWidth::Zero);    // skin-tone modifier inside a Wide block
    EXPECT_EQ(charWidth(0x1100), CharWidth::Wide);     // Hangul initial consonant
    EXPECT_EQ(charWidth(0x3000), CharWidth::Wide);     // ideographic space
    EXPECT_EQ(charWidth(0x3042), CharWidth::Wide);     // Hiragana a
    EXPECT_EQ(charWidth(0x4E2D), CharWidth::Wide);     // CJK ideograph
    EXPECT_EQ(charWidth(0xAC00), CharWidth::Wide);     // Hangul syllable
    EXPECT_EQ(charWidth(0xFF21), CharWidth::Wide);     // fullwidth A
    EXPECT_EQ(charWidth(0x1F600), CharWidth::Wide);    // grinning face
    EXPECT_EQ(charWidth(0x20000), CharWidth::Wide);    // CJK extension B
    EXPECT_EQ(charWidth(0x303F), CharWidth::Normal);   // half fill space, between Wide ranges
    EXPECT_EQ(charWidth(0x10FFFF), CharWidth::Normal); // last code point
}

TEST(TextShaper, DecodesUtf8) {
    EXPECT_EQ(decodeUtf8("a").cp, 0x61u);
    EXPECT_EQ(decodeUtf8("a").bytes, 1u);
    EXPECT_EQ(decodeUtf8("\xC3\xA9").cp, 0xE9u);
    EXPECT_EQ(decodeUtf8("\xC3\xA9").bytes, 2u);
    EXPECT_EQ(decodeUtf8("\xE4\xB8\xAD").cp, 0x4E2Du);
    EXPECT_EQ(decodeUtf8("\xE4\xB8\xAD").bytes, 3u);
    EXPECT_EQ(decodeUtf8("\xF0\x9F\x98\x80xyz").cp, 0x1F600u);
    EXPECT_EQ(decodeUtf8("\xF0\x9F\x98\x80xyz").bytes, 4u);
}

TEST(TextShaper, MonospaceAdvances) {
    const MonospaceTextShaper shaper(10, 4);
    EXPECT_EQ(shaper.cellWidth(), 10);
    EXPECT_EQ(shaper.tabSize(), 4u);
    EXPECT_EQ(shaper.advance(0x61, 0), 10);
    EXPECT_EQ(shaper.advance(0x61, 37), 10); // Ordinary characters ignore x.
    EXPECT_EQ(shaper.advance(0x4E2D, 0), 20);
    EXPECT_EQ(shaper.advance(0x0301, 15), 0);
    // A tab advances to the next multiple of 4 cells (40 units) after x.
    EXPECT_EQ(shaper.advance(0x09, 0), 40);
    EXPECT_EQ(shaper.advance(0x09, 10), 30);
    EXPECT_EQ(shaper.advance(0x09, 39), 1);
    EXPECT_EQ(shaper.advance(0x09, 40), 40);
    // The defaults: one unit per cell, tab size 4.
    const MonospaceTextShaper cells;
    EXPECT_EQ(cells.cellWidth(), 1);
    EXPECT_EQ(cells.advance(0x09, 6), 2);
}

} // namespace
