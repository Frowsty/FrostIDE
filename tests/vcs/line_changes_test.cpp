// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the editor's change markers (line_changes.hpp) and the side-by-side diff model
// (side_by_side.hpp): added, modified and deleted lines, rolling a change back, the marker under a
// line, the blocks and their word highlights, the scroll mapping and next/previous change.

#include "frost/diff/line_sequence.hpp"
#include "frost/vcs/line_changes.hpp"
#include "frost/vcs/side_by_side.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using frost::LineIndex;
using frost::diff::LineRange;
using frost::diff::LineSequence;
using frost::diff::LineSplit;
using namespace frost::vcs;

LineSequence lsp(std::string text) { return LineSequence::fromBytes(std::move(text), LineSplit::Lsp).value(); }

LineRange lines(std::uint32_t begin, std::uint32_t end) { return LineRange{LineIndex{begin}, LineIndex{end}}; }

// Applies `edit` to `text`.
std::string applied(std::string text, const LineRollback& edit) {
    const auto begin = static_cast<std::size_t>(edit.range.begin.value());
    text.replace(begin, static_cast<std::size_t>(edit.range.size()), edit.text);
    return text;
}

const std::string& baseText() {
    static const std::string value("one\ntwo\nthree\nfour\nfive\nsix\n");
    return value;
}

TEST(LineChanges, AddedModifiedAndDeletedLines) {
    const std::string current = "zero\none\ntwo\nTHREE\nfour\nsix\n";
    const LineSequence base = lsp(baseText());
    const LineSequence buffer = lsp(current);
    auto changes = computeLineChanges(base, buffer);
    ASSERT_TRUE(changes.has_value()) << changes.error().describe();
    ASSERT_EQ(changes->size(), 3U);
    EXPECT_EQ((*changes)[0], (LineChange{LineChangeKind::Added, lines(0, 1), lines(0, 0)}));
    EXPECT_EQ((*changes)[1], (LineChange{LineChangeKind::Modified, lines(3, 4), lines(2, 3)}));
    EXPECT_EQ((*changes)[2], (LineChange{LineChangeKind::Deleted, lines(5, 5), lines(4, 5)}));
    EXPECT_EQ(toString((*changes)[2].kind), "deleted");
    EXPECT_EQ(baseText(base, (*changes)[1]), "three\n");
    EXPECT_EQ(baseText(base, (*changes)[2]), "five\n");
    EXPECT_EQ(baseText(base, (*changes)[0]), "");

    // The marker under each line: line 5 ("six") carries the deletion mark above it.
    EXPECT_EQ(changeAtLine(*changes, LineIndex{0}), &(*changes)[0]);
    EXPECT_EQ(changeAtLine(*changes, LineIndex{1}), nullptr);
    EXPECT_EQ(changeAtLine(*changes, LineIndex{3}), &(*changes)[1]);
    EXPECT_EQ(changeAtLine(*changes, LineIndex{5}), &(*changes)[2]);
    EXPECT_EQ(changeAtLine(*changes, LineIndex{6}), nullptr);

    auto same = computeLineChanges(base, lsp(baseText()));
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(same->empty());
}

TEST(LineChanges, RollbackRestoresEachChange) {
    const std::string current = "zero\none\ntwo\nTHREE\nfour\nsix\n";
    const LineSequence base = lsp(baseText());
    const LineSequence buffer = lsp(current);
    auto changes = computeLineChanges(base, buffer);
    ASSERT_TRUE(changes.has_value()) << changes.error().describe();
    ASSERT_EQ(changes->size(), 3U);
    EXPECT_EQ(applied(current, rollbackEdit(base, buffer, (*changes)[0])), "one\ntwo\nTHREE\nfour\nsix\n");
    EXPECT_EQ(applied(current, rollbackEdit(base, buffer, (*changes)[1])), "zero\none\ntwo\nthree\nfour\nsix\n");
    EXPECT_EQ(applied(current, rollbackEdit(base, buffer, (*changes)[2])), "zero\none\ntwo\nTHREE\nfour\nfive\nsix\n");

    // Rolling every change back, last first, gives the base.
    std::string text = current;
    for (auto it = changes->rbegin(); it != changes->rend(); ++it) {
        text = applied(text, rollbackEdit(base, buffer, *it));
    }
    EXPECT_EQ(text, baseText());
}

TEST(LineChanges, DeletionAtTheEndAndAMissingFinalBreak) {
    const LineSequence base = lsp("a\nb\nc");
    const LineSequence buffer = lsp("a\n");
    auto changes = computeLineChanges(base, buffer);
    ASSERT_TRUE(changes.has_value()) << changes.error().describe();
    ASSERT_EQ(changes->size(), 1U);
    EXPECT_EQ((*changes)[0].kind, LineChangeKind::Deleted);
    EXPECT_EQ((*changes)[0].current, lines(1, 1));
    EXPECT_EQ(applied("a\n", rollbackEdit(base, buffer, (*changes)[0])), "a\nb\nc");
}

TEST(SideBySide, BlocksWordHighlightsAndNavigation) {
    const std::string left = "int a = 1;\nint b = 2;\nkeep\nold line\nkeep 2\ntail\n";
    const std::string right = "int a = 10;\nint b = 2;\nkeep\nkeep 2\nnew 1\nnew 2\ntail\n";
    auto diff = SideBySideDiff::compute(left, right);
    ASSERT_TRUE(diff.has_value()) << diff.error().describe();
    const auto blocks = diff->blocks();
    ASSERT_EQ(blocks.size(), 3U);
    EXPECT_EQ(blocks[0].kind(), LineChangeKind::Modified);
    EXPECT_EQ(blocks[1].kind(), LineChangeKind::Deleted);
    EXPECT_EQ(blocks[2].kind(), LineChangeKind::Added);
    EXPECT_EQ(blocks[1].hunk.a, lines(3, 4));
    EXPECT_EQ(blocks[1].hunk.b, lines(3, 3));

    // The word highlight of "1" -> "10".
    ASSERT_EQ(blocks[0].inner.size(), 1U);
    const auto& inner = blocks[0].inner.front();
    EXPECT_EQ(left.substr(inner.a.begin.value(), inner.a.size()), "1");
    EXPECT_EQ(right.substr(inner.b.begin.value(), inner.b.size()), "10");
    EXPECT_TRUE(blocks[1].inner.empty());

    // Scroll mapping: unchanged lines meet their twins; inside a block the other side's block.
    EXPECT_EQ(diff->mapLine(DiffSide::Left, LineIndex{2}), LineIndex{2});
    EXPECT_EQ(diff->mapLine(DiffSide::Left, LineIndex{3}), LineIndex{3});
    EXPECT_EQ(diff->mapLine(DiffSide::Left, LineIndex{4}), LineIndex{3});
    EXPECT_EQ(diff->mapLine(DiffSide::Left, LineIndex{5}), LineIndex{6});
    EXPECT_EQ(diff->mapLine(DiffSide::Right, LineIndex{5}), LineIndex{5});
    EXPECT_EQ(diff->mapLine(DiffSide::Right, LineIndex{6}), LineIndex{5});
    EXPECT_EQ(diff->mapLine(DiffSide::Right, LineIndex{99}),
              LineIndex{static_cast<std::uint32_t>(diff->left().size())});

    // F7 / Shift+F7.
    EXPECT_EQ(diff->blockAfter(DiffSide::Right, LineIndex{0}), std::optional<std::size_t>(1));
    EXPECT_EQ(diff->blockAfter(DiffSide::Right, LineIndex{3}), std::optional<std::size_t>(2));
    EXPECT_EQ(diff->blockAfter(DiffSide::Right, LineIndex{4}), std::nullopt);
    EXPECT_EQ(diff->blockBefore(DiffSide::Right, LineIndex{4}), std::optional<std::size_t>(1));
    EXPECT_EQ(diff->blockBefore(DiffSide::Right, LineIndex{0}), std::nullopt);
    EXPECT_EQ(diff->blockAt(DiffSide::Right, LineIndex{5}), std::optional<std::size_t>(2));
    EXPECT_EQ(diff->blockAt(DiffSide::Right, LineIndex{1}), std::nullopt);
    EXPECT_EQ(diff->hunks().size(), 3U);
}

TEST(SideBySide, IdenticalAndEmptyTexts) {
    auto same = SideBySideDiff::compute("a\nb\n", "a\nb\n");
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(same->blocks().empty());
    EXPECT_EQ(same->mapLine(DiffSide::Left, LineIndex{1}), LineIndex{1});
    auto added = SideBySideDiff::compute("", "new\nfile\n");
    ASSERT_TRUE(added.has_value());
    ASSERT_EQ(added->blocks().size(), 1U);
    EXPECT_EQ(added->blocks()[0].kind(), LineChangeKind::Added);
    EXPECT_EQ(added->blocks()[0].hunk.b, lines(0, 2));
}

} // namespace
