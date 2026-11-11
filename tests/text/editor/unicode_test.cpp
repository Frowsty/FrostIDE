// SPDX-License-Identifier: Apache-2.0
//
// Character navigation of the editor model (ADR 0025): Snapshot::reversedChunks, grapheme cluster
// boundaries on emoji, flags, ZWJ sequences, combining marks, Hangul and CRLF samples (also with
// clusters split across pieces), character classes and word boundaries.

#include "frost/text/editor/unicode.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_buffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace {

using frost::ByteOffset;
using frost::text::ByteRange;
using frost::text::CharClass;
using frost::text::charClass;
using frost::text::isGraphemeBoundary;
using frost::text::nextGraphemeBoundary;
using frost::text::nextWordBoundary;
using frost::text::previousGraphemeBoundary;
using frost::text::previousWordBoundary;
using frost::text::Snapshot;
using frost::text::TextBuffer;
using frost::text::TextBufferOptions;

// The grapheme samples (hex escapes, so the source stays ASCII).
constexpr std::string_view kEAcute = "e\xCC\x81";                        // e + U+0301
constexpr std::string_view kFlagDe = "\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA"; // RI D, RI E
constexpr std::string_view kFlagFr = "\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7"; // RI F, RI R
constexpr std::string_view kFamily = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
constexpr std::string_view kThumbsTone = "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"; // U+1F44D U+1F3FD
constexpr std::string_view kHangulLv = "\xE1\x84\x80\xE1\x85\xA1";           // U+1100 U+1161
constexpr std::string_view kKeycap = "1\xEF\xB8\x8F\xE2\x83\xA3";            // 1 FE0F 20E3

Snapshot snapshotOf(std::string text, std::uint32_t maxPieceBytes = 4096) {
    TextBufferOptions options;
    options.maxPieceBytes = maxPieceBytes;
    return TextBuffer::fromUtf8(std::move(text), options).value().snapshot();
}

std::string samples() {
    std::string text = "a";
    for (const std::string_view s : {kEAcute, kFlagDe, kFlagFr, kFamily}) {
        text += s;
    }
    text += "\r\n";
    for (const std::string_view s : {kThumbsTone, kHangulLv, kKeycap}) {
        text += s;
    }
    return text + "z";
}

// The cluster boundaries of samples(), from 0 to its size.
const std::vector<std::uint64_t>& boundaries() {
    static const std::vector<std::uint64_t> value{0, 1, 4, 12, 20, 38, 40, 48, 54, 61, 62};
    return value;
}

TEST(ReverseChunks, MatchForwardChunksInReverse) {
    std::string text;
    for (int i = 0; i < 200; ++i) {
        text += "line ";
        text += std::to_string(i);
        text += i % 3 == 0 ? "\xC3\xA9\n" : "\n";
    }
    const Snapshot snap = snapshotOf(text, 5);
    for (const ByteRange range :
         {ByteRange{ByteOffset{0}, snap.size()}, ByteRange{snap.clip(ByteOffset{7}), snap.clip(ByteOffset{431})},
          ByteRange{ByteOffset{3}, ByteOffset{3}}}) {
        std::vector<std::string_view> forward;
        for (const std::string_view chunk : snap.chunks(range)) {
            forward.push_back(chunk);
        }
        std::vector<std::string_view> backward;
        std::uint64_t end = range.end.value();
        const auto reversed = snap.reversedChunks(range);
        for (auto it = reversed.begin(); it != std::default_sentinel; ++it) {
            backward.push_back(*it);
            EXPECT_FALSE((*it).empty());
            EXPECT_EQ(it.offset().value() + (*it).size(), end); // Contiguous, from the end.
            end = it.offset().value();
        }
        EXPECT_EQ(end, range.begin.value());
        std::reverse(backward.begin(), backward.end());
        EXPECT_EQ(forward, backward);
    }
}

TEST(Grapheme, NextAndPreviousStepOverWholeClusters) {
    for (const std::uint32_t pieceBytes : {4096u, 4u, 5u}) {
        const Snapshot snap = snapshotOf(samples(), pieceBytes);
        ASSERT_EQ(snap.size().value(), boundaries().back());
        std::vector<std::uint64_t> forward{0};
        for (ByteOffset at{0}; at < snap.size();) {
            at = nextGraphemeBoundary(snap, at);
            forward.push_back(at.value());
        }
        EXPECT_EQ(forward, boundaries()) << "pieces of " << pieceBytes;
        std::vector<std::uint64_t> backward{snap.size().value()};
        for (ByteOffset at = snap.size(); at > ByteOffset{0};) {
            at = previousGraphemeBoundary(snap, at);
            backward.push_back(at.value());
        }
        std::reverse(backward.begin(), backward.end());
        EXPECT_EQ(backward, boundaries()) << "pieces of " << pieceBytes;
    }
}

TEST(Grapheme, BoundariesInsideClusters) {
    const Snapshot snap = snapshotOf(samples());
    for (std::uint64_t at = 0; at <= snap.size().value(); ++at) {
        if (!snap.isCharBoundary(ByteOffset{at})) {
            continue;
        }
        const bool expected = std::find(boundaries().begin(), boundaries().end(), at) != boundaries().end();
        EXPECT_EQ(isGraphemeBoundary(snap, ByteOffset{at}), expected) << "offset " << at;
    }
    // Inside a cluster, next goes to its end and previous to its start.
    EXPECT_EQ(nextGraphemeBoundary(snap, ByteOffset{8}), ByteOffset{12}); // Between the RIs of a flag.
    EXPECT_EQ(previousGraphemeBoundary(snap, ByteOffset{8}), ByteOffset{4});
    EXPECT_EQ(previousGraphemeBoundary(snap, ByteOffset{39}), ByteOffset{38}); // CRLF interior.
    EXPECT_EQ(nextGraphemeBoundary(snap, ByteOffset{39}), ByteOffset{40});
}

TEST(Grapheme, EdgesAndEmptyText) {
    const Snapshot empty = snapshotOf("");
    EXPECT_EQ(nextGraphemeBoundary(empty, ByteOffset{0}), ByteOffset{0});
    EXPECT_EQ(previousGraphemeBoundary(empty, ByteOffset{0}), ByteOffset{0});
    const Snapshot lines = snapshotOf("ab\n\ncd");
    EXPECT_EQ(previousGraphemeBoundary(lines, ByteOffset{4}), ByteOffset{3}); // Over an empty line's LF.
    EXPECT_EQ(previousGraphemeBoundary(lines, ByteOffset{3}), ByteOffset{2});
    EXPECT_EQ(nextGraphemeBoundary(lines, ByteOffset{2}), ByteOffset{3});
    EXPECT_EQ(nextGraphemeBoundary(lines, ByteOffset{6}), ByteOffset{6});
}

TEST(CharClasses, FromGeneralCategories) {
    EXPECT_EQ(charClass(U'a'), CharClass::Word);
    EXPECT_EQ(charClass(U'_'), CharClass::Word);
    EXPECT_EQ(charClass(U'7'), CharClass::Word);
    EXPECT_EQ(charClass(char32_t{0xE9}), CharClass::Word);   // e acute
    EXPECT_EQ(charClass(char32_t{0x4E2D}), CharClass::Word); // A CJK ideograph.
    EXPECT_EQ(charClass(char32_t{0x301}), CharClass::Word);  // A combining mark stays with its word.
    EXPECT_EQ(charClass(U' '), CharClass::Space);
    EXPECT_EQ(charClass(U'\t'), CharClass::Space);
    EXPECT_EQ(charClass(char32_t{0xA0}), CharClass::Space); // No-break space.
    EXPECT_EQ(charClass(U'\n'), CharClass::LineBreak);
    EXPECT_EQ(charClass(U'\r'), CharClass::LineBreak);
    EXPECT_EQ(charClass(U'.'), CharClass::Punctuation);
    EXPECT_EQ(charClass(U'('), CharClass::Punctuation);
    EXPECT_EQ(charClass(U'\U0001F600'), CharClass::Punctuation);
}

TEST(Words, NextAndPreviousBoundaries) {
    const Snapshot snap = snapshotOf("foo  bar_baz.qux(x)\nnext");
    std::vector<std::uint64_t> forward{0};
    for (ByteOffset at{0}; at < snap.size();) {
        at = nextWordBoundary(snap, at);
        forward.push_back(at.value());
    }
    EXPECT_EQ(forward, (std::vector<std::uint64_t>{0, 3, 12, 13, 16, 17, 18, 19, 20, 24}));
    std::vector<std::uint64_t> backward{snap.size().value()};
    for (ByteOffset at = snap.size(); at > ByteOffset{0};) {
        at = previousWordBoundary(snap, at);
        backward.push_back(at.value());
    }
    EXPECT_EQ(backward, (std::vector<std::uint64_t>{24, 20, 19, 18, 17, 16, 13, 12, 5, 0}));
    // Trailing spaces stop at the line end; leading ones at the line start.
    const Snapshot spaced = snapshotOf("a   \n   b");
    EXPECT_EQ(nextWordBoundary(spaced, ByteOffset{1}), ByteOffset{4});
    EXPECT_EQ(previousWordBoundary(spaced, ByteOffset{8}), ByteOffset{5});
    // Emoji clusters are never split.
    const Snapshot emoji = snapshotOf(std::string("x") + std::string(kFamily) + "y");
    EXPECT_EQ(nextWordBoundary(emoji, ByteOffset{1}), ByteOffset{19});
    EXPECT_EQ(previousWordBoundary(emoji, ByteOffset{19}), ByteOffset{1});
}

} // namespace
