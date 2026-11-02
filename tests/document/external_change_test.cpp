// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the SaveManager's external-change handling on MemFs (ADR 0028): a clean buffer
// reloads as one undoable edit, our own save's echo and a touch change nothing, a dirty buffer gets
// a three-way merge model with every decision, deleted files, a reload that would cross the
// large-file threshold, a change the watcher has not reported yet, and the watcher path itself
// (applyChanges and follow() over a MemFsWatcher in simulated time).

#include "frost/io/mem_fs_watcher.hpp"
#include "frost/sched/sim_executor.hpp"

#include "document_fixture.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <stop_token>
#include <string>
#include <vector>

namespace frost::document::test {
namespace {

using namespace std::chrono_literals;
using ExternalChangeTest = DocumentFixture;

TEST_F(ExternalChangeTest, CleanBufferReloadsAsOneUndoStep) {
    writeExternally("a.txt", "one\ntwo\nthree\n");
    FileDocument* document = open("a.txt");
    ASSERT_NE(document, nullptr);
    const std::uint32_t before = document->document().undoState().transactions;

    writeExternally("a.txt", "one\nTWO\nthree\nfour\n");
    const ExternalChange change = manager().checkDisk(*document);
    EXPECT_EQ(change.kind, ExternalChangeKind::Reloaded);
    EXPECT_EQ(textOf(*document), "one\nTWO\nthree\nfour\n");
    EXPECT_FALSE(document->dirty());
    EXPECT_EQ(document->diskState(), DiskState::InSync);
    EXPECT_EQ(document->document().undoState().transactions, before + 1);

    // Undo takes the whole reload back in one step; the buffer then differs from the disk.
    ASSERT_TRUE(document->document().undo());
    EXPECT_EQ(textOf(*document), "one\ntwo\nthree\n");
    EXPECT_TRUE(document->dirty());
    ASSERT_TRUE(document->document().redo());
    EXPECT_FALSE(document->dirty());

    // Nothing changed on disk since: no further reaction.
    EXPECT_EQ(manager().checkDisk(*document).kind, ExternalChangeKind::None);
}

TEST_F(ExternalChangeTest, OwnSaveEchoAndTouchChangeNothing) {
    writeExternally("a.txt", "x\n");
    FileDocument* document = open("a.txt");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 1, "y");
    ASSERT_TRUE(manager().save(*document));
    EXPECT_EQ(manager().checkDisk(*document).kind, ExternalChangeKind::None);

    // The same bytes written again (a touch, a checkout of the same content): only the time moves.
    writeExternally("a.txt", "y\n");
    EXPECT_EQ(manager().checkDisk(*document).kind, ExternalChangeKind::Touched);
    EXPECT_EQ(manager().checkDisk(*document).kind, ExternalChangeKind::None);
    EXPECT_FALSE(document->dirty());
}

TEST_F(ExternalChangeTest, DirtyBufferGetsAMergeModel) {
    writeExternally("m.txt", "a\nb\nc\n");
    FileDocument* document = open("m.txt");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 1, "A"); // Ours: the first line.
    writeExternally("m.txt", "a\nb\nC\n");

    const ExternalChange change = manager().checkDisk(*document);
    ASSERT_EQ(change.kind, ExternalChangeKind::MergePending);
    EXPECT_EQ(document->diskState(), DiskState::Changed);
    const PendingMerge* merge = document->pendingMerge();
    ASSERT_NE(merge, nullptr);
    EXPECT_EQ(merge->model.unresolvedCount(), 0U);
    EXPECT_EQ(merge->model.base().bytes(), "a\nb\nc\n");
    EXPECT_EQ(merge->model.ours().bytes(), "A\nb\nc\n");
    EXPECT_EQ(merge->model.theirs().bytes(), "a\nb\nC\n");
    EXPECT_EQ(textOf(*document), "A\nb\nc\n"); // The buffer is untouched until the decision.

    // While the merge waits, saving refuses (an explicit overwrite is the escape hatch) and Save All skips.
    auto refused = manager().save(*document);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::Busy);
    const SaveAllResult all = manager().saveAll();
    ASSERT_EQ(all.skipped.size(), 1U);
    EXPECT_EQ(all.skipped[0].reason, SkipReason::MergePending);
    EXPECT_EQ(read("m.txt"), "a\nb\nC\n");

    {
        auto applied = manager().resolveExternalChange(*document, MergeDecision::ApplyMerge);
        ASSERT_TRUE(applied) << applied.error().describe();
    }
    EXPECT_EQ(textOf(*document), "A\nb\nC\n");
    EXPECT_EQ(document->diskState(), DiskState::InSync);
    EXPECT_EQ(document->pendingMerge(), nullptr);
    EXPECT_TRUE(document->dirty()); // The merge result is not on disk yet.
    EXPECT_EQ(document->baseText().text(), "a\nb\nC\n");
    ASSERT_TRUE(manager().save(*document));
    EXPECT_EQ(read("m.txt"), "A\nb\nC\n");

    // The merge was one undoable step.
    ASSERT_TRUE(document->document().undo());
    EXPECT_EQ(textOf(*document), "A\nb\nc\n");
}

TEST_F(ExternalChangeTest, ConflictsMustBeResolvedBeforeApplying) {
    writeExternally("c.txt", "a\nb\nc\n");
    FileDocument* document = open("c.txt");
    ASSERT_NE(document, nullptr);
    edit(*document, 2, 3, "ours");
    writeExternally("c.txt", "a\ntheirs\nc\n");
    ASSERT_EQ(manager().checkDisk(*document).kind, ExternalChangeKind::MergePending);
    PendingMerge* merge = document->pendingMerge();
    ASSERT_NE(merge, nullptr);
    ASSERT_EQ(merge->model.unresolvedCount(), 1U);

    auto refused = manager().resolveExternalChange(*document, MergeDecision::ApplyMerge);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(textOf(*document), "a\nours\nc\n");

    std::size_t conflict = 0;
    while (merge->model.merge().chunks[conflict].kind != diff::ChunkKind::Conflict) {
        ++conflict;
    }
    ASSERT_TRUE(merge->model.resolve(conflict, diff::Take::Theirs));
    {
        auto applied = manager().resolveExternalChange(*document, MergeDecision::ApplyMerge);
        ASSERT_TRUE(applied) << applied.error().describe();
    }
    EXPECT_EQ(textOf(*document), "a\ntheirs\nc\n");
    EXPECT_FALSE(document->dirty()); // Taking theirs everywhere leaves the disk's text.
}

TEST_F(ExternalChangeTest, DiskRevertedToTheBaseDropsTheMerge) {
    writeExternally("v.txt", "base\n");
    FileDocument* document = open("v.txt");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 4, "mine");
    writeExternally("v.txt", "theirs\n");
    ASSERT_EQ(manager().checkDisk(*document).kind, ExternalChangeKind::MergePending);
    writeExternally("v.txt", "base\n"); // A checkout back, before the user decided.
    EXPECT_EQ(manager().checkDisk(*document).kind, ExternalChangeKind::Touched);
    EXPECT_EQ(document->diskState(), DiskState::InSync);
    EXPECT_EQ(document->pendingMerge(), nullptr);
    ASSERT_TRUE(manager().save(*document));
    EXPECT_EQ(read("v.txt"), "mine\n");
}

TEST_F(ExternalChangeTest, KeepBufferAndTakeDisk) {
    writeExternally("k.txt", "base\n");
    FileDocument* document = open("k.txt");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 4, "mine");
    writeExternally("k.txt", "theirs\n");
    ASSERT_EQ(manager().checkDisk(*document).kind, ExternalChangeKind::MergePending);
    ASSERT_TRUE(manager().resolveExternalChange(*document, MergeDecision::KeepBuffer));
    EXPECT_EQ(textOf(*document), "mine\n");
    EXPECT_TRUE(document->dirty());
    ASSERT_TRUE(document->document().undo());
    EXPECT_TRUE(document->dirty()); // "base" is not on disk any more either.
    ASSERT_TRUE(document->document().redo());
    ASSERT_TRUE(manager().save(*document)); // The disk's text is the base now: no conflict.
    EXPECT_EQ(read("k.txt"), "mine\n");

    edit(*document, 0, 4, "more");
    writeExternally("k.txt", "disk wins\n");
    ASSERT_EQ(manager().checkDisk(*document).kind, ExternalChangeKind::MergePending);
    ASSERT_TRUE(manager().resolveExternalChange(*document, MergeDecision::TakeDisk));
    EXPECT_EQ(textOf(*document), "disk wins\n");
    EXPECT_FALSE(document->dirty());
    ASSERT_TRUE(document->document().undo());
    EXPECT_EQ(textOf(*document), "more\n");

    auto none = manager().resolveExternalChange(*document, MergeDecision::TakeDisk);
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().code(), ErrorCode::NotFound);
}

TEST_F(ExternalChangeTest, EditsAfterTheMergeWasBuiltRebuildIt) {
    // Changes on non-adjacent lines merge cleanly (adjacent ones conflict, as in git).
    writeExternally("r.txt", "a\nb\nc\nd\ne\n");
    FileDocument* document = open("r.txt");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 1, "A");
    writeExternally("r.txt", "a\nb\nc\nd\nE\n");
    ASSERT_EQ(manager().checkDisk(*document).kind, ExternalChangeKind::MergePending);
    edit(*document, 4, 5, "C");
    auto rebuilt = manager().resolveExternalChange(*document, MergeDecision::ApplyMerge);
    ASSERT_FALSE(rebuilt);
    EXPECT_EQ(rebuilt.error().code(), ErrorCode::Busy);
    ASSERT_NE(document->pendingMerge(), nullptr);
    EXPECT_EQ(document->pendingMerge()->model.ours().bytes(), "A\nb\nC\nd\ne\n");
    {
        auto applied = manager().resolveExternalChange(*document, MergeDecision::ApplyMerge);
        ASSERT_TRUE(applied) << applied.error().describe();
    }
    EXPECT_EQ(textOf(*document), "A\nb\nC\nd\nE\n");
}

TEST_F(ExternalChangeTest, DeletedFiles) {
    writeExternally("d.txt", "keep me\n");
    FileDocument* document = open("d.txt");
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(fs.removeFile(path("d.txt")));
    EXPECT_EQ(manager().checkDisk(*document).kind, ExternalChangeKind::Deleted);
    EXPECT_EQ(document->diskState(), DiskState::Deleted);
    EXPECT_EQ(manager().checkDisk(*document).kind, ExternalChangeKind::None); // Reported once.
    EXPECT_EQ(textOf(*document), "keep me\n");

    // An autosave never recreates it; an explicit save does.
    edit(*document, 0, 4, "KEEP");
    const SaveAllResult autosaved = manager().saveAll(SaveReason::Autosave);
    ASSERT_EQ(autosaved.skipped.size(), 1U);
    EXPECT_EQ(autosaved.skipped[0].reason, SkipReason::Deleted);
    EXPECT_FALSE(fs.exists(path("d.txt")).value());
    auto saved = manager().save(*document);
    ASSERT_TRUE(saved) << saved.error().describe();
    EXPECT_EQ(saved->method, io::SafeWriteMethod::Created);
    EXPECT_EQ(read("d.txt"), "KEEP me\n");
    EXPECT_EQ(document->diskState(), DiskState::InSync);
}

TEST_F(ExternalChangeTest, ReloadCrossingTheLargeFileThresholdKeepsTheBuffer) {
    options.load.maxFileBytes = 64;
    writeExternally("g.txt", "small\n");
    FileDocument* document = open("g.txt");
    ASSERT_NE(document, nullptr);
    writeExternally("g.txt", std::string(100, 'x'));
    const ExternalChange change = manager().checkDisk(*document);
    ASSERT_EQ(change.kind, ExternalChangeKind::OutOfDate);
    ASSERT_TRUE(change.error.has_value());
    EXPECT_EQ(change.error->code(), ErrorCode::OutOfRange);
    EXPECT_EQ(document->diskState(), DiskState::OutOfDate);
    EXPECT_EQ(textOf(*document), "small\n");
    ASSERT_TRUE(document->outOfDateReason().has_value());

    // Saving needs the explicit overwrite; Save All passes it over.
    edit(*document, 0, 5, "SMALL");
    auto refused = manager().save(*document);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::Busy);
    EXPECT_EQ(manager().saveAll().skipped.at(0).reason, SkipReason::OutOfDate);
    SaveOptions overwrite;
    overwrite.overwriteExternalChanges = true;
    ASSERT_TRUE(manager().save(*document, overwrite));
    EXPECT_EQ(read("g.txt"), "SMALL\n");
    EXPECT_EQ(document->diskState(), DiskState::InSync);
}

TEST_F(ExternalChangeTest, SaveDetectsAnUnreportedChange) {
    writeExternally("u.txt", "a\nb\nc\n");
    FileDocument* document = open("u.txt");
    ASSERT_NE(document, nullptr);
    edit(*document, 0, 1, "A");
    writeExternally("u.txt", "a\nb\nC\n"); // The watcher has not delivered this yet.
    std::vector<ExternalChangeKind> heard;
    manager().setExternalChangeListener([&](const ExternalChange& change) { heard.push_back(change.kind); });
    auto refused = manager().save(*document);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::Busy);
    EXPECT_EQ(read("u.txt"), "a\nb\nC\n");
    EXPECT_EQ(heard, std::vector<ExternalChangeKind>{ExternalChangeKind::MergePending});
    {
        auto applied = manager().resolveExternalChange(*document, MergeDecision::ApplyMerge);
        ASSERT_TRUE(applied) << applied.error().describe();
    }
    ASSERT_TRUE(manager().save(*document));
    EXPECT_EQ(read("u.txt"), "A\nb\nC\n");
}

TEST_F(ExternalChangeTest, WatcherBatchesReachTheOpenDocuments) {
    writeExternally("w.txt", "w\n");
    writeExternally("other.txt", "o\n");
    FileDocument* document = open("w.txt");
    ASSERT_NE(document, nullptr);

    // A batch for another file changes nothing; one for the file (or a rescan of its directory) reloads it.
    writeExternally("w.txt", "W\n");
    io::FileChangeBatch unrelated;
    unrelated.changes.push_back(
        io::FileChange{io::FileChangeKind::Modified, path("other.txt"), {}, io::FileType::Regular});
    EXPECT_TRUE(manager().applyChanges(unrelated).empty());
    EXPECT_EQ(textOf(*document), "w\n");
    io::FileChangeBatch rescan;
    rescan.changes.push_back(io::FileChange{io::FileChangeKind::Rescan, workDirectory(), {}, io::FileType::Directory});
    const auto found = manager().applyChanges(rescan);
    ASSERT_EQ(found.size(), 1U);
    EXPECT_EQ(found[0].kind, ExternalChangeKind::Reloaded);
    EXPECT_EQ(textOf(*document), "W\n");

    // follow() pumps a real watch (MemFsWatcher in simulated time).
    sched::SimExecutor sim;
    io::MemFsWatcher watcher(fs, sim);
    auto watchHandle = sim.spawn(sched::Lane::UserVisible, watcher.watch(workDirectory(), io::WatchOptions{}));
    auto watch = watchHandle.join();
    ASSERT_TRUE(watch) << watch.error().describe();
    std::vector<ExternalChangeKind> heard;
    manager().setExternalChangeListener([&](const ExternalChange& change) { heard.push_back(change.kind); });
    std::stop_source stop;
    auto follow = sim.spawn(sched::Lane::UserVisible, manager().follow(**watch, stop.get_token()));
    writeExternally("w.txt", "followed\n");
    sim.runFor(1s);
    EXPECT_EQ(textOf(*document), "followed\n");
    EXPECT_EQ(heard, std::vector<ExternalChangeKind>{ExternalChangeKind::Reloaded});

    stop.request_stop();
    sim.runUntilIdle();
    ASSERT_TRUE(follow.ready());
    const auto ended = follow.join();
    ASSERT_FALSE(ended);
    EXPECT_EQ(ended.error().code(), ErrorCode::Cancelled);
    watch->reset();
    sim.runUntilIdle();
}

} // namespace
} // namespace frost::document::test
