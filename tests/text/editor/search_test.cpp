// SPDX-License-Identifier: Apache-2.0
//
// Find and replace in a file (ADR 0025): plain, case-sensitive, whole-word and RE2 regex patterns,
// forward and backward over texts split into small pieces, multi-line regexes, replacement
// templates with groups, compile errors, and the find, replace and replace-all commands (wrapping,
// and replace all as one undo step).

#include "frost/text/editor/search.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_buffer.hpp"

#include "text/editor/editor_fixture.hpp"
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using frost::ByteOffset;
using frost::ErrorCode;
using frost::text::ByteRange;
using frost::text::FindResult;
using frost::text::SearchOptions;
using frost::text::SearchPattern;
using frost::text::Snapshot;
using frost::text::TextBuffer;
using frost::text::TextBufferOptions;
using frost::text::testing::EditorFixture;
namespace commands = frost::text::commands;

// "«" and "»" (editor_fixture.hpp's selection markers).
#define M_A "\xC2\xAB"
#define M_H "\xC2\xBB"

Snapshot snapshotOf(std::string text, std::uint32_t maxPieceBytes = 4096) {
    TextBufferOptions options;
    options.maxPieceBytes = maxPieceBytes;
    return TextBuffer::fromUtf8(std::move(text), options).value().snapshot();
}

SearchPattern compiled(std::string_view pattern, SearchOptions options = {}) {
    return SearchPattern::compile(pattern, options).value();
}

// The matches of `pattern` in `text`, as begin offsets.
std::vector<std::uint64_t> begins(const SearchPattern& pattern, const Snapshot& text) {
    std::vector<std::uint64_t> out;
    for (const ByteRange match : pattern.findAll(text)) {
        out.push_back(match.begin.value());
    }
    return out;
}

ByteRange range(std::uint64_t begin, std::uint64_t end) { return ByteRange{ByteOffset{begin}, ByteOffset{end}}; }

TEST(SearchPattern, PlainCaseAndWholeWord) {
    const Snapshot text = snapshotOf("Foo foo FOO food foo_bar (foo)");
    EXPECT_EQ(begins(compiled("foo"), text), (std::vector<std::uint64_t>{0, 4, 8, 12, 17, 26}));
    EXPECT_EQ(begins(compiled("foo", {.caseSensitive = true}), text), (std::vector<std::uint64_t>{4, 12, 17, 26}));
    EXPECT_EQ(begins(compiled("foo", {.wholeWord = true}), text), (std::vector<std::uint64_t>{0, 4, 8, 26}));
    EXPECT_EQ(begins(compiled("foo", {.caseSensitive = true, .wholeWord = true}), text),
              (std::vector<std::uint64_t>{4, 26}));
    // Regex metacharacters are literal in a plain pattern.
    EXPECT_EQ(begins(compiled("(foo)"), text), (std::vector<std::uint64_t>{25}));
    EXPECT_EQ(begins(compiled("\xC3\xA9", {.caseSensitive = false}), snapshotOf("\xC3\x89t\xC3\xA9")),
              (std::vector<std::uint64_t>{0, 3}));
}

TEST(SearchPattern, ForwardAndBackwardAcrossPieces) {
    for (const std::uint32_t pieceBytes : {4096u, 4u}) {
        const Snapshot text = snapshotOf("abcabc\nxabcx", pieceBytes);
        const SearchPattern plain = compiled("abc", {.caseSensitive = true});
        EXPECT_EQ(plain.findNext(text, ByteOffset{0}), range(0, 3));
        EXPECT_EQ(plain.findNext(text, ByteOffset{1}), range(3, 6));
        EXPECT_EQ(plain.findNext(text, ByteOffset{4}), range(8, 11));
        EXPECT_EQ(plain.findNext(text, ByteOffset{9}), std::nullopt);
        EXPECT_EQ(plain.findPrevious(text, text.size()), range(8, 11));
        EXPECT_EQ(plain.findPrevious(text, ByteOffset{10}), range(3, 6));
        EXPECT_EQ(plain.findPrevious(text, ByteOffset{6}), range(3, 6));
        EXPECT_EQ(plain.findPrevious(text, ByteOffset{2}), std::nullopt);
        const SearchPattern folded = compiled("ABC");
        EXPECT_EQ(folded.findPrevious(text, ByteOffset{10}), range(3, 6));
        EXPECT_EQ(folded.findNext(text, ByteOffset{7}), range(8, 11));
    }
}

TEST(SearchPattern, Regex) {
    const Snapshot text = snapshotOf("x=1, y=22\nint main() {}\n  z=3");
    const SearchPattern assign = compiled(R"((\w)=(\d+))", {.regex = true});
    EXPECT_EQ(begins(assign, text), (std::vector<std::uint64_t>{0, 5, 26}));
    EXPECT_EQ(assign.findPrevious(text, ByteOffset{20}), range(5, 9));
    EXPECT_EQ(assign.replacementFor(text, range(5, 9), "$2=$1 ($0) $$"), "22=y (y=22) $");
    // Anchors are per line; a pattern with a line break matches across lines.
    EXPECT_EQ(begins(compiled("^ *[a-z]", {.regex = true}), text), (std::vector<std::uint64_t>{0, 10, 24}));
    EXPECT_EQ(compiled(R"(22\n\w+)", {.regex = true}).findNext(text, ByteOffset{0}), range(7, 13));
    EXPECT_EQ(begins(compiled("MAIN", {.wholeWord = true, .regex = true}), text), (std::vector<std::uint64_t>{14}));
    EXPECT_TRUE(assign.matches(text, range(0, 3)));
    EXPECT_FALSE(assign.matches(text, range(0, 2)));
}

TEST(SearchPattern, CompileErrors) {
    for (const auto& [pattern, options] : {std::pair<std::string_view, SearchOptions>{"", {}},
                                           std::pair<std::string_view, SearchOptions>{"(", {.regex = true}},
                                           std::pair<std::string_view, SearchOptions>{"\xC3", {}}}) {
        const auto result = SearchPattern::compile(pattern, options);
        ASSERT_FALSE(result.has_value()) << pattern;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(FindCommands, NextAndPreviousWrap) {
    EditorFixture fixture("ab ab a|b");
    const SearchPattern pattern = compiled("ab");
    FindResult found = commands::findNext(fixture.editor().input(), pattern);
    EXPECT_TRUE(found.found);
    EXPECT_TRUE(found.wrapped);
    ASSERT_TRUE(fixture.editor().apply(found.command).has_value());
    EXPECT_EQ(fixture.show(), M_A "ab" M_H " ab ab");
    found = commands::findNext(fixture.editor().input(), pattern);
    EXPECT_FALSE(found.wrapped);
    ASSERT_TRUE(fixture.editor().apply(found.command).has_value());
    EXPECT_EQ(fixture.show(), "ab " M_A "ab" M_H " ab");
    found = commands::findPrevious(fixture.editor().input(), pattern);
    ASSERT_TRUE(fixture.editor().apply(found.command).has_value());
    EXPECT_EQ(fixture.show(), M_A "ab" M_H " ab ab");
    found = commands::findPrevious(fixture.editor().input(), pattern, false);
    EXPECT_FALSE(found.found);
    EXPECT_TRUE(commands::findNext(fixture.editor().input(), compiled("zz")).found == false);
}

TEST(FindCommands, ReplaceNext) {
    EditorFixture fixture("|a-a-a");
    const SearchPattern pattern = compiled("a", {.caseSensitive = true});
    FindResult step = commands::replaceNext(fixture.editor().input(), pattern, "bb");
    EXPECT_EQ(step.replaced, 0u); // The selection was not a match yet: select it first.
    ASSERT_TRUE(fixture.editor().apply(step.command).has_value());
    EXPECT_EQ(fixture.show(), M_A "a" M_H "-a-a");
    step = commands::replaceNext(fixture.editor().input(), pattern, "bb");
    EXPECT_EQ(step.replaced, 1u);
    ASSERT_TRUE(fixture.editor().apply(step.command).has_value());
    EXPECT_EQ(fixture.show(), "bb-" M_A "a" M_H "-a");
}

TEST(FindCommands, ReplaceAllIsOneUndoStep) {
    EditorFixture fixture("x=1, y=2|");
    const SearchPattern pattern = compiled(R"((\w)=(\d))", {.regex = true});
    const FindResult all = commands::replaceAll(fixture.editor().input(), pattern, "$2:$1");
    EXPECT_EQ(all.replaced, 2u);
    ASSERT_TRUE(fixture.editor().apply(all.command).has_value());
    EXPECT_EQ(fixture.show(), "1:x, 2:y|");
    fixture.undo();
    EXPECT_EQ(fixture.show(), "x=1, y=2|");
    EXPECT_FALSE(fixture.document().undoState().canUndo);
    const FindResult none = commands::replaceAll(fixture.editor().input(), compiled("q"), "r");
    EXPECT_EQ(none.replaced, 0u);
    EXPECT_FALSE(none.command.changesText());
}

#undef M_A
#undef M_H

} // namespace
