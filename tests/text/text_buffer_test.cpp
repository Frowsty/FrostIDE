// SPDX-License-Identifier: Apache-2.0
//
// Edge-case tests for frost::text::TextBuffer and Snapshot (ADR 0013): the LSP line model with
// lone CRs and split CRLFs, TextChange normalisation (D8) and its LSP replay, the round-trip rules,
// the edit check order, batches, restore, UTF-16 conversions, large texts, piece caps, snapshot
// immutability and lifetimes. The random differential lives in differential_test.cpp.

#include "frost/core/error.hpp"
#include "frost/sched/detail/split_mix64.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_buffer.hpp"
#include "frost/text/text_summary.hpp"

#include "text/support/text_oracle.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using frost::ByteColumn;
using frost::ByteOffset;
using frost::ErrorCode;
using frost::LineIndex;
using frost::Result;
using frost::Utf16Column;
using frost::Utf16Offset;
using frost::sched::detail::SplitMix64;
using frost::text::ByteRange;
using frost::text::ChunkRange;
using frost::text::Edit;
using frost::text::LineBreak;
using frost::text::LineCursor;
using frost::text::LineInfo;
using frost::text::Point;
using frost::text::Snapshot;
using frost::text::summarize;
using frost::text::TextBuffer;
using frost::text::TextBufferOptions;
using frost::text::TextChange;
using frost::text::TextSummary;
using frost::text::Utf16Point;
using frost::text::test::oracleSummary;
using frost::text::test::TextOracle;

constexpr std::string_view kEmoji = "\xF0\x9F\x98\x80"; // U+1F600, a surrogate pair in UTF-16.
constexpr std::string_view kEAcute = "\xC3\xA9";        // U+00E9.

Point pt(std::uint32_t line, std::uint32_t column) { return Point{LineIndex{line}, ByteColumn{column}}; }
Utf16Point upt(std::uint32_t line, std::uint32_t column) { return Utf16Point{LineIndex{line}, Utf16Column{column}}; }
ByteOffset at(std::uint64_t offset) { return ByteOffset{offset}; }
ByteRange range(std::uint64_t begin, std::uint64_t end) { return ByteRange{ByteOffset{begin}, ByteOffset{end}}; }

TextBuffer load(std::string text, std::uint32_t maxPieceBytes = 4096) {
    Result<TextBuffer> buffer =
        TextBuffer::fromUtf8(std::move(text), TextBufferOptions{.maxPieceBytes = maxPieceBytes});
    EXPECT_TRUE(buffer.has_value()) << buffer.error().describe();
    return std::move(*buffer);
}

std::vector<std::string_view> pieces(const Snapshot& snapshot) {
    std::vector<std::string_view> out;
    for (auto cursor = snapshot.pieceTree().first(); cursor.item() != nullptr; cursor.next()) {
        out.push_back(cursor.item()->view());
    }
    return out;
}

// Invariants of the buffer and its snapshot, and the text and summary against the oracle.
::testing::AssertionResult consistent(const TextBuffer& buffer, std::string_view expected) {
    if (auto invariants = buffer.checkInvariants(); !invariants) {
        return ::testing::AssertionFailure() << invariants.error().describe();
    }
    if (buffer.current().text() != expected) {
        return ::testing::AssertionFailure() << "text differs";
    }
    if (!(buffer.current().summary() == oracleSummary(expected))) {
        return ::testing::AssertionFailure() << "summary differs from the oracle";
    }
    return ::testing::AssertionSuccess();
}

// The LSP-replay property: the change's old points, mapped to offsets in `before` by LSP's rules,
// spliced with the new text's [start, newEnd), give `after`; no bound is CRLF-interior.
::testing::AssertionResult replaysAsLsp(std::string_view before, std::string_view after, const TextChange& c) {
    TextOracle oldText{std::string(before)};
    oldText.rebuild();
    TextOracle newText{std::string(after)};
    newText.rebuild();
    if (c.startPoint != oldText.pointAt(c.start.value()) || c.oldEndPoint != oldText.pointAt(c.oldEnd.value()) ||
        c.newEndPoint != newText.pointAt(c.newEnd.value())) {
        return ::testing::AssertionFailure() << "points differ from the oracle's";
    }
    using frost::text::test::crlfInterior;
    if (crlfInterior(before, c.start.value()) || crlfInterior(before, c.oldEnd.value()) ||
        crlfInterior(after, c.start.value()) || crlfInterior(after, c.newEnd.value())) {
        return ::testing::AssertionFailure() << "a bound is CRLF-interior";
    }
    const std::uint64_t from = oldText.lspOffset(c.startPoint);
    const std::uint64_t to = oldText.lspOffset(c.oldEndPoint);
    const std::string replayed = std::string(before.substr(0, from)) +
                                 std::string(after.substr(c.start.value(), c.newEnd.value() - c.start.value())) +
                                 std::string(before.substr(to));
    if (replayed != after) {
        return ::testing::AssertionFailure() << "LSP replay gives a different text";
    }
    return ::testing::AssertionSuccess();
}

// Applies one replace and checks the exact normalised change and its replay.
void expectChange(std::string initial, std::uint64_t a, std::uint64_t b, std::string_view text, std::uint64_t start,
                  std::uint64_t oldEnd, std::uint64_t newEnd, Point startPoint, Point oldEndPoint, Point newEndPoint) {
    SCOPED_TRACE(::testing::Message() << "replace [" << a << ", " << b << ") with " << text.size() << " bytes");
    TextBuffer buffer = load(initial, 4);
    const Result<TextChange> change = buffer.replace(range(a, b), text);
    ASSERT_TRUE(change.has_value()) << change.error().describe();
    std::string after = initial;
    after.replace(a, b - a, text);
    EXPECT_TRUE(consistent(buffer, after));
    EXPECT_EQ(change->start.value(), start);
    EXPECT_EQ(change->oldEnd.value(), oldEnd);
    EXPECT_EQ(change->newEnd.value(), newEnd);
    EXPECT_EQ(change->startPoint, startPoint);
    EXPECT_EQ(change->oldEndPoint, oldEndPoint);
    EXPECT_EQ(change->newEndPoint, newEndPoint);
    EXPECT_TRUE(replaysAsLsp(initial, after, *change));
    const auto expected = frost::text::test::expectedChange(initial, a, b, text);
    EXPECT_EQ(expected.start, start) << "the oracle disagrees with the hand-computed change";
    EXPECT_EQ(expected.oldEnd, oldEnd);
    EXPECT_EQ(expected.newEnd, newEnd);
}

std::string randomText(SplitMix64& rng, std::size_t tokens) {
    constexpr std::string_view kTokens[] = {
        "a", "b", "\r", "\n", "\r\n", "\xC3\xA9", "\xE2\x82\xAC", kEmoji, std::string_view("\0", 1)};
    std::string text;
    for (std::size_t i = 0; i < tokens; ++i) {
        text += kTokens[rng.below(std::size(kTokens))];
    }
    return text;
}

TEST(TextBuffer, EmptyBuffer) {
    for (const Snapshot& s : {TextBuffer().snapshot(), Snapshot()}) {
        EXPECT_EQ(s.size(), at(0));
        EXPECT_EQ(s.lineCount(), 1u);
        EXPECT_EQ(s.utf16Size(), Utf16Offset{0});
        EXPECT_EQ(s.text(), "");
        const Result<LineInfo> line = s.lineInfo(LineIndex{0});
        ASSERT_TRUE(line.has_value());
        EXPECT_EQ(*line, (LineInfo{LineIndex{0}, range(0, 0), LineBreak::None}));
        EXPECT_EQ(s.offsetAt(pt(0, 5)), at(0));
        EXPECT_EQ(s.offsetAt(upt(0, 3)), at(0));
        EXPECT_EQ(s.offsetAt(pt(1, 0)).error().code(), ErrorCode::OutOfRange);
        EXPECT_EQ(s.offsetAtUtf16(Utf16Offset{0}), at(0));
        EXPECT_EQ(s.offsetAtUtf16(Utf16Offset{1}).error().code(), ErrorCode::OutOfRange);
        EXPECT_EQ(s.pointAt(at(0)), pt(0, 0));
        EXPECT_EQ(s.clip(at(9)), at(0));
        EXPECT_TRUE(s.isCharBoundary(at(0)));
        EXPECT_FALSE(s.isCharBoundary(at(1)));
        EXPECT_TRUE(s.chunkAt(at(0)).empty());
        const ChunkRange none = s.chunks(range(0, 0));
        EXPECT_TRUE(none.begin() == std::default_sentinel);
        Result<LineCursor> cursor = s.linesFrom(LineIndex{0});
        ASSERT_TRUE(cursor.has_value());
        LineInfo info;
        EXPECT_TRUE(cursor->next(info));
        EXPECT_EQ(info, *line);
        EXPECT_FALSE(cursor->next(info));
        EXPECT_TRUE(s.checkInvariants().has_value());
    }
    TextBuffer buffer;
    EXPECT_EQ(buffer.version(), 0u);
    EXPECT_TRUE(buffer.checkInvariants().has_value());
    const Result<TextBuffer> loaded = TextBuffer::fromUtf8("");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->current().lineCount(), 1u);
    EXPECT_TRUE(loaded->checkInvariants().has_value());
}

TEST(TextBuffer, CrLfSplitAcrossPieces) {
    // Loading with P = 4 cuts "abc\r" | "\nd": the CRLF straddles two pieces.
    TextBuffer loaded = load("abc\r\nd", 4);
    const std::vector<std::string_view> split = pieces(loaded.current());
    ASSERT_EQ(split.size(), 2u);
    EXPECT_EQ(split[0], "abc\r");
    EXPECT_EQ(split[1], "\nd");
    const Snapshot& s = loaded.current();
    EXPECT_TRUE(consistent(loaded, "abc\r\nd"));
    EXPECT_EQ(s.lineCount(), 2u);
    EXPECT_EQ(s.pointAt(at(4)), pt(0, 3)) << "the LF reports the CR's point";
    EXPECT_EQ(s.pointAt(at(3)), pt(0, 3));
    EXPECT_EQ(s.pointAt(at(5)), pt(1, 0));
    EXPECT_EQ(s.lineAt(at(4)), LineIndex{0});
    EXPECT_EQ(*s.lineInfo(LineIndex{0}), (LineInfo{LineIndex{0}, range(0, 3), LineBreak::CrLf}));
    EXPECT_EQ(*s.lineInfo(LineIndex{1}), (LineInfo{LineIndex{1}, range(5, 6), LineBreak::None}));
    EXPECT_EQ(s.utf16PointAt(at(4)), upt(0, 3));
    EXPECT_EQ(s.offsetAt(pt(0, 9)), at(3));

    // Inserting between CR and LF splits the CRLF (2 -> 3 lines); erasing the insert rejoins it.
    TextBuffer buffer = load("a\r\nb");
    EXPECT_EQ(buffer.current().lineCount(), 2u);
    ASSERT_TRUE(buffer.insert(at(2), "x").has_value());
    EXPECT_TRUE(consistent(buffer, "a\rx\nb"));
    EXPECT_EQ(buffer.current().lineCount(), 3u);
    EXPECT_EQ(buffer.current().lineInfo(LineIndex{0})->lineBreak, LineBreak::Cr);
    ASSERT_TRUE(buffer.erase(range(2, 3)).has_value());
    EXPECT_TRUE(consistent(buffer, "a\r\nb"));
    EXPECT_EQ(buffer.current().lineCount(), 2u);
    EXPECT_EQ(buffer.current().pointAt(at(2)), buffer.current().pointAt(at(1)));
    EXPECT_EQ(buffer.current().lineInfo(LineIndex{0})->lineBreak, LineBreak::CrLf);
}

TEST(TextBuffer, LoneCrAndLfCr) {
    TextBuffer cr = load("a\rb");
    EXPECT_EQ(cr.current().lineCount(), 2u);
    EXPECT_EQ(*cr.current().lineInfo(LineIndex{0}), (LineInfo{LineIndex{0}, range(0, 1), LineBreak::Cr}));
    EXPECT_EQ(cr.current().pointAt(at(2)), pt(1, 0));

    TextBuffer lfcr = load("a\n\rb");
    EXPECT_EQ(lfcr.current().lineCount(), 3u) << "LF then CR is two breaks";
    EXPECT_EQ(lfcr.current().lineInfo(LineIndex{1})->content, range(2, 2));
    EXPECT_EQ(lfcr.current().lineInfo(LineIndex{1})->lineBreak, LineBreak::Cr);

    TextBuffer crcrlf = load("\r\r\n");
    EXPECT_EQ(crcrlf.current().lineCount(), 3u);
    EXPECT_EQ(crcrlf.current().lineInfo(LineIndex{0})->lineBreak, LineBreak::Cr);
    EXPECT_EQ(crcrlf.current().lineInfo(LineIndex{1})->lineBreak, LineBreak::CrLf);
    EXPECT_EQ(*crcrlf.current().lineInfo(LineIndex{2}), (LineInfo{LineIndex{2}, range(3, 3), LineBreak::None}));
    EXPECT_EQ(crcrlf.current().pointAt(at(2)), pt(1, 0)) << "inside the CRLF: the CR's point";

    // A trailing CR, then an appended LF: the lone CR becomes a CRLF, the line count stays 2.
    TextBuffer trailing = load("ab\r");
    EXPECT_EQ(trailing.current().lineCount(), 2u);
    EXPECT_EQ(trailing.current().lineInfo(LineIndex{1})->content, range(3, 3));
    const Result<TextChange> change = trailing.insert(at(3), "\n");
    ASSERT_TRUE(change.has_value());
    EXPECT_TRUE(consistent(trailing, "ab\r\n"));
    EXPECT_EQ(trailing.current().lineCount(), 2u);
    EXPECT_EQ(trailing.current().lineInfo(LineIndex{0})->lineBreak, LineBreak::CrLf);
    EXPECT_EQ(change->start, at(2)) << "widened to the CR";
    EXPECT_TRUE(replaysAsLsp("ab\r", "ab\r\n", *change));
}

TEST(TextBuffer, TextChangeNormalisedAtBreaks) {
    // Inserting LF after a lone CR: the change starts at the CR.
    expectChange("a\rb", 2, 2, "\n", 1, 2, 3, pt(0, 1), pt(1, 0), pt(1, 0));
    // Inserting between CR and LF: it covers the whole old CRLF.
    expectChange("a\r\nb", 2, 2, "x", 1, 3, 4, pt(0, 1), pt(1, 0), pt(2, 0));
    // Inserting CR before LF: the change ends after the LF.
    expectChange("a\nb", 1, 1, "\r", 1, 2, 3, pt(0, 1), pt(1, 0), pt(1, 0));
    // Erasing the text between a CR and an LF creates a CRLF: widened on both sides.
    expectChange("a\rxy\nb", 2, 4, "", 1, 5, 3, pt(0, 1), pt(2, 0), pt(1, 0));
    // Erasing the CR of a CRLF.
    expectChange("a\r\nb", 1, 2, "", 1, 3, 2, pt(0, 1), pt(1, 0), pt(1, 0));
    // Erasing the LF of a CRLF.
    expectChange("a\r\nb", 2, 3, "", 1, 3, 2, pt(0, 1), pt(1, 0), pt(1, 0));
    // A plain edit is not widened.
    expectChange("ab\ncd", 1, 4, "XY", 1, 4, 3, pt(0, 1), pt(1, 1), pt(0, 3));
    // Replacing a CRLF's CR by LF.
    expectChange("a\r\nb", 1, 2, "\n", 1, 3, 3, pt(0, 1), pt(1, 0), pt(2, 0));
}

TEST(TextBuffer, RoundTripRules) {
    const std::string text = std::string("ab\r\n") + std::string(kEmoji) + "\r\r\n\n" + std::string(kEAcute) + "x\r" +
                             std::string(kEmoji) + "\r\n";
    for (const std::uint32_t cap : {4u, 5u, 4096u}) {
        TextBuffer buffer = load(text, cap);
        const Snapshot& s = buffer.current();
        TextOracle oracle{text};
        oracle.rebuild();
        for (std::uint64_t x = 0; x <= text.size(); ++x) {
            SCOPED_TRACE(::testing::Message() << "cap " << cap << ", offset " << x);
            const std::uint64_t floored = oracle.crlfFloor(oracle.clip(x));
            EXPECT_EQ(s.pointAt(at(x)), oracle.pointAt(x));
            EXPECT_EQ(s.utf16PointAt(at(x)), oracle.utf16PointAt(x));
            EXPECT_EQ(s.utf16At(at(x)).value(), oracle.utf16At(x));
            EXPECT_EQ(s.offsetAt(s.pointAt(at(x))), at(floored));
            EXPECT_EQ(s.offsetAt(s.utf16PointAt(at(x))), at(floored));
            EXPECT_EQ(s.offsetAtUtf16(s.utf16At(at(x))), at(oracle.clip(x)));
        }
        for (std::uint32_t line = 0; line < oracle.lineCount(); ++line) {
            const LineInfo info = *oracle.lineInfo(line);
            for (std::uint64_t y = info.content.begin.value(); y <= info.content.end.value(); ++y) {
                if (!oracle.isCharBoundary(y)) {
                    continue;
                }
                const Point p = pt(line, static_cast<std::uint32_t>(y - info.content.begin.value()));
                const Result<ByteOffset> offset = s.offsetAt(p);
                ASSERT_TRUE(offset.has_value());
                EXPECT_EQ(s.pointAt(*offset), p) << "canonical point " << line << ":" << p.column.value();
            }
        }
    }
}

TEST(TextBuffer, InvalidUtf8Rejected) {
    const Result<TextBuffer> bad = TextBuffer::fromUtf8("ab\xFF"
                                                        "cd");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(bad.error().message().find("at byte 2"), std::string::npos) << bad.error().message();
    const Result<TextBuffer> truncated = TextBuffer::fromUtf8("abc\xF0\x9F\x98");
    ASSERT_FALSE(truncated.has_value());
    EXPECT_NE(truncated.error().message().find("at byte 3"), std::string::npos);

    // Type "ab" so it coalesces into one piece at the write cursor.
    TextBuffer buffer;
    ASSERT_TRUE(buffer.insert(at(0), "a").has_value());
    ASSERT_TRUE(buffer.insert(at(1), "b").has_value());
    ASSERT_EQ(pieces(buffer.current()).size(), 1u);
    const Snapshot before = buffer.snapshot();
    const auto unchanged = [&] {
        EXPECT_EQ(buffer.version(), 2u);
        EXPECT_TRUE(buffer.current().pieceTree().sharesRootWith(before.pieceTree()));
        EXPECT_TRUE(consistent(buffer, "ab"));
    };
    const Result<TextChange> insert = buffer.insert(at(2), "x\xC3");
    ASSERT_FALSE(insert.has_value());
    EXPECT_EQ(insert.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(insert.error().message(), "ill-formed UTF-8 at byte 1 of the inserted text");
    unchanged();
    const Result<TextChange> replace = buffer.replace(range(0, 1), "\xED\xA0\x80"); // A surrogate.
    ASSERT_FALSE(replace.has_value());
    EXPECT_EQ(replace.error().message(), "ill-formed UTF-8 at byte 0 of the inserted text");
    unchanged();
    const Edit edits[] = {{range(0, 0), "ok"}, {range(1, 1), "\xC0\xAF"}}; // Non-shortest '/'.
    const Result<std::vector<TextChange>> batch = buffer.applyEdits(edits);
    ASSERT_FALSE(batch.has_value());
    EXPECT_EQ(batch.error().message(), "edit 1: ill-formed UTF-8 at byte 0 of the inserted text");
    unchanged();
    // The storage is unchanged too: the next typed byte still coalesces behind "ab".
    ASSERT_TRUE(buffer.insert(at(2), "c").has_value());
    EXPECT_EQ(pieces(buffer.current()).size(), 1u) << "a rejected edit moved the write cursor";
    EXPECT_TRUE(consistent(buffer, "abc"));
}

TEST(TextBuffer, OffsetInsideCharRejected) {
    // "a" [0, 1), "é" [1, 3), emoji [3, 7).
    TextBuffer buffer = load("a" + std::string(kEAcute) + std::string(kEmoji));
    const auto expectInside = [&](const Result<TextChange>& result, std::uint64_t byte) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(result.error().message(), std::format("byte {} is inside a UTF-8 sequence", byte));
    };
    expectInside(buffer.insert(at(2), "x"), 2);
    expectInside(buffer.erase(range(1, 2)), 2);
    expectInside(buffer.erase(range(4, 7)), 4);
    expectInside(buffer.replace(range(0, 6), "y"), 6);
    EXPECT_EQ(buffer.version(), 0u);
    ASSERT_TRUE(buffer.erase(range(1, 3)).has_value());
    EXPECT_TRUE(consistent(buffer, "a" + std::string(kEmoji)));
}

TEST(TextBuffer, OutOfRangeErrors) {
    TextBuffer buffer = load("ab\ncd");
    EXPECT_EQ(buffer.insert(at(6), "x").error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(buffer.erase(range(0, 6)).error().message(), "range end 6 is beyond the text's 5 bytes");
    EXPECT_EQ(buffer.replace(range(9, 9), "").error().code(), ErrorCode::OutOfRange);
    const Snapshot& s = buffer.current();
    EXPECT_EQ(s.lineInfo(LineIndex{2}).error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(s.lineInfo(LineIndex{2}).error().message(), "line 2 is out of range (the text has 2 lines)");
    EXPECT_EQ(s.linesFrom(LineIndex{2}).error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(s.offsetAt(pt(2, 0)).error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(s.offsetAt(upt(7, 0)).error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(s.offsetAtUtf16(Utf16Offset{6}).error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(s.offsetAtUtf16(Utf16Offset{5}), at(5));
    EXPECT_EQ(buffer.version(), 0u);
    EXPECT_TRUE(consistent(buffer, "ab\ncd"));
}

TEST(TextBuffer, ErrorPrecedence) {
    // "a" [0, 1), "é" [1, 3), "b" [3, 4); a limit of 6 bytes for check 5.
    Result<TextBuffer> loaded =
        TextBuffer::fromUtf8("a" + std::string(kEAcute) + "b", TextBufferOptions{.maxBufferBytes = 6});
    ASSERT_TRUE(loaded.has_value());
    TextBuffer& buffer = *loaded;
    const std::string bad = "\xFF";
    const std::string tooLong(3, 'z'); // 4 + 3 > 6.
    struct Case {
        const char* name;
        ByteRange range;
        std::string text;
        ErrorCode code;
        const char* message;
    };
    const Case cases[] = {
        {"1 before 2", range(9, 5), "", ErrorCode::InvalidArgument, "range begin 9 is after its end 5"},
        {"1 before 3", range(2, 1), "", ErrorCode::InvalidArgument, "range begin 2 is after its end 1"},
        {"1 before 4", range(3, 1), bad, ErrorCode::InvalidArgument, "range begin 3 is after its end 1"},
        {"1 before 5", range(4, 0), tooLong, ErrorCode::InvalidArgument, "range begin 4 is after its end 0"},
        {"2 before 3", range(2, 9), "", ErrorCode::OutOfRange, "range end 9 is beyond the text's 4 bytes"},
        {"2 before 4", range(0, 5), bad, ErrorCode::OutOfRange, "range end 5 is beyond the text's 4 bytes"},
        {"2 before 5", range(4, 7), tooLong, ErrorCode::OutOfRange, "range end 7 is beyond the text's 4 bytes"},
        {"3 before 4", range(2, 2), bad, ErrorCode::InvalidArgument, "byte 2 is inside a UTF-8 sequence"},
        {"3 before 5", range(0, 2), tooLong + "zz", ErrorCode::InvalidArgument, "byte 2 is inside a UTF-8 sequence"},
        {"4 before 5", range(4, 4), tooLong + bad, ErrorCode::InvalidArgument,
         "ill-formed UTF-8 at byte 3 of the inserted text"},
        {"5 alone", range(4, 4), tooLong, ErrorCode::OutOfRange,
         "the edit would make the text 7 bytes, more than the buffer limit of 6"},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        const Result<TextChange> result = buffer.replace(c.range, c.text);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), c.code);
        EXPECT_EQ(result.error().message(), c.message);
        // The same order in a batch, with the edit index as prefix.
        const Edit edits[] = {{range(0, 0), ""}, {c.range, c.text}};
        const Result<std::vector<TextChange>> batch = buffer.applyEdits(edits);
        ASSERT_FALSE(batch.has_value());
        EXPECT_EQ(batch.error().code(), c.code);
        if (c.code == ErrorCode::OutOfRange && std::string_view(c.message).starts_with("the edit")) {
            EXPECT_EQ(batch.error().message(),
                      "the edits would make the text 7 bytes, more than the buffer limit of 6");
        } else {
            EXPECT_EQ(batch.error().message(), std::string("edit 1: ") + c.message);
        }
    }
    EXPECT_EQ(buffer.version(), 0u);
    EXPECT_TRUE(consistent(buffer, "a" + std::string(kEAcute) + "b"));
}

TEST(TextBuffer, NoOpEditsKeepVersion) {
    TextBuffer buffer = load("a\r\nb" + std::string(kEAcute));
    for (const Result<TextChange>& change :
         {buffer.insert(at(2), ""), buffer.erase(range(2, 2)), buffer.replace(range(2, 2), "")}) {
        ASSERT_TRUE(change.has_value());
        // Unnormalised: the requested offset, even inside a CRLF, with all points equal.
        EXPECT_EQ(*change, (TextChange{at(2), at(2), at(2), pt(0, 1), pt(0, 1), pt(0, 1)}));
    }
    EXPECT_EQ(buffer.version(), 0u);
    // No-op edits are still validated.
    EXPECT_EQ(buffer.insert(at(5), "").error().code(), ErrorCode::InvalidArgument) << "inside é";
    EXPECT_EQ(buffer.insert(at(7), "").error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(buffer.erase(range(3, 2)).error().code(), ErrorCode::InvalidArgument);
    // Batches of no-ops, and the empty batch, keep the version and return no changes.
    const Edit noops[] = {{range(0, 0), ""}, {range(1, 1), ""}};
    const Result<std::vector<TextChange>> batch = buffer.applyEdits(noops);
    ASSERT_TRUE(batch.has_value());
    EXPECT_TRUE(batch->empty());
    const Result<std::vector<TextChange>> empty = buffer.applyEdits({});
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->empty());
    EXPECT_EQ(buffer.version(), 0u);
    // A batch mixing a no-op and an edit bumps once and reports only the edit.
    const Edit mixed[] = {{range(0, 0), ""}, {range(1, 1), "x"}};
    const Result<std::vector<TextChange>> one = buffer.applyEdits(mixed);
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(one->size(), 1u);
    EXPECT_EQ(buffer.version(), 1u);
}

TEST(TextBuffer, TextChangePoints) {
    TextBuffer buffer = load("line one\nline two\nline three");
    // Replace "one\nline" with "1\n2\n3".
    const Result<TextChange> change = buffer.replace(range(5, 13), "1\n2\n3");
    ASSERT_TRUE(change.has_value());
    EXPECT_EQ(*change, (TextChange{at(5), at(13), at(10), pt(0, 5), pt(1, 4), pt(2, 1)}));
    EXPECT_TRUE(replaysAsLsp("line one\nline two\nline three", buffer.current().text(), *change));
    EXPECT_EQ(buffer.version(), 1u);
    EXPECT_EQ(buffer.current().version(), 1u);
    // A multi-byte line: byte columns count bytes.
    TextBuffer wide = load(std::string(kEmoji) + "x");
    const Result<TextChange> insert = wide.insert(at(4), "\n");
    ASSERT_TRUE(insert.has_value());
    EXPECT_EQ(insert->startPoint, pt(0, 4));
    EXPECT_EQ(insert->newEndPoint, pt(1, 0));
}

TEST(TextBuffer, ApplyEditsMatchesSequential) {
    SplitMix64 rng(11);
    for (int round = 0; round < 200; ++round) {
        const std::string initial = randomText(rng, 1 + static_cast<std::size_t>(rng.below(40)));
        const std::uint32_t cap = round % 2 == 0 ? 4u : 4096u;
        TextBuffer batched = load(initial, cap);
        TextBuffer sequential = load(initial, cap);
        TextOracle oracle{initial};
        // Up to 4 sorted, non-overlapping edits at char boundaries.
        std::vector<std::uint64_t> cuts;
        for (int i = 0; i < 8; ++i) {
            cuts.push_back(oracle.floorChar(rng.below(initial.size() + 1)));
        }
        std::sort(cuts.begin(), cuts.end());
        std::vector<std::string> texts;
        texts.reserve(4);
        std::vector<Edit> edits;
        for (std::size_t i = 0; i < 4; ++i) {
            texts.push_back(randomText(rng, static_cast<std::size_t>(rng.below(4))));
            const std::uint64_t b = rng.below(3) == 0 ? cuts[2 * i] : cuts[2 * i + 1];
            edits.push_back(Edit{range(cuts[2 * i], b), texts.back()});
        }
        const Result<std::vector<TextChange>> changes = batched.applyEdits(edits);
        ASSERT_TRUE(changes.has_value()) << changes.error().describe();
        std::vector<TextChange> expected;
        std::string text = initial;
        for (std::size_t i = edits.size(); i-- > 0;) {
            if (edits[i].range.empty() && edits[i].text.empty()) {
                continue;
            }
            const std::string before = text;
            const Result<TextChange> change = sequential.replace(edits[i].range, edits[i].text);
            ASSERT_TRUE(change.has_value());
            expected.push_back(*change);
            text.replace(edits[i].range.begin.value(), edits[i].range.size(), edits[i].text);
            EXPECT_TRUE(replaysAsLsp(before, text, *change));
        }
        EXPECT_EQ(*changes, expected) << "round " << round;
        EXPECT_TRUE(consistent(batched, text));
        EXPECT_EQ(batched.version(), expected.empty() ? 0u : 1u);
    }
}

TEST(TextBuffer, ApplyEditsRejectsOverlapAndUnsorted) {
    TextBuffer buffer = load("abcdef");
    const auto rejected = [&](std::initializer_list<Edit> list) {
        const std::vector<Edit> edits(list);
        const Result<std::vector<TextChange>> result = buffer.applyEdits(edits);
        EXPECT_FALSE(result.has_value());
        if (!result) {
            EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
            EXPECT_TRUE(result.error().message().starts_with("edit 1: ")) << result.error().message();
        }
    };
    rejected({{range(0, 3), "x"}, {range(2, 4), "y"}}); // Overlap.
    rejected({{range(4, 5), "x"}, {range(0, 1), "y"}}); // Unsorted.
    rejected({{range(3, 3), "x"}, {range(1, 1), "y"}}); // Unsorted inserts.
    rejected({{range(1, 3), "x"}, {range(1, 1), "y"}}); // An insert at a non-empty range's begin.
    EXPECT_EQ(buffer.version(), 0u);
    EXPECT_TRUE(consistent(buffer, "abcdef"));
    // Allowed: an insert at the begin of a following range, and a range ending where the next starts.
    const Edit allowed[] = {{range(1, 1), "x"}, {range(1, 3), "y"}, {range(3, 4), "z"}};
    ASSERT_TRUE(buffer.applyEdits(allowed).has_value());
    EXPECT_TRUE(consistent(buffer, "axyzef"));
}

TEST(TextBuffer, ApplyEditsSameOffsetOrder) {
    TextBuffer buffer = load("ab");
    const Edit edits[] = {{range(1, 1), "x"}, {range(1, 1), "y"}, {range(1, 1), "z"}};
    const Result<std::vector<TextChange>> changes = buffer.applyEdits(edits);
    ASSERT_TRUE(changes.has_value());
    EXPECT_TRUE(consistent(buffer, "axyzb")) << "inserts at one offset keep their given order";
    ASSERT_EQ(changes->size(), 3u);
    // Applied last to first, each change at offset 1 in its own step's coordinates.
    for (const TextChange& change : *changes) {
        EXPECT_EQ(change.start, at(1));
        EXPECT_EQ(change.oldEnd, at(1));
        EXPECT_EQ(change.newEnd, at(2));
    }
}

TEST(TextBuffer, RestoreEarlierSnapshot) {
    TextBuffer buffer = load("one\ntwo");
    const Snapshot original = buffer.snapshot();
    ASSERT_TRUE(buffer.insert(at(7), "\nthree\r\n").has_value());
    ASSERT_TRUE(buffer.erase(range(0, 4)).has_value());
    EXPECT_EQ(buffer.version(), 2u);
    const Result<TextChange> change = buffer.restore(original);
    ASSERT_TRUE(change.has_value());
    EXPECT_EQ(buffer.version(), 3u);
    EXPECT_EQ(buffer.current().version(), 3u);
    EXPECT_TRUE(consistent(buffer, "one\ntwo"));
    // "two\nthree\r\n" (11 bytes, row 2 empty) became "one\ntwo" (7 bytes, ends at 1:3).
    EXPECT_EQ(*change, (TextChange{at(0), at(11), at(7), pt(0, 0), pt(2, 0), pt(1, 3)}));
    EXPECT_TRUE(replaysAsLsp("two\nthree\r\n", "one\ntwo", *change));
    EXPECT_TRUE(buffer.current().pieceTree().sharesRootWith(original.pieceTree()));
    // Restoring current() (an alias) works and bumps the version.
    const Result<TextChange> self = buffer.restore(buffer.current());
    ASSERT_TRUE(self.has_value());
    EXPECT_EQ(buffer.version(), 4u);
    EXPECT_EQ(*self, (TextChange{at(0), at(7), at(7), pt(0, 0), pt(1, 3), pt(1, 3)}));
    EXPECT_TRUE(consistent(buffer, "one\ntwo"));
}

TEST(TextBuffer, RestoreForeignSnapshotRejected) {
    TextBuffer buffer = load("abc");
    TextBuffer other = load("abc");
    for (const Snapshot& foreign : {other.snapshot(), Snapshot()}) {
        const Result<TextChange> result = buffer.restore(foreign);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(result.error().message(), "the snapshot does not come from this buffer");
    }
    EXPECT_EQ(buffer.version(), 0u);
    EXPECT_FALSE(Snapshot().sharesStorageWith(Snapshot()));
    EXPECT_TRUE(buffer.snapshot().sharesStorageWith(buffer.current()));
    EXPECT_FALSE(buffer.snapshot().sharesStorageWith(other.current()));
}

TEST(TextBuffer, RestoreThenCoalesce) {
    // Typed text coalesces only behind the write cursor, never over bytes another snapshot sees.
    TextBuffer buffer;
    ASSERT_TRUE(buffer.insert(at(0), "a").has_value());
    ASSERT_TRUE(buffer.insert(at(1), "b").has_value());
    ASSERT_TRUE(buffer.insert(at(2), "c").has_value());
    EXPECT_EQ(pieces(buffer.current()).size(), 1u) << "typing coalesces";
    const Snapshot abc = buffer.snapshot();
    ASSERT_TRUE(buffer.insert(at(3), "d").has_value());
    const Snapshot abcd = buffer.snapshot();
    ASSERT_TRUE(buffer.restore(abc).has_value());
    ASSERT_TRUE(buffer.insert(at(3), "x").has_value());
    EXPECT_TRUE(consistent(buffer, "abcx"));
    EXPECT_EQ(abcd.text(), "abcd") << "the restored piece was extended over published bytes";
    EXPECT_EQ(abc.text(), "abc");
    EXPECT_EQ(pieces(buffer.current()).size(), 2u);
    // Typing continues to coalesce behind the new insert.
    ASSERT_TRUE(buffer.insert(at(4), "y").has_value());
    EXPECT_EQ(pieces(buffer.current()).size(), 2u);
    EXPECT_TRUE(consistent(buffer, "abcxy"));
    // Coalescing respects the piece cap.
    TextBuffer capped(TextBufferOptions{.maxPieceBytes = 4});
    for (std::uint64_t i = 0; i < 10; ++i) {
        ASSERT_TRUE(capped.insert(at(i), "q").has_value());
    }
    EXPECT_EQ(pieces(capped.current()).size(), 3u);
    EXPECT_TRUE(consistent(capped, "qqqqqqqqqq"));
}

TEST(TextBuffer, Utf16SurrogateFloors) {
    // "a" [0, 1), emoji [1, 5) = UTF-16 [1, 3), "b" [5, 6) = UTF-16 [3, 4).
    TextBuffer buffer = load("a" + std::string(kEmoji) + "b");
    const Snapshot& s = buffer.current();
    EXPECT_EQ(s.utf16Size(), Utf16Offset{4});
    EXPECT_EQ(s.offsetAt(upt(0, 2)), at(1)) << "between the surrogates floors to the emoji";
    EXPECT_EQ(s.offsetAt(upt(0, 3)), at(5));
    EXPECT_EQ(s.offsetAtUtf16(Utf16Offset{2}), at(1));
    EXPECT_EQ(s.offsetAtUtf16(Utf16Offset{3}), at(5));
    for (std::uint64_t inside = 2; inside < 5; ++inside) {
        EXPECT_EQ(s.utf16At(at(inside)), Utf16Offset{1});
        EXPECT_EQ(s.utf16PointAt(at(inside)), upt(0, 1));
        EXPECT_EQ(s.clip(at(inside)), at(1));
        EXPECT_FALSE(s.isCharBoundary(at(inside)));
        EXPECT_EQ(s.pointAt(at(inside)), pt(0, static_cast<std::uint32_t>(inside))) << "byte columns are not floored";
    }
    EXPECT_EQ(s.utf16PointAt(at(6)), upt(0, 4));
}

TEST(TextBuffer, ColumnClampsToLineEnd) {
    TextBuffer buffer = load("abc\r\nd" + std::string(kEAcute) + "\nlast");
    const Snapshot& s = buffer.current();
    EXPECT_EQ(s.offsetAt(pt(0, 10)), at(3));
    EXPECT_EQ(s.offsetAt(upt(0, 10)), at(3));
    EXPECT_EQ(s.offsetAt(pt(1, 2)), at(6)) << "inside é floors to its start";
    EXPECT_EQ(s.offsetAt(pt(1, 99)), at(8));
    EXPECT_EQ(s.offsetAt(upt(1, 99)), at(8));
    EXPECT_EQ(s.offsetAt(pt(2, 99)), at(13));
    EXPECT_EQ(s.offsetAt(upt(2, 4)), at(13));
}

TEST(TextBuffer, NulBytes) {
    const std::string text("a\0b\n\0\0\r\n\0", 9);
    TextBuffer buffer = load(text, 4);
    const Snapshot& s = buffer.current();
    EXPECT_EQ(s.size(), at(9));
    EXPECT_EQ(s.lineCount(), 3u);
    EXPECT_EQ(s.byteAt(at(1)), '\0');
    EXPECT_EQ(s.text(), text);
    EXPECT_EQ(s.lineInfo(LineIndex{1})->content, range(4, 6));
    EXPECT_TRUE(consistent(buffer, text));
    ASSERT_TRUE(buffer.insert(at(9), std::string_view("\0", 1)).has_value());
    EXPECT_TRUE(consistent(buffer, text + std::string(1, '\0')));
}

TEST(TextBuffer, HugeLine) {
    // An 8 MiB single line of "a" + emoji (5 bytes, 3 UTF-16 units, 2 characters).
    constexpr std::size_t kUnits = 8 * 1024 * 1024 / 5;
    std::string line;
    line.reserve(kUnits * 5);
    for (std::size_t i = 0; i < kUnits; ++i) {
        line += 'a';
        line += kEmoji;
    }
    const auto checkHuge = [&](const TextBuffer& buffer) {
        const Snapshot& s = buffer.current();
        EXPECT_EQ(s.size(), at(line.size()));
        EXPECT_EQ(s.lineCount(), 1u);
        EXPECT_EQ(s.summary().longestLine, 0u);
        EXPECT_EQ(s.summary().longestLineChars, 2 * kUnits);
        EXPECT_EQ(s.utf16Size(), Utf16Offset{3 * kUnits});
        EXPECT_EQ(s.pointAt(at(line.size())), pt(0, static_cast<std::uint32_t>(line.size())));
        const std::uint64_t far = 5 * (kUnits - 7);
        EXPECT_EQ(s.utf16PointAt(at(far)), upt(0, static_cast<std::uint32_t>(3 * (kUnits - 7))));
        EXPECT_EQ(s.offsetAt(upt(0, static_cast<std::uint32_t>(3 * (kUnits - 7) + 2))), at(far + 1));
        EXPECT_EQ(s.offsetAt(pt(0, static_cast<std::uint32_t>(far + 3))), at(far + 1));
        EXPECT_EQ(s.offsetAt(pt(0, 0xFFFF'FFFFu)), at(line.size()));
        EXPECT_EQ(s.offsetAtUtf16(Utf16Offset{3 * kUnits - 1}), at(line.size() - 4));
        EXPECT_TRUE(buffer.checkInvariants().has_value());
    };
    checkHuge(load(line));
    // Built from 1000 appends.
    TextBuffer built;
    const std::size_t step = (kUnits + 999) / 1000 * 5;
    for (std::size_t offset = 0; offset < line.size(); offset += step) {
        const std::string_view part = std::string_view(line).substr(offset, step);
        ASSERT_TRUE(built.insert(at(offset), part).has_value());
    }
    EXPECT_LE(built.version(), 1000u);
    checkHuge(built);
    EXPECT_EQ(built.current().text(), line);
}

TEST(TextBuffer, ManyLines) {
    constexpr std::uint32_t kLines = 1'000'000;
    std::string text;
    text.reserve(kLines * 8);
    std::vector<std::uint64_t> starts;
    starts.reserve(kLines + 1);
    for (std::uint32_t i = 0; i < kLines; ++i) {
        starts.push_back(text.size());
        text += std::to_string(i % 1000);
        text += i % 3 == 0 ? "\r\n" : "\n";
    }
    starts.push_back(text.size());
    TextBuffer buffer = load(text);
    const Snapshot& s = buffer.current();
    EXPECT_EQ(s.lineCount(), kLines + 1);
    const auto expectedLine = [&](std::uint32_t line) {
        if (line == kLines) {
            return LineInfo{LineIndex{line}, range(text.size(), text.size()), LineBreak::None};
        }
        const std::uint64_t width = std::to_string(line % 1000).size();
        return LineInfo{LineIndex{line}, range(starts[line], starts[line] + width),
                        line % 3 == 0 ? LineBreak::CrLf : LineBreak::Lf};
    };
    SplitMix64 rng(5);
    for (int i = 0; i < 1000; ++i) {
        const auto first = static_cast<std::uint32_t>(rng.below(kLines + 1));
        ASSERT_EQ(*s.lineInfo(LineIndex{first}), expectedLine(first));
        Result<LineCursor> cursor = s.linesFrom(LineIndex{first});
        ASSERT_TRUE(cursor.has_value());
        LineInfo info;
        for (std::uint32_t line = first; line < std::min(first + 16, kLines + 1); ++line) {
            ASSERT_TRUE(cursor->next(info));
            ASSERT_EQ(info, expectedLine(line));
        }
        ASSERT_EQ(s.lineAt(at(starts[std::min(first, kLines)])), LineIndex{std::min(first, kLines)});
    }
    Result<LineCursor> last = s.linesFrom(LineIndex{kLines});
    LineInfo info;
    ASSERT_TRUE(last->next(info));
    EXPECT_EQ(info, expectedLine(kLines));
    EXPECT_FALSE(last->next(info));
}

TEST(TextBuffer, MaxBufferBytes) {
    const TextBufferOptions options{.maxBufferBytes = 10};
    const Result<TextBuffer> tooLong = TextBuffer::fromUtf8("01234567890", options);
    ASSERT_FALSE(tooLong.has_value());
    EXPECT_EQ(tooLong.error().code(), ErrorCode::OutOfRange);
    EXPECT_EQ(tooLong.error().message(), "the text has 11 bytes, more than the buffer limit of 10");
    // The size check comes before validation.
    EXPECT_EQ(TextBuffer::fromUtf8("0123456789\xFF", options).error().code(), ErrorCode::OutOfRange);
    Result<TextBuffer> buffer = TextBuffer::fromUtf8("01234", options);
    ASSERT_TRUE(buffer.has_value());
    EXPECT_EQ(buffer->insert(at(5), "abcdef").error().code(), ErrorCode::OutOfRange);
    ASSERT_TRUE(buffer->insert(at(5), "abcde").has_value());
    EXPECT_EQ(buffer->current().size(), at(10));
    EXPECT_EQ(buffer->insert(at(0), "x").error().code(), ErrorCode::OutOfRange);
    ASSERT_TRUE(buffer->replace(range(0, 1), "y").has_value()) << "a same-size replace at the limit";
    const Edit edits[] = {{range(0, 1), ""}, {range(2, 2), "zz"}};
    EXPECT_EQ(buffer->applyEdits(edits).error().code(), ErrorCode::OutOfRange);
    EXPECT_TRUE(consistent(*buffer, "y1234abcde"));
}

TEST(TextBuffer, PieceCapRespected) {
    SplitMix64 rng(9);
    for (const std::uint32_t cap : {4u, 5u, 7u, 16u}) {
        SCOPED_TRACE(::testing::Message() << "cap " << cap);
        std::string text = randomText(rng, 300);
        TextBuffer buffer = load(text, cap);
        for (int i = 0; i < 50; ++i) {
            const std::string insert = randomText(rng, static_cast<std::size_t>(rng.below(40)));
            TextOracle oracle{text};
            const std::uint64_t x = oracle.floorChar(rng.below(text.size() + 1));
            ASSERT_TRUE(buffer.insert(at(x), insert).has_value());
            text.insert(x, insert);
        }
        for (const std::string_view piece : pieces(buffer.current())) {
            EXPECT_GE(piece.size(), 1u);
            EXPECT_LE(piece.size(), cap);
            EXPECT_TRUE(frost::text::test::oracleValid(piece));
        }
        EXPECT_TRUE(consistent(buffer, text));
    }
}

TEST(TextBuffer, SnapshotsImmutable) {
    SplitMix64 rng(13);
    TextBuffer buffer = load("start\n", 5);
    std::string text = "start\n";
    std::vector<std::pair<Snapshot, std::string>> history;
    for (int i = 0; i < 300; ++i) {
        history.emplace_back(buffer.snapshot(), text);
        TextOracle oracle{text};
        const std::uint64_t a = oracle.floorChar(rng.below(text.size() + 1));
        const std::uint64_t b = oracle.floorChar(std::min<std::uint64_t>(text.size(), a + rng.below(6)));
        const std::string insert = randomText(rng, static_cast<std::size_t>(rng.below(5)));
        ASSERT_TRUE(buffer.replace(range(a, b), insert).has_value());
        text.replace(a, b - a, insert);
    }
    for (std::size_t i = 0; i < history.size(); ++i) {
        const auto& [snapshot, expected] = history[i];
        ASSERT_EQ(snapshot.text(), expected) << "snapshot " << i;
        ASSERT_EQ(snapshot.summary(), summarize(expected));
        ASSERT_TRUE(snapshot.checkInvariants().has_value());
    }
    EXPECT_TRUE(consistent(buffer, text));
}

TEST(TextBuffer, SnapshotOutlivesBuffer) {
    Snapshot survivor;
    {
        TextBuffer buffer = load("first\r\nsecond", 4);
        ASSERT_TRUE(buffer.insert(at(13), "\nthird").has_value());
        survivor = buffer.snapshot();
    }
    // The ASan legs catch any dangling view here.
    EXPECT_EQ(survivor.text(), "first\r\nsecond\nthird");
    EXPECT_EQ(survivor.lineCount(), 3u);
    EXPECT_EQ(survivor.lineInfo(LineIndex{2})->content, range(14, 19));
    EXPECT_EQ(survivor.chunkAt(at(14)).front(), 't');
    EXPECT_TRUE(survivor.checkInvariants().has_value());
}

TEST(TextBuffer, ChunkRangeOutlivesSnapshotAndBuffer) {
    std::optional<ChunkRange> chunks;
    std::optional<LineCursor> lines;
    {
        TextBuffer buffer = load("alpha\nbeta\ngamma", 4);
        ASSERT_TRUE(buffer.insert(at(16), "\ndelta").has_value());
        const Snapshot snapshot = buffer.snapshot();
        chunks.emplace(snapshot.chunks(range(2, 20)));
        lines.emplace(*snapshot.linesFrom(LineIndex{1}));
    }
    std::string joined;
    for (const std::string_view chunk : *chunks) {
        joined += chunk;
    }
    EXPECT_EQ(joined, std::string_view("alpha\nbeta\ngamma\ndelta").substr(2, 18));
    EXPECT_EQ(chunks->range(), range(2, 20));
    LineInfo info;
    std::vector<std::uint64_t> starts;
    while (lines->next(info)) {
        starts.push_back(info.content.begin.value());
    }
    EXPECT_EQ(starts, (std::vector<std::uint64_t>{6, 11, 17}));
}

TEST(TextBuffer, ChunksValidAndCovering) {
    const std::string text = "x" + std::string(kEmoji) + "\r\n" + std::string(kEAcute) + "abc\xE2\x82\xAC" + "\n";
    for (const std::uint32_t cap : {4u, 5u, 4096u}) {
        TextBuffer buffer = load(text, cap);
        const Snapshot& s = buffer.current();
        TextOracle oracle{text};
        for (std::uint64_t a = 0; a <= text.size(); ++a) {
            for (std::uint64_t b = a; b <= text.size(); ++b) {
                if (!oracle.isCharBoundary(a) || !oracle.isCharBoundary(b)) {
                    continue;
                }
                std::string joined;
                for (const std::string_view chunk : s.chunks(range(a, b))) {
                    ASSERT_FALSE(chunk.empty());
                    ASSERT_TRUE(frost::text::test::oracleValid(chunk));
                    joined += chunk;
                }
                ASSERT_EQ(joined, text.substr(a, b - a)) << "cap " << cap << " [" << a << ", " << b << ")";
                ASSERT_EQ(s.text(range(a, b)), text.substr(a, b - a));
            }
            const std::string_view rest = s.chunkAt(at(a));
            ASSERT_TRUE(std::string_view(text).substr(a).starts_with(rest));
            ASSERT_EQ(rest.empty(), a == text.size());
        }
    }
}

TEST(TextBuffer, SummaryOfMatchesOracle) {
    const std::string text = "ab\r" + std::string(kEmoji) + "\r\n\n" + std::string(kEAcute) + "\rlonger line\r\n";
    for (const std::uint32_t cap : {4u, 7u, 4096u}) {
        TextBuffer buffer = load(text, cap);
        const Snapshot& s = buffer.current();
        TextOracle oracle{text};
        for (std::uint64_t a = 0; a <= text.size(); ++a) {
            for (std::uint64_t b = a; b <= text.size(); ++b) {
                if (!oracle.isCharBoundary(a) || !oracle.isCharBoundary(b)) {
                    continue;
                }
                ASSERT_EQ(s.summaryOf(range(a, b)), oracleSummary(std::string_view(text).substr(a, b - a)))
                    << "cap " << cap << " [" << a << ", " << b << ")";
            }
        }
    }
}

TEST(TextBuffer, LineCursorMatchesLineInfo) {
    const std::string text = "\r\n\r\r\nab\n" + std::string(kEmoji) + "\r" + std::string(1000, 'z') + "\n\n";
    for (const std::uint32_t cap : {4u, 64u, 4096u}) {
        TextBuffer buffer = load(text, cap);
        const Snapshot& s = buffer.current();
        for (std::uint32_t first = 0; first < s.lineCount(); ++first) {
            Result<LineCursor> cursor = s.linesFrom(LineIndex{first});
            ASSERT_TRUE(cursor.has_value());
            LineInfo info;
            for (std::uint32_t line = first; line < s.lineCount(); ++line) {
                ASSERT_TRUE(cursor->next(info));
                ASSERT_EQ(info, *s.lineInfo(LineIndex{line})) << "cap " << cap << " from " << first;
            }
            ASSERT_FALSE(cursor->next(info));
        }
    }
}

} // namespace
