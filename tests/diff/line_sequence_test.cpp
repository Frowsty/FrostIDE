// SPDX-License-Identifier: Apache-2.0
//
// Tests of frost::diff::LineSequence (ADR 0019, D2): the Lsp and Lf split rules against the naive
// splitter oracle, the mapping to text::Snapshot lines, the limits checked before copying, shared
// storage across copies and moves, and firstLineBreak.

#include "frost/diff/line_sequence.hpp"
#include "frost/text/text_buffer.hpp"

#include "diff/diff_test_util.hpp"
#include "diff/support/generators.hpp"
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace frost::diff::test {
namespace {

std::vector<std::string> linesOf(const LineSequence& sequence) {
    std::vector<std::string> lines;
    for (std::uint32_t i = 0; i < sequence.size(); ++i) {
        lines.emplace_back(sequence.line(LineIndex{i}));
    }
    return lines;
}

using Lines = std::vector<std::string>;

TEST(LineSequence, SplitRules) {
    struct Case {
        std::string text;
        Lines lsp;
        Lines lf;
    };
    const Case cases[] = {
        {"", {}, {}},
        {"a", {"a"}, {"a"}},
        {"a\n", {"a\n"}, {"a\n"}},
        {"a\n\n", {"a\n", "\n"}, {"a\n", "\n"}},
        {"a\nb", {"a\n", "b"}, {"a\n", "b"}},
        {"a\r\nb\r\n", {"a\r\n", "b\r\n"}, {"a\r\n", "b\r\n"}},
        {"a\rb", {"a\r", "b"}, {"a\rb"}},
        {"a\r", {"a\r"}, {"a\r"}},
        {"\r\r\n\n", {"\r", "\r\n", "\n"}, {"\r\r\n", "\n"}},
        {"x\r\ry\n", {"x\r", "\r", "y\n"}, {"x\r\ry\n"}},
        {std::string("\0\n\xFF", 3), {std::string("\0\n", 2), "\xFF"}, {std::string("\0\n", 2), "\xFF"}},
    };
    for (const Case& c : cases) {
        const LineSequence lsp = makeSequence(c.text, LineSplit::Lsp);
        const LineSequence lf = makeSequence(c.text, LineSplit::Lf);
        EXPECT_EQ(linesOf(lsp), c.lsp) << printable(c.text);
        EXPECT_EQ(linesOf(lf), c.lf) << printable(c.text);
        EXPECT_EQ(lsp.bytes(), c.text);
        EXPECT_EQ(lsp.split(), LineSplit::Lsp);
        EXPECT_EQ(lf.split(), LineSplit::Lf);
        // Lines tile the bytes, and a line lacks a break only at the end.
        for (const LineSequence* sequence : {&lsp, &lf}) {
            const bool lastHasBreak =
                !c.text.empty() &&
                (c.text.back() == '\n' || (sequence->split() == LineSplit::Lsp && c.text.back() == '\r'));
            std::uint64_t at = 0;
            for (std::uint32_t i = 0; i < sequence->size(); ++i) {
                const text::ByteRange range = sequence->lineRange(LineIndex{i});
                EXPECT_EQ(range.begin.value(), at);
                at = range.end.value();
                EXPECT_EQ(sequence->hasBreak(LineIndex{i}), i + 1 < sequence->size() || lastHasBreak);
            }
            EXPECT_EQ(at, c.text.size());
            const text::ByteRange all = sequence->byteRange(LineRange{LineIndex{0}, LineIndex{sequence->size()}});
            EXPECT_EQ(all.begin.value(), 0u);
            EXPECT_EQ(all.end.value(), c.text.size());
        }
    }
    // Lines compare byte-exactly: equal hashes for equal lines, and "a\r\n" differs from "a\n".
    const LineSequence mixed = makeSequence("a\r\na\na\na", LineSplit::Lsp);
    ASSERT_EQ(mixed.size(), 4u);
    EXPECT_EQ(mixed.lineHash(LineIndex{1}), mixed.lineHash(LineIndex{2}));
    EXPECT_NE(mixed.lineHash(LineIndex{0}), mixed.lineHash(LineIndex{1}));
    EXPECT_NE(mixed.lineHash(LineIndex{2}), mixed.lineHash(LineIndex{3}));
    // The default sequence is empty Lsp.
    const LineSequence empty;
    EXPECT_EQ(empty.size(), 0u);
    EXPECT_EQ(empty.split(), LineSplit::Lsp);
    EXPECT_TRUE(empty.bytes().empty());
    EXPECT_EQ(empty.firstLineBreak(), text::LineBreak::None);
}

TEST(LineSequence, FromSnapshotEqualsFromBytes) {
    SplitMix64 rng(0xD1FF0001);
    const std::uint32_t cases = scaled(2000, 500, 300);
    for (std::uint32_t c = 0; c < cases; ++c) {
        const PairClass kind = kAllPairClasses[rng.below(std::size(kAllPairClasses))];
        TextPair pair = generatePair(kind, rng);
        if (!validUtf8(pair.a)) {
            continue; // TextBuffer holds only well-formed UTF-8.
        }
        auto buffer = text::TextBuffer::fromUtf8(pair.a);
        ASSERT_TRUE(buffer.has_value());
        for (const LineSplit split : {LineSplit::Lsp, LineSplit::Lf}) {
            auto fromSnapshot = LineSequence::fromSnapshot(buffer->snapshot(), split);
            auto fromBytes = LineSequence::fromBytes(pair.a, split);
            ASSERT_TRUE(fromSnapshot.has_value()) << fromSnapshot.error().describe();
            ASSERT_TRUE(fromBytes.has_value());
            EXPECT_EQ(fromSnapshot->bytes(), fromBytes->bytes());
            EXPECT_EQ(linesOf(*fromSnapshot), linesOf(*fromBytes));
            EXPECT_EQ(linesOf(*fromBytes), naiveSplit(pair.a, split)) << printable(pair.a);
            EXPECT_EQ(fromSnapshot->firstLineBreak(), fromBytes->firstLineBreak());
            for (std::uint32_t i = 0; i < fromBytes->size(); ++i) {
                ASSERT_EQ(fromSnapshot->lineHash(LineIndex{i}), fromBytes->lineHash(LineIndex{i}));
            }
        }
    }
}

TEST(LineSequence, SnapshotLineMapping) {
    SplitMix64 rng(0xD1FF0002);
    const std::uint32_t cases = scaled(1000, 300, 200);
    for (std::uint32_t c = 0; c < cases; ++c) {
        TextPair pair = generatePair(c % 2 == 0 ? PairClass::LoneCr : PairClass::CodeLike, rng);
        if (!validUtf8(pair.a)) {
            continue;
        }
        auto buffer = text::TextBuffer::fromUtf8(pair.a);
        ASSERT_TRUE(buffer.has_value());
        const text::Snapshot snapshot = buffer->snapshot();
        auto sequence = LineSequence::fromSnapshot(snapshot, LineSplit::Lsp);
        ASSERT_TRUE(sequence.has_value());
        // size() == lineCount() - 1 when the text is empty or ends with a break, else lineCount().
        const bool endsWithBreak = pair.a.empty() || pair.a.back() == '\n' || pair.a.back() == '\r';
        EXPECT_EQ(sequence->size(), snapshot.lineCount() - (endsWithBreak ? 1u : 0u)) << printable(pair.a);
        for (std::uint32_t i = 0; i < sequence->size(); ++i) {
            auto info = snapshot.lineInfo(LineIndex{i});
            ASSERT_TRUE(info.has_value());
            const std::size_t breakBytes = info->lineBreak == text::LineBreak::CrLf   ? 2
                                           : info->lineBreak == text::LineBreak::None ? 0
                                                                                      : 1;
            const text::ByteRange expected{info->content.begin, ByteOffset{info->content.end.value() + breakBytes}};
            EXPECT_EQ(sequence->lineRange(LineIndex{i}), expected);
            EXPECT_EQ(sequence->line(LineIndex{i}), snapshot.text(expected));
        }
    }
}

TEST(LineSequence, Limits) {
    SequenceLimits limits;
    limits.maxBytes = 10;
    auto tooBig = LineSequence::fromBytes(std::string(11, 'x'), LineSplit::Lsp, limits);
    ASSERT_FALSE(tooBig.has_value());
    EXPECT_EQ(tooBig.error().code(), ErrorCode::OutOfRange);
    EXPECT_TRUE(LineSequence::fromBytes(std::string(10, 'x'), LineSplit::Lsp, limits).has_value());

    // maxLines is checked by the counting pass, before the line table exists: 1M LFs, maxLines 4.
    limits = SequenceLimits{};
    limits.maxLines = 4;
    for (const LineSplit split : {LineSplit::Lsp, LineSplit::Lf}) {
        auto many = LineSequence::fromBytes(std::string(1u << 20, '\n'), split, limits);
        ASSERT_FALSE(many.has_value());
        EXPECT_EQ(many.error().code(), ErrorCode::OutOfRange);
        EXPECT_TRUE(LineSequence::fromBytes("a\nb\nc\nd\n", split, limits).has_value());
        EXPECT_TRUE(LineSequence::fromBytes("a\nb\nc\nd", split, limits).has_value());
        EXPECT_FALSE(LineSequence::fromBytes("a\nb\nc\nd\ne", split, limits).has_value());
    }
    // Lone CRs count under Lsp only.
    EXPECT_FALSE(LineSequence::fromBytes("\r\r\r\r\r", LineSplit::Lsp, limits).has_value());
    EXPECT_TRUE(LineSequence::fromBytes("\r\r\r\r\r", LineSplit::Lf, limits).has_value());

    // fromSnapshot checks size and line count before copying.
    auto buffer = text::TextBuffer::fromUtf8(std::string(1000, '\n'));
    ASSERT_TRUE(buffer.has_value());
    limits = SequenceLimits{};
    limits.maxLines = 999;
    auto lines = LineSequence::fromSnapshot(buffer->snapshot(), LineSplit::Lf, limits);
    ASSERT_FALSE(lines.has_value());
    EXPECT_EQ(lines.error().code(), ErrorCode::OutOfRange);
    limits.maxLines = 1000;
    EXPECT_TRUE(LineSequence::fromSnapshot(buffer->snapshot(), LineSplit::Lf, limits).has_value());
    EXPECT_TRUE(LineSequence::fromSnapshot(buffer->snapshot(), LineSplit::Lsp, limits).has_value());
    limits.maxBytes = 999;
    auto bytes = LineSequence::fromSnapshot(buffer->snapshot(), LineSplit::Lsp, limits);
    ASSERT_FALSE(bytes.has_value());
    EXPECT_EQ(bytes.error().code(), ErrorCode::OutOfRange);
}

// fromSnapshot's O(1) line count from the snapshot summary, at the boundary: maxLines equal to the
// count the splitter finds passes and one less fails, for texts whose last line ends in a lone CR,
// lacks a break, or ends in CRLF, under both splits (large texts span several chunks).
TEST(LineSequence, SnapshotCountAtBoundary) {
    std::string many;
    for (std::uint32_t i = 0; i < 30000; ++i) {
        many += i % 3 == 0 ? "line\r\n" : i % 3 == 1 ? "x\r" : "y\n";
    }
    const std::string texts[] = {
        "a\rb\r",     "a\nb\r", "a\r",    "\r",           "a\nb",        "a\r\nb",      "a\rb",
        "a\r\nb\r\n", "a\n\r",  "\r\n\r", many + "end\r", many + "tail", many + "\r\n", many,
    };
    for (const std::string& text : texts) {
        auto buffer = text::TextBuffer::fromUtf8(text);
        ASSERT_TRUE(buffer.has_value());
        for (const LineSplit split : {LineSplit::Lsp, LineSplit::Lf}) {
            auto counted = LineSequence::fromBytes(text, split);
            ASSERT_TRUE(counted.has_value());
            const std::uint32_t n = counted->size();
            ASSERT_GT(n, 0u);
            SequenceLimits limits;
            limits.maxLines = n;
            auto fits = LineSequence::fromSnapshot(buffer->snapshot(), split, limits);
            ASSERT_TRUE(fits.has_value()) << printable(text, 40) << ": " << fits.error().describe();
            EXPECT_EQ(fits->size(), n) << printable(text, 40);
            EXPECT_EQ(fits->bytes(), text);
            limits.maxLines = n - 1;
            auto over = LineSequence::fromSnapshot(buffer->snapshot(), split, limits);
            ASSERT_FALSE(over.has_value()) << printable(text, 40) << " with maxLines " << n - 1;
            EXPECT_EQ(over.error().code(), ErrorCode::OutOfRange);
        }
    }
}

TEST(LineSequence, CopiesShareStorage) {
    // SSO-sized and heap-sized texts: views stay valid across copies and moves of the sequence.
    for (const std::string& bytes : {std::string("ab\ncd"), std::string(5000, 'x') + "\nend"}) {
        auto made = LineSequence::fromBytes(bytes);
        ASSERT_TRUE(made.has_value());
        const std::string_view before = made->bytes();
        const std::string_view line = made->line(LineIndex{1});
        LineSequence copy = *made;
        LineSequence moved = std::move(*made);
        *made = LineSequence(); // Assigning to the moved-from object is allowed.
        EXPECT_EQ(copy.bytes().data(), before.data());
        EXPECT_EQ(moved.bytes().data(), before.data());
        EXPECT_EQ(moved.line(LineIndex{1}).data(), line.data());
        LineSequence relocated;
        relocated = std::move(moved);
        EXPECT_EQ(relocated.line(LineIndex{1}), line);
        EXPECT_EQ(std::string(line), bytes.substr(bytes.find('\n') + 1));
        std::vector<LineSequence> many(64, copy); // Reallocation moves the elements.
        many.resize(4096);
        EXPECT_EQ(many[0].bytes().data(), before.data());
    }
}

TEST(LineSequence, FirstLineBreak) {
    struct Case {
        std::string text;
        text::LineBreak lsp;
        text::LineBreak lf;
    };
    const Case cases[] = {
        {"", text::LineBreak::None, text::LineBreak::None},
        {"abc", text::LineBreak::None, text::LineBreak::None},
        {"a\nb\r\n", text::LineBreak::Lf, text::LineBreak::Lf},
        {"a\r\nb\n", text::LineBreak::CrLf, text::LineBreak::CrLf},
        {"a\rb\n", text::LineBreak::Cr, text::LineBreak::Lf},
        {"a\rb\r\n", text::LineBreak::Cr, text::LineBreak::CrLf},
        {"a\r", text::LineBreak::Cr, text::LineBreak::None},
        {"\r\n", text::LineBreak::CrLf, text::LineBreak::CrLf},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(makeSequence(c.text, LineSplit::Lsp).firstLineBreak(), c.lsp) << printable(c.text);
        EXPECT_EQ(makeSequence(c.text, LineSplit::Lf).firstLineBreak(), c.lf) << printable(c.text);
    }
}

} // namespace
} // namespace frost::diff::test
