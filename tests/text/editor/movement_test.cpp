// SPDX-License-Identifier: Apache-2.0
//
// Movement and selection commands of the editor model (ADR 0025): by grapheme cluster and word, line
// starts and ends, vertical movement with a goal column (with and without the DisplayMap, also
// through soft-wrapped rows), pages, the text's ends, selection extension, Select All, Go to Line
// and its input parser, next/previous member through an IOutlineProvider double, undo restoring the
// selections, and a second Editor following the first's changes.

#include "frost/text/editor/commands.hpp"
#include "frost/text/editor/editor.hpp"
#include "frost/text/editor/outline.hpp"

#include "text/editor/editor_fixture.hpp"
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace {

using frost::ByteColumn;
using frost::ByteOffset;
using frost::ErrorCode;
using frost::LineIndex;
using frost::text::ByteRange;
using frost::text::CommandInput;
using frost::text::Editor;
using frost::text::EditorConfig;
using frost::text::IOutlineProvider;
using frost::text::Movement;
using frost::text::parseGoToLine;
using frost::text::Point;
using frost::text::Snapshot;
using frost::text::WrapMode;
using frost::text::testing::applied;
using frost::text::testing::EditorFixture;
namespace commands = frost::text::commands;

// "«" and "»" (editor_fixture.hpp's selection markers).
#define M_A "\xC2\xAB"
#define M_H "\xC2\xBB"

constexpr std::string_view kFamily = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";

std::string moved(std::string_view marked, Movement movement, bool extend = false) {
    return applied(marked, commands::move, movement, extend);
}

// The outline double: fixed member ranges.
class FixedOutline final : public IOutlineProvider {
public:
    explicit FixedOutline(std::vector<ByteRange> members) : m_members(std::move(members)) {}
    [[nodiscard]] std::vector<ByteRange> members(const Snapshot&) const noexcept override { return m_members; }

private:
    std::vector<ByteRange> m_members;
};

ByteRange range(std::uint64_t begin, std::uint64_t end) { return ByteRange{ByteOffset{begin}, ByteOffset{end}}; }

TEST(Movement, ByGraphemeCluster) {
    const std::string family(kFamily);
    EXPECT_EQ(moved("a|" + family + "b", Movement::CharRight), "a" + family + "|b");
    EXPECT_EQ(moved("a" + family + "|b", Movement::CharLeft), "a|" + family + "b");
    EXPECT_EQ(moved("e\xCC\x81|x", Movement::CharLeft), "|e\xCC\x81x");
    EXPECT_EQ(moved("a|\r\nb", Movement::CharRight), "a\r\n|b");
    EXPECT_EQ(moved("|ab", Movement::CharLeft), "|ab");
    EXPECT_EQ(moved("ab|", Movement::CharRight), "ab|");
    EXPECT_EQ(moved("a|" + family + "b", Movement::CharRight, true), "a" M_A + family + M_H "b");
}

TEST(Movement, CharMovesCollapseSelections) {
    EXPECT_EQ(moved("a" M_A "bc" M_H "d", Movement::CharLeft), "a|bcd");
    EXPECT_EQ(moved("a" M_H "bc" M_A "d", Movement::CharRight), "abc|d");
    EXPECT_EQ(moved("a" M_A "bc" M_H "d", Movement::CharRight, true), "a" M_A "bcd" M_H);
}

TEST(Movement, ByWord) {
    EXPECT_EQ(moved("foo |bar baz", Movement::WordRight), "foo bar| baz");
    EXPECT_EQ(moved("foo bar| baz", Movement::WordLeft), "foo |bar baz");
    EXPECT_EQ(moved("fo|o bar", Movement::WordRight, true), "fo" M_A "o" M_H " bar");
    EXPECT_EQ(moved("a.b|c d", Movement::WordLeft, true), "a." M_H "b" M_A "c d");
    EXPECT_EQ(moved("x|y z|w", Movement::WordRight), "xy| zw|");
}

TEST(Movement, LineEndsAndTextEnds) {
    EXPECT_EQ(moved("ab\nc|d\ne", Movement::LineEnd), "ab\ncd|\ne");
    EXPECT_EQ(moved("ab\nc|d\r\ne", Movement::LineEnd, true), "ab\nc" M_A "d" M_H "\r\ne");
    EXPECT_EQ(moved("ab\nc|d\ne", Movement::TextStart), "|ab\ncd\ne");
    EXPECT_EQ(moved("ab\nc|d\ne", Movement::TextEnd, true), "ab\nc" M_A "d\ne" M_H);
}

TEST(Movement, VerticalKeepsTheGoalColumn) {
    EditorFixture fixture("abcdef|\nab\nabcdefgh");
    fixture.run(commands::move, Movement::LineDown, false);
    EXPECT_EQ(fixture.show(), "abcdef\nab|\nabcdefgh");
    fixture.run(commands::move, Movement::LineDown, false);
    EXPECT_EQ(fixture.show(), "abcdef\nab\nabcdef|gh");
    fixture.run(commands::move, Movement::LineDown, false); // The last row: the caret stays.
    EXPECT_EQ(fixture.show(), "abcdef\nab\nabcdef|gh");
    fixture.run(commands::move, Movement::LineUp, true);
    EXPECT_EQ(fixture.show(), "abcdef\nab" M_H "\nabcdef" M_A "gh");
    EXPECT_EQ(moved("a|b\ncd", Movement::LineUp), "a|b\ncd"); // The first row: the caret stays.
}

TEST(Movement, VerticalWithoutADisplayMapCountsCodePoints) {
    EditorFixture fixture("x\xC3\xA9y|\nabcd");
    CommandInput input = fixture.editor().input();
    input.display = nullptr;
    const frost::text::CommandResult down = commands::move(input, Movement::LineDown, false);
    ASSERT_TRUE(fixture.editor().apply(down).has_value());
    EXPECT_EQ(fixture.show(), "x\xC3\xA9y\nabc|d");
}

TEST(Movement, VerticalThroughSoftWrappedRows) {
    EditorConfig config;
    config.wrap.mode = WrapMode::Column;
    config.wrap.columns = 4;
    EditorFixture fixture("a|bcdefghij\nxy", frost::text::languageById("cpp"), config);
    ASSERT_GT(fixture.editor().display().rowCount(), 2u);
    fixture.run(commands::move, Movement::LineDown, false);
    const ByteOffset second = fixture.editor().state().primary().head;
    EXPECT_GT(second, ByteOffset{1});
    EXPECT_LT(second, ByteOffset{10}); // Still on the first buffer line, one row down.
    fixture.run(commands::move, Movement::LineUp, false);
    EXPECT_EQ(fixture.show(), "a|bcdefghij\nxy");
}

// Rider keeps the caret where it is on Up at the first row and Down at the last row (ADR 0037),
// with and without soft wrap.
TEST(Movement, UpOnTheFirstRowAndDownOnTheLastRowStay) {
    EXPECT_EQ(moved("ab|c\nde", Movement::LineUp), "ab|c\nde");
    EXPECT_EQ(moved("abc\nd|e", Movement::LineDown), "abc\nd|e");
    EXPECT_EQ(moved("ab|c", Movement::LineUp), "ab|c");
    EXPECT_EQ(moved("ab|c", Movement::LineDown), "ab|c");
    EditorConfig config;
    config.wrap.mode = WrapMode::Column;
    config.wrap.columns = 4;
    EditorFixture first("ab|cdefghij", frost::text::languageById("cpp"), config);
    ASSERT_GT(first.editor().display().rowCount(), 2u);
    first.run(commands::move, Movement::LineUp, false);
    EXPECT_EQ(first.show(), "ab|cdefghij");
    EditorFixture last("abcdefgh|ij", frost::text::languageById("cpp"), config);
    last.run(commands::move, Movement::LineDown, false);
    EXPECT_EQ(last.show(), "abcdefgh|ij");
}

TEST(Movement, Pages) {
    EditorConfig config;
    config.pageRows = 2;
    EditorFixture fixture("a|0\na1\na2\na3\na4", frost::text::languageById("cpp"), config);
    fixture.run(commands::move, Movement::PageDown, false);
    EXPECT_EQ(fixture.show(), "a0\na1\na|2\na3\na4");
    fixture.run(commands::move, Movement::PageDown, true);
    EXPECT_EQ(fixture.show(), "a0\na1\na" M_A "2\na3\na" M_H "4");
    fixture.run(commands::move, Movement::PageUp, false);
    EXPECT_EQ(fixture.show(), "a0\na1\na|2\na3\na4");
}

TEST(Selection, SelectAll) {
    EXPECT_EQ(applied("ab|c\nd", commands::selectAll), M_A "abc\nd" M_H);
    EXPECT_EQ(applied("|", commands::selectAll), "|");
}

TEST(GoToLine, MovesToThePoint) {
    EXPECT_EQ(applied("|a\nbcd\ne", commands::goToLine, Point{LineIndex{1}, ByteColumn{2}}), "a\nbc|d\ne");
    EXPECT_EQ(applied("|a\nbcd\ne", commands::goToLine, Point{LineIndex{1}, ByteColumn{9}}), "a\nbcd|\ne");
    EXPECT_EQ(applied("|a\nbcd\ne", commands::goToLine, Point{LineIndex{7}, ByteColumn{0}}), "a\nbcd\n|e");
    EXPECT_EQ(applied("a|\nb|", commands::goToLine, Point{LineIndex{0}, ByteColumn{0}}), "|a\nb");
    EXPECT_EQ(applied("|\xC3\xA9", commands::goToLine, Point{LineIndex{0}, ByteColumn{1}}), "|\xC3\xA9");
}

TEST(GoToLine, ParsesInput) {
    EXPECT_EQ(parseGoToLine("12").value(), (Point{LineIndex{11}, ByteColumn{0}}));
    EXPECT_EQ(parseGoToLine(" 3:4 ").value(), (Point{LineIndex{2}, ByteColumn{3}}));
    EXPECT_EQ(parseGoToLine("3,4").value(), (Point{LineIndex{2}, ByteColumn{3}}));
    for (const std::string_view bad : {"", " ", "0", "x", "1:", "1:0", "-1", "1:2:3", "99999999999"}) {
        const auto parsed = parseGoToLine(bad);
        ASSERT_FALSE(parsed.has_value()) << bad;
        EXPECT_EQ(parsed.error().code(), ErrorCode::InvalidArgument) << bad;
    }
}

TEST(Members, NextAndPreviousThroughTheOutline) {
    const FixedOutline outline({range(10, 20), range(30, 40)});
    EditorFixture fixture(std::string(50, 'x'));
    fixture.run(commands::nextMember, outline, false);
    EXPECT_EQ(fixture.editor().state().primary().head, ByteOffset{10});
    fixture.run(commands::nextMember, outline, false);
    EXPECT_EQ(fixture.editor().state().primary().head, ByteOffset{30});
    fixture.run(commands::nextMember, outline, false); // No member after the last.
    EXPECT_EQ(fixture.editor().state().primary().head, ByteOffset{30});
    fixture.run(commands::previousMember, outline, true);
    EXPECT_EQ(fixture.editor().state().primary().anchor, ByteOffset{30});
    EXPECT_EQ(fixture.editor().state().primary().head, ByteOffset{10});
    fixture.run(commands::previousMember, outline, false);
    EXPECT_EQ(fixture.editor().state().primary().head, ByteOffset{10});
}

TEST(History, UndoRestoresTheSelections) {
    EditorFixture fixture("a" M_A "foo" M_H " b");
    fixture.typeChars("x");
    EXPECT_EQ(fixture.show(), "ax| b");
    fixture.run(commands::move, Movement::TextEnd, false);
    fixture.undo();
    EXPECT_EQ(fixture.show(), "a" M_A "foo" M_H " b");
    fixture.redo();
    EXPECT_EQ(fixture.show(), "ax| b");
}

TEST(History, ASecondEditorFollowsTheChanges) {
    EditorFixture fixture("ab|cd");
    Editor other(fixture.document(), fixture.shaper());
    other.setState(frost::text::EditorState::caretAt(ByteOffset{3}));
    // The first editor types at 2; the second follows each change, in order.
    for (const std::string_view ch : {"X", "Y"}) {
        const auto update = fixture.editor().run(commands::type, ch);
        ASSERT_TRUE(update.has_value());
        ASSERT_TRUE(update->change.has_value());
        ASSERT_TRUE(other.follow(*update->change).has_value());
    }
    EXPECT_EQ(fixture.text(), "abXYcd");
    EXPECT_EQ(other.state().primary().head, ByteOffset{5});
    EXPECT_EQ(other.display().version(), fixture.document().version());
    // A change that does not continue from the second editor's version is refused.
    const auto update = fixture.editor().run(commands::type, "Z");
    ASSERT_TRUE(update.has_value());
    ASSERT_TRUE(other.follow(*update->change).has_value());
    EXPECT_FALSE(other.follow(*update->change).has_value()); // Already followed.
    EXPECT_EQ(other.state().primary().head, ByteOffset{6});
}

#undef M_A
#undef M_H

TEST(History, GlobalRenameOnlyBarrierPreservesCaretsInEveryFollowingEditor) {
    EditorFixture fixture("ab|cd");
    Editor peer(fixture.document(),fixture.shaper());
    peer.setState(frost::text::EditorState::caretAt(ByteOffset{3}));
    std::array<std::byte,16> identity{};identity[0]=std::byte{8};
    const frost::TransactionId token(identity);
    auto& document=fixture.document();
    ASSERT_TRUE(document.acquireExternalWrite(token));
    ASSERT_TRUE(document.recordExternalBarrier(token));
    auto undone=document.undoExternal(token);ASSERT_TRUE(undone);
    ASSERT_TRUE(fixture.editor().follow(*undone));ASSERT_TRUE(peer.follow(*undone));
    EXPECT_EQ(fixture.editor().state().primary().head,ByteOffset{2});
    EXPECT_EQ(peer.state().primary().head,ByteOffset{3});
    auto redone=document.redoExternal(token);ASSERT_TRUE(redone);
    ASSERT_TRUE(fixture.editor().follow(*redone));ASSERT_TRUE(peer.follow(*redone));
    EXPECT_EQ(fixture.editor().state().primary().head,ByteOffset{2});
    EXPECT_EQ(peer.state().primary().head,ByteOffset{3});
    document.releaseExternalWrite(token);
}

} // namespace
