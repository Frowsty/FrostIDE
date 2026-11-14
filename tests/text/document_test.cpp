// SPDX-License-Identifier: Apache-2.0
//
// Document (ADR 0017): undo and redo with precise changes, branches, the error order, the
// ChangeSet table, the history cap, stateId, anchor and selection restoration, coalescing, groups,
// IME composition and DocumentSnapshot. Edge cases by hand; the differential covers the rest.

#include "frost/core/clock.hpp"
#include "frost/text/document.hpp"

#include "text/support/text_oracle.hpp"
#include "text/support/undo_ops.hpp"
#include "undo_tree.hpp"
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ByteOffset;
using frost::ErrorCode;
using frost::ManualClock;
using frost::Result;
using frost::text::AnchorId;
using frost::text::Bias;
using frost::text::ByteRange;
using frost::text::ChangeOrigin;
using frost::text::ChangeSet;
using frost::text::Document;
using frost::text::DocumentOptions;
using frost::text::Edit;
using frost::text::EditContext;
using frost::text::EditKind;
using frost::text::RawEdit;
using frost::text::Selection;
using frost::text::SelectionSet;
using frost::text::TextBufferOptions;
using frost::text::TextChange;
using frost::text::test::checkLspReplayByExtraction;
using frost::text::test::expectedChange;

ByteRange range(std::uint64_t a, std::uint64_t b) { return ByteRange{ByteOffset{a}, ByteOffset{b}}; }
RawEdit raw(std::uint64_t a, std::uint64_t b, std::uint64_t n) { return RawEdit{range(a, b), n}; }
SelectionSet caret(std::uint64_t at) { return SelectionSet::caret(ByteOffset{at}); }
std::string text(const Document& doc) { return doc.text().text(); }
std::uint64_t at(const Document& doc, AnchorId id) { return doc.anchors().resolve(id).value().value(); }

::testing::AssertionResult valid(const Document& doc) {
    if (const Result<void> checked = doc.checkInvariants(); !checked) {
        return ::testing::AssertionFailure() << checked.error().describe();
    }
    return ::testing::AssertionSuccess();
}

// Types `s` at `offset` as Typing with carets before and after.
ChangeSet type(Document& doc, std::uint64_t offset, std::string_view s) {
    return doc.replace(range(offset, offset), s, EditContext{EditKind::Typing, caret(offset), caret(offset + s.size())})
        .value();
}

struct Fixture {
    explicit Fixture(std::string initial, DocumentOptions options = {}, TextBufferOptions buffer = {})
        : doc(Document::fromUtf8(std::move(initial), buffer, options, clock).value()) {}
    ManualClock clock;
    Document doc;
};

// --- DocumentUndo ---------------------------------------------------------------------------------

TEST(DocumentUndo, UndoRedoSingleEdit) {
    Document doc = Document::fromUtf8("hello").value();
    EXPECT_FALSE(doc.undoState().canUndo);
    const ChangeSet edit =
        doc.replace(range(5, 5), " world", EditContext{EditKind::Other, caret(5), caret(11)}).value();
    EXPECT_EQ(edit.baseVersion, 0u);
    EXPECT_EQ(edit.version, 1u);
    EXPECT_EQ(text(doc), "hello world");
    EXPECT_TRUE(doc.undoState().canUndo);
    const ChangeSet undo = doc.undo().value();
    EXPECT_EQ(undo.origin, ChangeOrigin::Undo);
    EXPECT_EQ(undo.baseVersion, 1u);
    EXPECT_EQ(undo.version, 2u);
    EXPECT_EQ(undo.edits, (std::vector{raw(5, 11, 0)}));
    ASSERT_EQ(undo.changes.size(), 1u);
    EXPECT_EQ(undo.changes[0].start, ByteOffset{5});
    EXPECT_EQ(undo.changes[0].oldEnd, ByteOffset{11});
    EXPECT_EQ(undo.changes[0].newEnd, ByteOffset{5});
    EXPECT_EQ(undo.selections, caret(5));
    EXPECT_EQ(text(doc), "hello");
    EXPECT_FALSE(doc.undoState().canUndo);
    EXPECT_TRUE(doc.undoState().canRedo);
    const ChangeSet redo = doc.redo().value();
    EXPECT_EQ(redo.origin, ChangeOrigin::Redo);
    EXPECT_EQ(redo.edits, (std::vector{raw(5, 5, 6)}));
    EXPECT_EQ(redo.selections, caret(11));
    EXPECT_EQ(text(doc), "hello world");
    EXPECT_FALSE(doc.undoState().canRedo);
    EXPECT_TRUE(valid(doc));
}

TEST(DocumentUndo, PreciseChangesForTypingRun) {
    Fixture f("x");
    for (std::uint64_t i = 0; i < 1000; ++i) {
        (void)type(f.doc, 1 + i, "a");
        f.clock.advance(10ms);
    }
    EXPECT_EQ(f.doc.undoState().transactions, 1u);
    ASSERT_TRUE(valid(f.doc));
    const std::string before = text(f.doc);
    const ChangeSet undo = f.doc.undo().value();
    ASSERT_EQ(undo.changes.size(), 1u); // One precise change, not a whole-text one.
    EXPECT_EQ(undo.changes[0].start, ByteOffset{1});
    EXPECT_EQ(undo.changes[0].oldEnd, ByteOffset{1001});
    EXPECT_EQ(undo.changes[0].newEnd, ByteOffset{1});
    EXPECT_EQ(text(f.doc), "x");
    EXPECT_EQ(checkLspReplayByExtraction(before, text(f.doc), undo.changes), std::nullopt);
}

TEST(DocumentUndo, PreciseChangesCrlf) {
    const auto expectChange = [](const TextChange& c, std::uint64_t start, std::uint64_t oldEnd, std::uint64_t newEnd,
                                 const char* what) {
        EXPECT_EQ(c.start, ByteOffset{start}) << what;
        EXPECT_EQ(c.oldEnd, ByteOffset{oldEnd}) << what;
        EXPECT_EQ(c.newEnd, ByteOffset{newEnd}) << what;
    };
    {
        // "\n" typed after "\r": the change widens to the CR in both directions.
        Document doc = Document::fromUtf8("a\r").value();
        (void)doc.replace(range(2, 2), "\n", EditContext{}).value();
        const ChangeSet undo = doc.undo().value();
        ASSERT_EQ(undo.changes.size(), 1u);
        expectChange(undo.changes[0], 1, 3, 2, "undo of LF after CR");
        EXPECT_EQ(undo.changes[0].startPoint.line.value(), 0u);
        EXPECT_EQ(undo.changes[0].oldEndPoint.line.value(), 1u);
        EXPECT_EQ(checkLspReplayByExtraction("a\r\n", "a\r", undo.changes), std::nullopt);
        const ChangeSet redo = doc.redo().value();
        ASSERT_EQ(redo.changes.size(), 1u);
        expectChange(redo.changes[0], 1, 2, 3, "redo of LF after CR");
        EXPECT_EQ(checkLspReplayByExtraction("a\r", "a\r\n", redo.changes), std::nullopt);
        const auto e = expectedChange("a\r\n", 2, 3, "");
        EXPECT_EQ(e.start, 1u);
    }
    {
        // "x" inserted between CR and LF.
        Document doc = Document::fromUtf8("a\r\nb").value();
        (void)doc.replace(range(2, 2), "x", EditContext{}).value();
        EXPECT_EQ(text(doc), "a\rx\nb");
        const ChangeSet undo = doc.undo().value();
        ASSERT_EQ(undo.changes.size(), 1u);
        expectChange(undo.changes[0], 1, 4, 3, "undo of x between CR and LF");
        EXPECT_EQ(checkLspReplayByExtraction("a\rx\nb", "a\r\nb", undo.changes), std::nullopt);
        const ChangeSet redo = doc.redo().value();
        ASSERT_EQ(redo.changes.size(), 1u);
        expectChange(redo.changes[0], 1, 3, 4, "redo of x between CR and LF");
        EXPECT_EQ(checkLspReplayByExtraction("a\r\nb", "a\rx\nb", redo.changes), std::nullopt);
    }
}

TEST(DocumentUndo, MultiCaretRegions) {
    Document doc = Document::fromUtf8("aaa bbb ccc").value();
    const std::vector<Edit> edits{{range(3, 3), "X"}, {range(7, 7), "X"}, {range(11, 11), "X"}};
    const ChangeSet edit = doc.edit(edits, EditContext{EditKind::Typing, {}, {}}).value();
    EXPECT_EQ(text(doc), "aaaX bbbX cccX");
    EXPECT_EQ(edit.changes.size(), 3u);
    EXPECT_EQ(doc.undoTreeForTesting().currentNode().regions.size(), 3u);
    const ChangeSet undo = doc.undo().value();
    EXPECT_EQ(undo.edits, (std::vector{raw(3, 4, 0), raw(8, 9, 0), raw(13, 14, 0)}));
    ASSERT_EQ(undo.changes.size(), 3u); // In application order: last region first.
    EXPECT_EQ(undo.changes[0].start, ByteOffset{13});
    EXPECT_EQ(undo.changes[1].start, ByteOffset{8});
    EXPECT_EQ(undo.changes[2].start, ByteOffset{3});
    EXPECT_EQ(text(doc), "aaa bbb ccc");
    EXPECT_EQ(checkLspReplayByExtraction("aaaX bbbX cccX", "aaa bbb ccc", undo.changes), std::nullopt);
    const ChangeSet redo = doc.redo().value();
    EXPECT_EQ(redo.edits, (std::vector{raw(3, 3, 1), raw(7, 7, 1), raw(11, 11, 1)}));
    EXPECT_EQ(checkLspReplayByExtraction("aaa bbb ccc", "aaaX bbbX cccX", redo.changes), std::nullopt);
    EXPECT_TRUE(valid(doc));
}

TEST(DocumentUndo, Branches) {
    Document doc = Document::fromUtf8("base").value();
    (void)doc.replace(range(4, 4), "A", EditContext{}).value();
    (void)doc.undo().value();
    (void)doc.replace(range(4, 4), "B", EditContext{}).value();
    (void)doc.undo().value();
    EXPECT_EQ(doc.undoState().redoBranches, 2u);
    (void)doc.redo().value(); // redoChild: the newest child, B.
    EXPECT_EQ(text(doc), "baseB");
    (void)doc.undo().value();
    (void)doc.redo(0).value(); // Branch 0 is A.
    EXPECT_EQ(text(doc), "baseA");
    (void)doc.undo().value();
    (void)doc.redo().value(); // redoChild follows the last undone-from child: A.
    EXPECT_EQ(text(doc), "baseA");
    (void)doc.undo().value();
    EXPECT_EQ(doc.redo(2).error().code(), ErrorCode::OutOfRange);
    EXPECT_TRUE(valid(doc));
}

TEST(DocumentUndo, ErrorsAndOrder) {
    Fixture f("ab\xC3\xA9", {}, TextBufferOptions{.maxPieceBytes = 4096, .maxBufferBytes = 12});
    Document& doc = f.doc;
    const auto unchanged = [&](const char* what, auto&& op, ErrorCode expected) {
        const std::uint64_t version = doc.version();
        const auto state = doc.undoState();
        const std::string before = text(doc);
        const auto range = doc.compositionRange();
        const auto result = op();
        ASSERT_FALSE(result.has_value()) << what;
        EXPECT_EQ(result.error().code(), expected) << what << ": " << result.error().describe();
        EXPECT_EQ(doc.version(), version) << what;
        EXPECT_EQ(doc.undoState().stateId, state.stateId) << what;
        EXPECT_EQ(doc.undoState().transactions, state.transactions) << what;
        EXPECT_EQ(doc.compositionRange(), range) << what;
        EXPECT_EQ(text(doc), before) << what;
    };
    unchanged("undo at the root", [&] { return doc.undo(); }, ErrorCode::NotFound);
    unchanged("redo with no child", [&] { return doc.redo(); }, ErrorCode::NotFound);
    unchanged("redo branch 0 with no child", [&] { return doc.redo(0); }, ErrorCode::OutOfRange);
    unchanged("endGroup", [&] { return doc.endGroup({}); }, ErrorCode::InvalidArgument);
    unchanged("rollbackGroup", [&] { return doc.rollbackGroup(); }, ErrorCode::InvalidArgument);
    unchanged("update without composition", [&] { return doc.updateComposition("x"); }, ErrorCode::InvalidArgument);
    unchanged("cancel without composition", [&] { return doc.cancelComposition(); }, ErrorCode::InvalidArgument);
    unchanged("commit without composition", [&] { return doc.commitComposition("x", {}); }, ErrorCode::InvalidArgument);
    unchanged("begin beyond the text", [&] { return doc.beginComposition(ByteOffset{5}); }, ErrorCode::OutOfRange);
    unchanged(
        "begin inside a sequence", [&] { return doc.beginComposition(ByteOffset{3}); }, ErrorCode::InvalidArgument);
    // TextBuffer's checks, in order: begin > end before end > size.
    unchanged("edit", [&] { return doc.replace(range(9, 8), "\xFF", EditContext{}); }, ErrorCode::InvalidArgument);
    unchanged("edit beyond", [&] { return doc.replace(range(2, 9), "\xFF", EditContext{}); }, ErrorCode::OutOfRange);
    unchanged(
        "edit over the limit", [&] { return doc.replace(range(0, 0), "123456789", EditContext{}); },
        ErrorCode::OutOfRange);

    // In a group: Busy for history and composition, InvalidArgument for nothing else.
    ASSERT_TRUE(doc.beginGroup(EditContext{}).has_value());
    unchanged("undo in a group", [&] { return doc.undo(); }, ErrorCode::Busy);
    unchanged("redo in a group", [&] { return doc.redo(5); }, ErrorCode::Busy);
    unchanged("begin in a group", [&] { return doc.beginComposition(ByteOffset{9}); }, ErrorCode::Busy);
    ASSERT_TRUE(doc.endGroup({}).has_value());

    // While composing: Busy first, before any argument check.
    ASSERT_TRUE(doc.beginComposition(ByteOffset{2}).has_value());
    unchanged("edit while composing", [&] { return doc.replace(range(9, 1), "x", EditContext{}); }, ErrorCode::Busy);
    unchanged("undo while composing", [&] { return doc.undo(); }, ErrorCode::Busy);
    unchanged("redo while composing", [&] { return doc.redo(7); }, ErrorCode::Busy);
    unchanged("group while composing", [&] { return doc.beginGroup(EditContext{}); }, ErrorCode::Busy);
    unchanged("second begin", [&] { return doc.beginComposition(ByteOffset{99}); }, ErrorCode::Busy);
    unchanged("ill-formed preedit", [&] { return doc.updateComposition("\xC3"); }, ErrorCode::InvalidArgument);
    unchanged("preedit over the limit", [&] { return doc.updateComposition("123456789"); }, ErrorCode::OutOfRange);
    ASSERT_TRUE(doc.updateComposition("pq").has_value());
    // commit: begin > end; end > base size; not containing the start; inside a sequence; then text.
    unchanged(
        "commit begin > end", [&] { return doc.commitComposition("\xFF", {}, range(3, 2)); },
        ErrorCode::InvalidArgument);
    unchanged(
        "commit beyond the base", [&] { return doc.commitComposition("\xFF", {}, range(3, 5)); },
        ErrorCode::OutOfRange);
    unchanged(
        "commit not containing the start", [&] { return doc.commitComposition("x", {}, range(0, 1)); },
        ErrorCode::InvalidArgument);
    unchanged(
        "commit inside a sequence", [&] { return doc.commitComposition("x", {}, range(2, 3)); },
        ErrorCode::InvalidArgument);
    unchanged(
        "commit ill-formed", [&] { return doc.commitComposition("\xFF", {}, range(1, 2)); },
        ErrorCode::InvalidArgument);
    unchanged("commit over the limit", [&] { return doc.commitComposition("123456789", {}); }, ErrorCode::OutOfRange);
    EXPECT_TRUE(doc.undoState().composing); // The composition stays active.
    EXPECT_TRUE(valid(doc));
    ASSERT_TRUE(doc.commitComposition("Z", {}).has_value());
    EXPECT_EQ(text(doc), "abZ\xC3\xA9");
    EXPECT_TRUE(valid(doc));
}

TEST(DocumentUndo, ChangeSetTable) {
    Document doc = Document::fromUtf8("abc").value();
    // edit: the requested edits (no-ops dropped), context.selectionsAfter.
    const std::vector<Edit> edits{{range(0, 0), ""}, {range(1, 2), "XY"}};
    const ChangeSet edit = doc.edit(edits, EditContext{EditKind::Other, caret(1), caret(3)}).value();
    EXPECT_EQ(edit.origin, ChangeOrigin::Edit);
    EXPECT_EQ(edit.baseVersion, 0u);
    EXPECT_EQ(edit.version, 1u);
    EXPECT_EQ(edit.edits, (std::vector{raw(1, 2, 2)}));
    EXPECT_EQ(edit.changes.size(), 1u);
    EXPECT_EQ(edit.selections, caret(3));
    // An empty batch changes nothing but echoes the selections.
    const ChangeSet none = doc.edit({}, EditContext{EditKind::Typing, caret(3), caret(2)}).value();
    EXPECT_EQ(none.baseVersion, 1u);
    EXPECT_EQ(none.version, 1u);
    EXPECT_TRUE(none.edits.empty());
    EXPECT_EQ(none.selections, caret(2));
    // undo: [{r.after, |r.before|}], selectionsBefore.
    const ChangeSet undo = doc.undo().value();
    EXPECT_EQ(undo.origin, ChangeOrigin::Undo);
    EXPECT_EQ(undo.baseVersion, 1u);
    EXPECT_EQ(undo.version, 2u);
    EXPECT_EQ(undo.edits, (std::vector{raw(1, 3, 1)}));
    EXPECT_EQ(undo.selections, caret(1));
    // redo: [{r.before, |r.after|}], selectionsAfter.
    const ChangeSet redo = doc.redo().value();
    EXPECT_EQ(redo.origin, ChangeOrigin::Redo);
    EXPECT_EQ(redo.edits, (std::vector{raw(1, 2, 2)}));
    EXPECT_EQ(redo.selections, caret(3));
    // rollbackGroup with no node: nothing changes; with a node: the group's undo.
    ASSERT_TRUE(doc.beginGroup(EditContext{EditKind::Paste, caret(4), {}}).has_value());
    const ChangeSet emptyRollback = doc.rollbackGroup().value();
    EXPECT_EQ(emptyRollback.origin, ChangeOrigin::Rollback);
    EXPECT_EQ(emptyRollback.baseVersion, emptyRollback.version);
    EXPECT_TRUE(emptyRollback.edits.empty());
    EXPECT_EQ(emptyRollback.selections, caret(4));
    ASSERT_TRUE(doc.beginGroup(EditContext{EditKind::Paste, caret(4), {}}).has_value());
    (void)doc.replace(range(4, 4), "!!", EditContext{}).value();
    const std::uint64_t v = doc.version();
    const ChangeSet rollback = doc.rollbackGroup().value();
    EXPECT_EQ(rollback.origin, ChangeOrigin::Rollback);
    EXPECT_EQ(rollback.baseVersion, v);
    EXPECT_EQ(rollback.version, v + 1);
    EXPECT_EQ(rollback.edits, (std::vector{raw(4, 6, 0)}));
    EXPECT_EQ(rollback.selections, caret(4));
    // updateComposition: [{[start, start + len), |p|}], empty selections.
    ASSERT_TRUE(doc.beginComposition(ByteOffset{2}).has_value());
    const ChangeSet first = doc.updateComposition("kk").value();
    EXPECT_EQ(first.origin, ChangeOrigin::Composition);
    EXPECT_EQ(first.edits, (std::vector{raw(2, 2, 2)}));
    EXPECT_TRUE(first.selections.empty());
    const ChangeSet second = doc.updateComposition("k").value();
    EXPECT_EQ(second.edits, (std::vector{raw(2, 4, 1)}));
    const ChangeSet same = doc.updateComposition("k").value();
    EXPECT_EQ(same.edits, (std::vector{raw(2, 3, 1)})); // A replacement with equal bytes is a change.
    // commitComposition: [{[rb, re + len), |t|}], selectionsAfter; recorded against the base.
    const ChangeSet commit =
        doc.commitComposition("Q", EditContext{EditKind::Other, caret(2), caret(3)}, range(1, 2)).value();
    EXPECT_EQ(commit.origin, ChangeOrigin::Edit);
    EXPECT_EQ(commit.edits, (std::vector{raw(1, 3, 1)}));
    EXPECT_EQ(commit.selections, caret(3));
    EXPECT_EQ(text(doc), "aQYc");
    EXPECT_EQ(doc.undoTreeForTesting().currentNode().kind, EditKind::Typing);
    // cancelComposition: [{[start, start + len), 0}], empty selections.
    ASSERT_TRUE(doc.beginComposition(ByteOffset{1}).has_value());
    (void)doc.updateComposition("zz").value();
    const ChangeSet cancel = doc.cancelComposition().value();
    EXPECT_EQ(cancel.origin, ChangeOrigin::Composition);
    EXPECT_EQ(cancel.edits, (std::vector{raw(1, 3, 0)}));
    EXPECT_EQ(text(doc), "aQYc");
    EXPECT_TRUE(valid(doc));
}

TEST(DocumentUndo, HistoryCapPrunes) {
    {
        // Cap 1: each new transaction re-roots; the new root cannot be undone.
        Document doc = Document::fromUtf8("", {}, DocumentOptions{.maxTransactions = 1}).value();
        (void)doc.replace(range(0, 0), "A", EditContext{}).value();
        (void)doc.replace(range(1, 1), "B", EditContext{}).value();
        (void)doc.replace(range(2, 2), "C", EditContext{}).value();
        EXPECT_EQ(doc.undoState().transactions, 1u);
        ASSERT_TRUE(valid(doc));
        EXPECT_TRUE(doc.undo().has_value());
        EXPECT_EQ(text(doc), "AB");
        EXPECT_EQ(doc.undo().error().code(), ErrorCode::NotFound);
        EXPECT_TRUE(doc.redo().has_value());
        EXPECT_EQ(text(doc), "ABC");
    }
    {
        // Cap 3 with a branch: the oldest root subtree off the current path goes first.
        Document doc = Document::fromUtf8("", {}, DocumentOptions{.maxTransactions = 3}).value();
        (void)doc.replace(range(0, 0), "A", EditContext{}).value(); // Root child A.
        (void)doc.undo().value();
        (void)doc.replace(range(0, 0), "B", EditContext{}).value(); // Root child B (current path).
        (void)doc.replace(range(1, 1), "C", EditContext{}).value();
        EXPECT_EQ(doc.undoState().transactions, 3u);
        (void)doc.replace(range(2, 2), "D", EditContext{}).value(); // 4 > 3: A goes.
        EXPECT_EQ(doc.undoState().transactions, 3u);
        ASSERT_TRUE(valid(doc));
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(doc.undo().has_value());
        }
        EXPECT_EQ(text(doc), "");
        EXPECT_EQ(doc.undoState().redoBranches, 1u);
        ASSERT_TRUE(doc.redo().has_value()); // The root's redo child is the surviving B.
        EXPECT_EQ(text(doc), "B");
        // One more transaction from here: the root has one child, so the tree re-roots at B.
        (void)doc.replace(range(1, 1), "E", EditContext{}).value();
        EXPECT_EQ(doc.undoState().transactions, 3u);
        ASSERT_TRUE(valid(doc));
        ASSERT_TRUE(doc.undo().has_value());
        EXPECT_EQ(text(doc), "B");
        EXPECT_EQ(doc.undo().error().code(), ErrorCode::NotFound); // B is the root now.
        EXPECT_EQ(doc.undoState().redoBranches, 2u);
    }
}

// The history cap removes a dropped redo branch from the tree at once but frees its nodes a few per
// later operation (UndoTree::kReleasedPerOp), so the keystroke that drops it stays cheap.
TEST(DocumentUndo, HistoryCapDefersDroppedBranch) {
    using frost::text::detail::UndoTree;
    Fixture f("", DocumentOptions{.maxTransactions = 12});
    for (std::uint64_t i = 0; i < 10; ++i) {
        (void)f.doc.replace(range(i, i), "a", EditContext{}).value(); // Branch A: 10 transactions.
    }
    for (int i = 0; i < 10; ++i) {
        (void)f.doc.undo().value();
    }
    (void)f.doc.replace(range(0, 0), "b", EditContext{}).value(); // Branch B: 11 transactions,
    (void)f.doc.replace(range(1, 1), "b", EditContext{}).value(); // 12,
    EXPECT_TRUE(f.doc.undoTreeForTesting().dropped.empty());
    (void)f.doc.replace(range(2, 2), "b", EditContext{}).value(); // 13 > 12: A's 10 nodes go.
    EXPECT_EQ(f.doc.undoState().transactions, 3u);
    EXPECT_EQ(f.doc.undoTreeForTesting().dropped.size(), 10 - UndoTree::kReleasedPerOp);
    ASSERT_TRUE(valid(f.doc));
    (void)f.doc.replace(range(3, 3), "b", EditContext{}).value();
    EXPECT_EQ(f.doc.undoTreeForTesting().dropped.size(), 10 - 2 * UndoTree::kReleasedPerOp);
    (void)f.doc.undo().value(); // Undo and redo free dropped nodes too.
    EXPECT_TRUE(f.doc.undoTreeForTesting().dropped.empty());
    EXPECT_EQ(text(f.doc), "bbb");
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(f.doc.undo().has_value());
    }
    EXPECT_EQ(text(f.doc), "");
    EXPECT_EQ(f.doc.undoState().redoBranches, 1u); // Only B survives.
    EXPECT_EQ(f.doc.undo().error().code(), ErrorCode::NotFound);
    EXPECT_TRUE(valid(f.doc));
}

std::size_t pieceCount(const frost::text::detail::PieceTree& tree) {
    std::size_t count = 0;
    for (frost::text::detail::PieceTree::Cursor c = tree.first(); c.item() != nullptr; c.next()) {
        ++count;
    }
    return count;
}

/*
 * Coalesced delete runs and groups of deletes keep the removed text as the parent text's own
 * pieces (a review found one one-byte piece per keystroke, both in the history and, after undo, in
 * the live tree): a run removing k bytes keeps a slice of at most k / maxPieceBytes + 2 pieces, and
 * its undo leaves the document at most two pieces larger than before the run (the two cuts).
 */
TEST(DocumentUndo, DeleteRunsKeepWholePieces) {
    std::string initial;
    for (int i = 0; i < 20'000; ++i) {
        initial += static_cast<char>('a' + i % 26);
    }
    constexpr std::uint64_t kDeletes = 1000;
    for (const std::uint32_t maxPieceBytes : {4096u, 16u}) {
        for (const int mode : {0, 1, 2}) { // DeleteBackward, DeleteForward, a group of backspaces.
            const std::string label = std::format("maxPieceBytes {} mode {}", maxPieceBytes, mode);
            Fixture f(initial, {}, TextBufferOptions{.maxPieceBytes = maxPieceBytes});
            const std::size_t piecesBefore = pieceCount(f.doc.text().pieceTree());
            if (mode == 2) {
                ASSERT_TRUE(f.doc.beginGroup(EditContext{}).has_value());
            }
            std::uint64_t cursor = 12'345;
            for (std::uint64_t i = 0; i < kDeletes; ++i) {
                if (mode == 1) {
                    (void)f.doc
                        .replace(range(cursor, cursor + 1), "",
                                 EditContext{EditKind::DeleteForward, caret(cursor), caret(cursor)})
                        .value();
                } else {
                    (void)f.doc
                        .replace(range(cursor - 1, cursor), "",
                                 EditContext{EditKind::DeleteBackward, caret(cursor), caret(cursor - 1)})
                        .value();
                    --cursor;
                }
                if (i % 250 == 0) {
                    ASSERT_TRUE(valid(f.doc)) << label; // U5 compares the removed slice with X.
                }
            }
            if (mode == 2) {
                ASSERT_TRUE(f.doc.endGroup({}).has_value());
            }
            ASSERT_EQ(f.doc.undoState().transactions, 1u) << label;
            const auto& regions = f.doc.undoTreeForTesting().currentNode().regions;
            ASSERT_EQ(regions.size(), 1u) << label;
            EXPECT_EQ(regions[0].removed.summary().bytes, kDeletes) << label;
            EXPECT_LE(pieceCount(regions[0].removed), kDeletes / maxPieceBytes + 2) << label;
            const std::string afterRun = text(f.doc);
            (void)f.doc.undo().value();
            EXPECT_EQ(text(f.doc), initial) << label;
            EXPECT_LE(pieceCount(f.doc.text().pieceTree()), piecesBefore + 2) << label;
            (void)f.doc.redo().value();
            EXPECT_EQ(text(f.doc), afterRun) << label;
            (void)f.doc.undo().value();
            EXPECT_LE(pieceCount(f.doc.text().pieceTree()), piecesBefore + 2) << label;
            EXPECT_TRUE(valid(f.doc)) << label;
        }
    }
}

TEST(DocumentUndo, StateIdTracksSavedState) {
    Fixture f("text");
    const std::uint64_t saved = f.doc.undoState().stateId;
    (void)type(f.doc, 4, "a");
    const std::uint64_t typed = f.doc.undoState().stateId;
    EXPECT_NE(typed, saved);
    (void)type(f.doc, 5, "b"); // A coalesced extension changes it.
    const std::uint64_t extended = f.doc.undoState().stateId;
    EXPECT_NE(extended, typed);
    EXPECT_EQ(f.doc.undoState().transactions, 1u);
    (void)f.doc.undo().value();
    EXPECT_EQ(f.doc.undoState().stateId, saved); // Clean again.
    (void)f.doc.redo().value();
    EXPECT_EQ(f.doc.undoState().stateId, extended); // Dirty.
    ASSERT_TRUE(f.doc.beginComposition(ByteOffset{6}).has_value());
    (void)f.doc.updateComposition("pre").value();
    EXPECT_EQ(f.doc.undoState().stateId, extended); // Preedit leaves it.
    (void)f.doc.cancelComposition().value();
    EXPECT_EQ(f.doc.undoState().stateId, extended);
    (void)f.doc.edit({}, EditContext{}).value(); // A no-op leaves it.
    EXPECT_EQ(f.doc.undoState().stateId, extended);
}

TEST(DocumentUndo, AnchorsRestoredOnUndoOfDeletion) {
    Document doc = Document::fromUtf8("line1\nline2\nline3\n").value();
    const AnchorId bookmark = doc.createAnchor(ByteOffset{8}, Bias::Left).value();
    const AnchorId right = doc.createAnchor(ByteOffset{9}, Bias::Right).value();
    const AnchorId after = doc.createAnchor(ByteOffset{14}, Bias::Left).value();
    (void)doc.replace(range(6, 12), "", EditContext{EditKind::Other, {}, {}}).value(); // Delete "line2\n".
    EXPECT_EQ(at(doc, bookmark), 6u);
    EXPECT_EQ(at(doc, right), 6u);
    EXPECT_EQ(at(doc, after), 8u);
    (void)doc.undo().value();
    EXPECT_EQ(at(doc, bookmark), 8u); // Back at its exact offset.
    EXPECT_EQ(at(doc, right), 9u);
    EXPECT_EQ(at(doc, after), 14u);
    (void)doc.redo().value();
    EXPECT_EQ(at(doc, bookmark), 6u);
    (void)doc.undo().value();
    EXPECT_EQ(at(doc, bookmark), 8u);
    // An anchor created after the edit is mapped, not restored.
    (void)doc.redo().value();
    const AnchorId late = doc.createAnchor(ByteOffset{6}, Bias::Right).value();
    (void)doc.undo().value();
    EXPECT_EQ(at(doc, late), 12u);
    EXPECT_TRUE(valid(doc));
}

TEST(DocumentUndo, SelectionsRestored) {
    Document doc = Document::fromUtf8("one two").value();
    const SelectionSet before = SelectionSet::fromSelections(std::vector{Selection{ByteOffset{4}, ByteOffset{7}}}, 0);
    (void)doc.replace(range(4, 7), "2", EditContext{EditKind::Other, before, caret(5)}).value();
    EXPECT_EQ(doc.undo().value().selections, before);
    EXPECT_EQ(doc.redo().value().selections, caret(5));
}

// --- DocumentCoalesce -----------------------------------------------------------------------------

TEST(DocumentCoalesce, TypingWithinWindow) {
    Fixture f("");
    for (std::uint64_t i = 0; i < 5; ++i) {
        (void)type(f.doc, i, "x");
        f.clock.advance(500ms);
    }
    EXPECT_EQ(f.doc.undoState().transactions, 1u);
    (void)f.doc.undo().value();
    EXPECT_EQ(text(f.doc), "");
}

TEST(DocumentCoalesce, WindowExpiry) {
    Fixture f("");
    (void)type(f.doc, 0, "a");
    f.clock.advance(900ms);
    (void)type(f.doc, 1, "b");
    f.clock.advance(900ms); // 1800 ms after the first edit, 900 after the latest: the window slides.
    (void)type(f.doc, 2, "c");
    f.clock.advance(1000ms); // Inclusive.
    (void)type(f.doc, 3, "d");
    EXPECT_EQ(f.doc.undoState().transactions, 1u);
    f.clock.advance(1001ms);
    (void)type(f.doc, 4, "e");
    EXPECT_EQ(f.doc.undoState().transactions, 2u);
}

TEST(DocumentCoalesce, CaretMoveBreaks) {
    Fixture f("abc");
    (void)type(f.doc, 3, "x");
    // The next keystroke's selectionsBefore differs from the transaction's selectionsAfter.
    (void)f.doc.replace(range(0, 0), "y", EditContext{EditKind::Typing, caret(0), caret(1)}).value();
    EXPECT_EQ(f.doc.undoState().transactions, 2u);
    // Empty selection sets compare equal: callers without selections coalesce by kind and time.
    (void)f.doc.replace(range(0, 0), "1", EditContext{EditKind::Typing, {}, {}}).value();
    (void)f.doc.replace(range(1, 1), "2", EditContext{EditKind::Typing, {}, {}}).value();
    EXPECT_EQ(f.doc.undoState().transactions, 3u); // "1" broke (empty != caret(1)), "2" joined it.
}

TEST(DocumentCoalesce, KindChangeBreaks) {
    Fixture f("abc");
    (void)type(f.doc, 3, "x");
    (void)f.doc.replace(range(3, 4), "", EditContext{EditKind::DeleteBackward, caret(4), caret(3)}).value();
    EXPECT_EQ(f.doc.undoState().transactions, 2u);
    (void)f.doc.replace(range(3, 3), "p", EditContext{EditKind::Paste, caret(3), caret(4)}).value();
    (void)f.doc.replace(range(4, 4), "q", EditContext{EditKind::Paste, caret(4), caret(5)}).value();
    EXPECT_EQ(f.doc.undoState().transactions, 4u); // Paste never coalesces.
}

TEST(DocumentCoalesce, BackspaceAndDeleteRuns) {
    Fixture f("0123456789");
    for (std::uint64_t i = 0; i < 3; ++i) {
        const std::uint64_t c = 10 - i;
        (void)f.doc.replace(range(c - 1, c), "", EditContext{EditKind::DeleteBackward, caret(c), caret(c - 1)}).value();
    }
    EXPECT_EQ(f.doc.undoState().transactions, 1u);
    for (int i = 0; i < 3; ++i) {
        (void)f.doc.replace(range(2, 3), "", EditContext{EditKind::DeleteForward, caret(2), caret(2)}).value();
    }
    EXPECT_EQ(f.doc.undoState().transactions, 2u);
    EXPECT_EQ(text(f.doc), "0156");
    (void)f.doc.undo().value();
    EXPECT_EQ(text(f.doc), "0123456");
    (void)f.doc.undo().value();
    EXPECT_EQ(text(f.doc), "0123456789");
    EXPECT_TRUE(valid(f.doc));
}

TEST(DocumentCoalesce, BreakCoalescing) {
    Fixture f("");
    (void)type(f.doc, 0, "a");
    f.doc.breakCoalescing();
    (void)type(f.doc, 1, "b");
    EXPECT_EQ(f.doc.undoState().transactions, 2u);
}

TEST(DocumentCoalesce, UndoSeals) {
    Fixture f("");
    (void)type(f.doc, 0, "a");
    (void)type(f.doc, 1, "b");
    (void)f.doc.undo().value();
    (void)f.doc.redo().value();
    (void)type(f.doc, 2, "c"); // Undo and redo sealed the transaction.
    EXPECT_EQ(f.doc.undoState().transactions, 2u);
    (void)f.doc.undo().value();
    EXPECT_EQ(text(f.doc), "ab");
}

TEST(DocumentCoalesce, RejectedEditDoesNotSeal) {
    Fixture f("", {}, TextBufferOptions{.maxPieceBytes = 4096, .maxBufferBytes = 8});
    (void)type(f.doc, 0, "a");
    EXPECT_EQ(f.doc.replace(range(5, 5), "z", EditContext{EditKind::Typing, caret(1), caret(2)}).error().code(),
              ErrorCode::OutOfRange);
    // Over the buffer limit in the middle of the run: rejected, and the run continues.
    EXPECT_EQ(
        f.doc.replace(range(1, 1), "123456789", EditContext{EditKind::Typing, caret(1), caret(10)}).error().code(),
        ErrorCode::OutOfRange);
    (void)type(f.doc, 1, "b");
    EXPECT_EQ(f.doc.undoState().transactions, 1u);
    EXPECT_EQ(f.doc.undoTreeForTesting().currentNode().regions.size(), 1u);
    (void)f.doc.undo().value();
    EXPECT_EQ(text(f.doc), "");
}

TEST(DocumentCoalesce, IdenticalReplaceIsAChange) {
    Fixture f("abc");
    const std::uint64_t id = f.doc.undoState().stateId;
    const ChangeSet same = f.doc.replace(range(0, 1), "a", EditContext{}).value();
    EXPECT_EQ(same.version, same.baseVersion + 1);
    EXPECT_EQ(f.doc.undoState().transactions, 1u);
    EXPECT_NE(f.doc.undoState().stateId, id);
    EXPECT_EQ(f.doc.undoTreeForTesting().currentNode().regions.size(), 1u);
    const ChangeSet undo = f.doc.undo().value();
    EXPECT_EQ(undo.edits, (std::vector{raw(0, 1, 1)}));
    EXPECT_EQ(text(f.doc), "abc");
}

// --- DocumentGroup --------------------------------------------------------------------------------

TEST(DocumentGroup, NestedGroupsOneTransaction) {
    Fixture f("abc");
    const SelectionSet outerBefore = caret(1);
    ASSERT_TRUE(f.doc.beginGroup(EditContext{EditKind::Paste, outerBefore, {}}).has_value());
    (void)f.doc.replace(range(0, 0), "1", EditContext{EditKind::Typing, caret(0), caret(1)}).value();
    ASSERT_TRUE(f.doc.beginGroup(EditContext{EditKind::Other, caret(1), {}}).has_value()); // Ignored.
    (void)f.doc.replace(range(4, 4), "2", EditContext{}).value();
    ASSERT_TRUE(f.doc.endGroup(caret(2)).has_value()); // Ignored.
    EXPECT_TRUE(f.doc.undoState().groupOpen);
    (void)f.doc.replace(range(2, 2), "3", EditContext{}).value();
    ASSERT_TRUE(f.doc.endGroup(caret(6)).has_value());
    EXPECT_FALSE(f.doc.undoState().groupOpen);
    EXPECT_EQ(f.doc.undoState().transactions, 1u);
    EXPECT_EQ(f.doc.undoTreeForTesting().currentNode().kind, EditKind::Paste);
    EXPECT_EQ(text(f.doc), "1a3bc2");
    const ChangeSet undo = f.doc.undo().value();
    EXPECT_EQ(undo.selections, outerBefore);
    EXPECT_EQ(text(f.doc), "abc");
    EXPECT_EQ(f.doc.redo().value().selections, caret(6));
    // A Typing group never coalesces with typing after it.
    (void)type(f.doc, 6, "z");
    EXPECT_EQ(f.doc.undoState().transactions, 2u);
    EXPECT_TRUE(valid(f.doc));
}

TEST(DocumentGroup, FailedEditInsideGroup) {
    Fixture f("abc");
    ASSERT_TRUE(f.doc.beginGroup(EditContext{}).has_value());
    (void)f.doc.replace(range(3, 3), "d", EditContext{}).value();
    EXPECT_EQ(f.doc.replace(range(9, 9), "x", EditContext{}).error().code(), ErrorCode::OutOfRange);
    EXPECT_TRUE(f.doc.undoState().groupOpen);
    EXPECT_EQ(text(f.doc), "abcd");
    // Rollback after a failed step works.
    const ChangeSet rollback = f.doc.rollbackGroup().value();
    EXPECT_EQ(rollback.edits, (std::vector{raw(3, 4, 0)}));
    EXPECT_EQ(text(f.doc), "abc");
    EXPECT_EQ(f.doc.undoState().transactions, 0u);
    EXPECT_TRUE(valid(f.doc));
}

TEST(DocumentGroup, Rollback) {
    Fixture f("hello world");
    (void)f.doc.replace(range(0, 0), ">", EditContext{}).value();
    (void)f.doc.undo().value(); // A redo branch that the rollback must restore.
    const AnchorId bookmark = f.doc.createAnchor(ByteOffset{7}, Bias::Left).value();
    const auto before = f.doc.undoState();
    const std::uint64_t redoChild = f.doc.undoTreeForTesting().currentNode().redoChild;
    ASSERT_TRUE(f.doc.beginGroup(EditContext{EditKind::Other, caret(2), {}}).has_value());
    (void)f.doc.replace(range(5, 11), "", EditContext{}).value();
    (void)f.doc.replace(range(0, 5), "bye", EditContext{}).value();
    EXPECT_EQ(at(f.doc, bookmark), 0u);
    const ChangeSet rollback = f.doc.rollbackGroup().value();
    EXPECT_EQ(rollback.selections, caret(2));
    EXPECT_EQ(text(f.doc), "hello world");
    EXPECT_EQ(at(f.doc, bookmark), 7u);
    EXPECT_EQ(f.doc.undoState().transactions, before.transactions);
    EXPECT_EQ(f.doc.undoState().redoBranches, before.redoBranches); // No branch left behind.
    EXPECT_EQ(f.doc.undoState().stateId, before.stateId);
    EXPECT_EQ(f.doc.undoTreeForTesting().currentNode().redoChild, redoChild);
    EXPECT_FALSE(f.doc.undoState().groupOpen);
    EXPECT_TRUE(valid(f.doc));
    (void)f.doc.redo().value();
    EXPECT_EQ(text(f.doc), ">hello world");
}

// The parent's earlier redo child comes back even when it is not its last child (the review's
// mutation "rollback ignores the previous redoChild" passed DocumentGroup.Rollback, whose parent
// had one child).
TEST(DocumentGroup, RollbackRestoresEarlierRedoChild) {
    Fixture f("text");
    (void)f.doc.replace(range(0, 0), "A", EditContext{}).value();
    (void)f.doc.undo().value();
    (void)f.doc.replace(range(0, 0), "B", EditContext{}).value();
    (void)f.doc.undo().value();
    (void)f.doc.redo(0u).value(); // Back into A, then out: the root's redo child is A, not B.
    (void)f.doc.undo().value();
    const auto& root = f.doc.undoTreeForTesting().currentNode();
    ASSERT_EQ(root.children.size(), 2u);
    const std::uint64_t redoChild = root.redoChild;
    ASSERT_EQ(redoChild, root.children.front());
    ASSERT_TRUE(f.doc.beginGroup(EditContext{}).has_value());
    (void)f.doc.replace(range(4, 4), "!", EditContext{}).value();
    (void)f.doc.rollbackGroup().value();
    EXPECT_EQ(f.doc.undoTreeForTesting().currentNode().redoChild, redoChild);
    (void)f.doc.redo().value();
    EXPECT_EQ(text(f.doc), "Atext");
    EXPECT_TRUE(valid(f.doc));
}

TEST(DocumentGroup, RollbackAfterPrune) {
    Fixture f("", DocumentOptions{.maxTransactions = 1});
    (void)f.doc.replace(range(0, 0), "A", EditContext{}).value();
    (void)f.doc.undo().value();
    ASSERT_TRUE(f.doc.beginGroup(EditContext{}).has_value());
    (void)f.doc.replace(range(0, 0), "G", EditContext{}).value(); // The group node drops A.
    EXPECT_EQ(f.doc.undoState().transactions, 1u);
    (void)f.doc.rollbackGroup().value();
    EXPECT_FALSE(f.doc.undoState().canRedo); // A is gone; the redo child is not restored to it.
    EXPECT_EQ(f.doc.undoState().redoBranches, 0u);
    EXPECT_EQ(f.doc.redo().error().code(), ErrorCode::NotFound);
    EXPECT_TRUE(valid(f.doc));
}

TEST(DocumentGroup, EmptyGroupRecordsNothing) {
    Fixture f("abc");
    const auto before = f.doc.undoState();
    ASSERT_TRUE(f.doc.beginGroup(EditContext{}).has_value());
    (void)f.doc.edit({}, EditContext{}).value();
    (void)f.doc.replace(range(1, 1), "", EditContext{}).value();
    ASSERT_TRUE(f.doc.endGroup({}).has_value());
    EXPECT_EQ(f.doc.undoState().transactions, before.transactions);
    EXPECT_EQ(f.doc.undoState().stateId, before.stateId);
    EXPECT_EQ(f.doc.version(), 0u);
}

TEST(DocumentGroup, BusyInsideGroup) {
    Fixture f("abc");
    ASSERT_TRUE(f.doc.beginGroup(EditContext{}).has_value());
    EXPECT_EQ(f.doc.undo().error().code(), ErrorCode::Busy);
    EXPECT_EQ(f.doc.redo().error().code(), ErrorCode::Busy);
    EXPECT_EQ(f.doc.beginComposition(ByteOffset{0}).error().code(), ErrorCode::Busy);
    EXPECT_TRUE(f.doc.createAnchor(ByteOffset{1}, Bias::Left).has_value()); // Anchors are allowed.
    ASSERT_TRUE(f.doc.beginGroup(EditContext{}).has_value());               // So is nesting.
    EXPECT_EQ(f.doc.undoState().groupOpen, true);
}

// --- DocumentIme ----------------------------------------------------------------------------------

TEST(DocumentIme, PreeditNotUndoable) {
    Fixture f("ab");
    ASSERT_TRUE(f.doc.beginComposition(ByteOffset{1}).has_value());
    for (const char* preedit : {"k", "ka", "kan"}) {
        const ChangeSet update = f.doc.updateComposition(preedit).value();
        EXPECT_EQ(update.origin, ChangeOrigin::Composition);
    }
    EXPECT_EQ(text(f.doc), "akanb");
    EXPECT_EQ(f.doc.undoState().transactions, 0u);
    (void)f.doc.commitComposition("\xE6\xBC\xA2", EditContext{EditKind::Typing, caret(1), caret(4)}).value(); // 漢
    EXPECT_EQ(text(f.doc), "a\xE6\xBC\xA2"
                           "b");
    EXPECT_EQ(f.doc.undoState().transactions, 1u);
    const ChangeSet undo = f.doc.undo().value();
    EXPECT_EQ(undo.edits, (std::vector{raw(1, 4, 0)}));
    EXPECT_EQ(text(f.doc), "ab"); // No preedit state is reachable.
    EXPECT_EQ(f.doc.undo().error().code(), ErrorCode::NotFound);
    (void)f.doc.redo().value();
    EXPECT_EQ(text(f.doc), "a\xE6\xBC\xA2"
                           "b");
    EXPECT_TRUE(valid(f.doc));
}

TEST(DocumentIme, CommitCoalescesWithTyping) {
    Fixture f("");
    (void)type(f.doc, 0, "a");
    ASSERT_TRUE(f.doc.beginComposition(ByteOffset{1}).has_value());
    (void)f.doc.updateComposition("b").value();
    f.clock.advance(100ms);
    (void)f.doc.commitComposition("B", EditContext{EditKind::Other, caret(1), caret(2)}).value();
    EXPECT_EQ(f.doc.undoState().transactions, 1u);
    (void)type(f.doc, 2, "c");
    EXPECT_EQ(f.doc.undoState().transactions, 1u);
    (void)f.doc.undo().value();
    EXPECT_EQ(text(f.doc), "");
}

TEST(DocumentIme, CommitReplacesSurrounding) {
    Fixture f("abc");
    ASSERT_TRUE(f.doc.beginComposition(ByteOffset{3}).has_value());
    (void)f.doc.updateComposition("d").value();
    const ChangeSet commit = f.doc.commitComposition("X", EditContext{}, range(1, 3)).value();
    EXPECT_EQ(commit.edits, (std::vector{raw(1, 4, 1)}));
    EXPECT_EQ(text(f.doc), "aX");
    EXPECT_EQ(f.doc.undoState().transactions, 1u);
    const ChangeSet undo = f.doc.undo().value();
    EXPECT_EQ(undo.edits, (std::vector{raw(1, 2, 2)})); // One step against the base.
    EXPECT_EQ(text(f.doc), "abc");
    EXPECT_TRUE(valid(f.doc));
}

TEST(DocumentIme, EmptyCommitIsCancel) {
    Fixture f("abc");
    ASSERT_TRUE(f.doc.beginComposition(ByteOffset{1}).has_value());
    (void)f.doc.updateComposition("zz").value();
    const ChangeSet commit = f.doc.commitComposition("", EditContext{}).value();
    EXPECT_EQ(commit.origin, ChangeOrigin::Composition);
    EXPECT_EQ(commit.edits, (std::vector{raw(1, 3, 0)}));
    EXPECT_FALSE(f.doc.undoState().composing);
    EXPECT_EQ(f.doc.undoState().transactions, 0u);
    EXPECT_EQ(text(f.doc), "abc");
}

TEST(DocumentIme, CancelRestoresText) {
    Fixture f("abc");
    const std::uint64_t v = f.doc.version();
    ASSERT_TRUE(f.doc.beginComposition(ByteOffset{2}).has_value());
    (void)f.doc.updateComposition("xyz").value();
    (void)f.doc.cancelComposition().value();
    EXPECT_EQ(text(f.doc), "abc");
    EXPECT_EQ(f.doc.version(), v + 2);
    EXPECT_FALSE(f.doc.compositionRange().has_value());
    EXPECT_EQ(f.doc.undoState().transactions, 0u);
}

TEST(DocumentIme, CommittedTextExcludesPreedit) {
    Fixture f("abc");
    ASSERT_TRUE(f.doc.beginComposition(ByteOffset{1}).has_value());
    (void)f.doc.updateComposition("PRE").value();
    EXPECT_EQ(text(f.doc), "aPREbc");
    EXPECT_EQ(f.doc.committedText().text(), "abc");
    EXPECT_EQ(f.doc.compositionRange(), range(1, 4));
    (void)f.doc.commitComposition("c", EditContext{}).value();
    EXPECT_EQ(f.doc.committedText().text(), "acbc");
    EXPECT_EQ(f.doc.committedText().version(), f.doc.version());
}

TEST(DocumentIme, BusyWhileComposing) {
    Fixture f("abc");
    ASSERT_TRUE(f.doc.beginComposition(ByteOffset{1}).has_value());
    EXPECT_EQ(f.doc.replace(range(0, 0), "x", EditContext{}).error().code(), ErrorCode::Busy);
    EXPECT_EQ(f.doc.edit({}, EditContext{}).error().code(), ErrorCode::Busy);
    EXPECT_EQ(f.doc.undo().error().code(), ErrorCode::Busy);
    EXPECT_EQ(f.doc.redo().error().code(), ErrorCode::Busy);
    EXPECT_EQ(f.doc.beginGroup(EditContext{}).error().code(), ErrorCode::Busy);
    EXPECT_EQ(f.doc.beginComposition(ByteOffset{0}).error().code(), ErrorCode::Busy);
    EXPECT_TRUE(f.doc.createAnchor(ByteOffset{2}, Bias::Right).has_value());
}

TEST(DocumentIme, AnchorsAroundPreedit) {
    Fixture f("abcd");
    const AnchorId left = f.doc.createAnchor(ByteOffset{2}, Bias::Left).value();
    const AnchorId right = f.doc.createAnchor(ByteOffset{2}, Bias::Right).value();
    const AnchorId later = f.doc.createAnchor(ByteOffset{3}, Bias::Left).value();
    ASSERT_TRUE(f.doc.beginComposition(ByteOffset{2}).has_value());
    (void)f.doc.updateComposition("xy").value();
    EXPECT_EQ(at(f.doc, left), 2u);
    EXPECT_EQ(at(f.doc, right), 4u);
    EXPECT_EQ(at(f.doc, later), 5u);
    (void)f.doc.cancelComposition().value(); // Unaffected by a cancelled preedit.
    EXPECT_EQ(at(f.doc, left), 2u);
    EXPECT_EQ(at(f.doc, right), 2u);
    EXPECT_EQ(at(f.doc, later), 3u);
    ASSERT_TRUE(f.doc.beginComposition(ByteOffset{2}).has_value());
    (void)f.doc.updateComposition("xyz").value();
    (void)f.doc.commitComposition("Q", EditContext{}, range(1, 3)).value(); // Net: [1, 3) -> "Q".
    EXPECT_EQ(text(f.doc), "aQd");
    EXPECT_EQ(at(f.doc, left), 1u);
    EXPECT_EQ(at(f.doc, right), 2u);
    EXPECT_EQ(at(f.doc, later), 1u);
    (void)f.doc.undo().value();
    EXPECT_EQ(at(f.doc, left), 2u);
    EXPECT_EQ(at(f.doc, right), 2u);
    EXPECT_EQ(at(f.doc, later), 3u);
    EXPECT_TRUE(valid(f.doc));
}

TEST(DocumentIme, InvariantsWhileComposing) {
    Fixture f("abc");
    (void)type(f.doc, 3, "d"); // An open transaction.
    ASSERT_TRUE(f.doc.beginComposition(ByteOffset{4}).has_value());
    (void)f.doc.updateComposition("pre").value();
    EXPECT_TRUE(valid(f.doc)); // U5 against the base, U6 for the preedit.
    EXPECT_TRUE(f.doc.undoTreeForTesting().currentNode().open);
    (void)f.doc.updateComposition("").value();
    EXPECT_TRUE(valid(f.doc));
    (void)f.doc.commitComposition("e", EditContext{EditKind::Typing, caret(4), caret(5)}).value();
    EXPECT_TRUE(valid(f.doc));
    EXPECT_EQ(f.doc.undoState().transactions, 1u);
}

// --- DocumentSnapshot -----------------------------------------------------------------------------

TEST(DocumentSnapshot, ResolvesAnchorsInLaterSnapshots) {
    Document doc = Document::fromUtf8("0123456789").value();
    const AnchorId id = doc.createAnchor(ByteOffset{5}, Bias::Right).value();
    const auto first = doc.snapshot();
    (void)doc.replace(range(0, 0), "ab", EditContext{}).value();
    const auto second = doc.snapshot();
    (void)doc.replace(range(3, 6), "", EditContext{}).value();
    const auto third = doc.snapshot();
    EXPECT_EQ(first.anchors.resolve(id).value(), ByteOffset{5});
    EXPECT_EQ(second.anchors.resolve(id).value(), ByteOffset{7});
    EXPECT_EQ(third.anchors.resolve(id).value(), ByteOffset{4});
    EXPECT_EQ(first.anchors.version(), first.text.version());
    EXPECT_EQ(third.anchors.version(), third.text.version());
    EXPECT_EQ(third.anchors.textSize(), third.text.size());
    EXPECT_EQ(first.text.text(), "0123456789"); // Earlier snapshots are untouched.
}

} // namespace

TEST(DocumentExternalTransaction, LocalUndoAndRedoCannotSplitGlobalBoundary) {
    auto value = Document::fromUtf8("before");
    ASSERT_TRUE(value);
    std::array<std::byte, 16> identity{};
    identity[0] = std::byte{1};
    const frost::TransactionId id(identity);
    ASSERT_TRUE(value->acquireExternalWrite(id));
    ASSERT_FALSE(value->replace(range(0, 6), "wrong writer", EditContext{}));
    ASSERT_TRUE(value->replaceExternal(id, range(0, 6), "after", EditContext{}));
    ASSERT_TRUE(value->markExternalTransaction(id));
    value->releaseExternalWrite(id);
    EXPECT_EQ(value->undoState().externalUndo, id);
    EXPECT_FALSE(value->undo());
    EXPECT_EQ(text(*value), "after");
    ASSERT_TRUE(value->acquireExternalWrite(id));
    ASSERT_TRUE(value->undoExternal(id));
    value->releaseExternalWrite(id);
    EXPECT_EQ(text(*value), "before");
    EXPECT_FALSE(value->redo());
    ASSERT_TRUE(value->acquireExternalWrite(id));
    ASSERT_TRUE(value->redoExternal(id));
    value->releaseExternalWrite(id);
    EXPECT_EQ(text(*value), "after");
}

TEST(DocumentExternalTransaction, CompositionAndAnotherCoordinatorRefuseWithoutChanges) {
    auto value = Document::fromUtf8("a");
    ASSERT_TRUE(value);
    std::array<std::byte, 16> bytes{};
    bytes[0] = std::byte{7};
    const frost::TransactionId id(bytes);
    ASSERT_TRUE(value->beginComposition(ByteOffset{0}));
    EXPECT_FALSE(value->acquireExternalWrite(id));
    ASSERT_TRUE(value->cancelComposition());
    ASSERT_TRUE(value->acquireExternalWrite(id));
    EXPECT_FALSE(value->acquireExternalWrite(id));
    EXPECT_FALSE(value->beginComposition(ByteOffset{0}));
    value->releaseExternalWrite(id);
    EXPECT_EQ(text(*value), "a");
}
