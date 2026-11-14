// SPDX-License-Identifier: Apache-2.0
// Authored native multi-file Apply/global Undo, dirty buffers, moves, conflicts and final admission.
#include "frost/document/refactoring_transaction.hpp"
#include "frost/io/file_transaction_replace.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"

#include "file_transaction_replace_testing.hpp"
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
#include "support/directory_link.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
namespace frost::document::test {
namespace {
Result<void> overwriteInterference(std::string_view path, std::string_view bytes) {
#ifdef _WIN32
    auto wide = io::toWin32Path(path);
    if (!wide)
        return std::unexpected(wide.error());
    HANDLE file = CreateFileW(wide->c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return std::unexpected(io::lastWin32Error("test: full-share overwrite", path));
    DWORD count = 0;
    if (!SetEndOfFile(file) || !WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) ||
        count != bytes.size() || !FlushFileBuffers(file)) {
        auto error = io::lastWin32Error("test: actual overwrite", path);
        CloseHandle(file);
        return std::unexpected(error);
    }
    CloseHandle(file);
#else
    const int file = ::open(std::string(path).c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (file < 0)
        return std::unexpected(io::errnoError(errno, "test: actual overwrite", path));
    const auto count = ::write(file, bytes.data(), bytes.size());
    const auto flushed = ::fsync(file);
    const auto error = errno;
    ::close(file);
    if (count < 0 || static_cast<std::size_t>(count) != bytes.size() || flushed != 0)
        return std::unexpected(io::errnoError(error, "test: actual overwrite", path));
#endif
    return {};
}
class RefactoringTransactionTest : public ::testing::Test {
protected:
    frost::test::ScratchDir scratch{"refactoring"};
    io::IFileSystem& fs = io::realFileSystem();
    std::string a = scratch.join("a.cpp"), b = scratch.join("b.cpp"), project = scratch.join("p.txt");
    std::optional<storage::HotExitJournal> journal;
    std::unique_ptr<SaveManager> documents;
    std::unique_ptr<RefactoringCoordinator> coordinator;
    std::size_t changed = 0, renamed = 0;
    RefactoringOptions options;
    void SetUp() override {
        ASSERT_TRUE(fs.writeFileAtomic(a, "before a\r\n"));
        ASSERT_TRUE(fs.writeFileAtomic(b, "before b\n"));
        ASSERT_TRUE(fs.writeFileAtomic(project, "a.cpp\nb.cpp\n"));
        auto preferred = scratch.join("recovery");
        auto store = io::ensurePrivateTransactionStore(a, preferred, scratch.path());
        ASSERT_TRUE(store) << store.error().describe();
        auto durable = storage::HotExitJournal::create(scratch.join("hotexit"));
        ASSERT_TRUE(durable) << durable.error().describe();
        journal.emplace(std::move(*durable));
        documents = std::make_unique<SaveManager>(SaveManagerServices{&fs, &systemClock(), &*journal});
        options = RefactoringOptions{};
        options.recoveryDirectory = *store;
        options.privateStoreForPath = [this, store = *store](std::string_view path) {
            return io::ensurePrivateTransactionStore(path, store, scratch.path());
        };
        options.changed = [this](FileDocument&, const text::ChangeSet&) { ++changed; };
        options.renamed = [this](FileDocument&, std::string_view) { ++renamed; };
        coordinator = std::make_unique<RefactoringCoordinator>(*documents, fs, options);
    }
    void TearDown() override { io::detail::setReplacementObserver(nullptr, nullptr); }
    std::string bytes(std::string_view path) {
        auto value = fs.readFile(path);
        EXPECT_TRUE(value);
        return value ? *value : std::string{};
    }
    FileDocument* open(std::string_view path) {
        auto value = documents->open(path);
        EXPECT_TRUE(value);
        return value ? *value : nullptr;
    }
    RefactoringPlan plan() {
        RefactoringPlan value;
        value.label = "Rename";
        value.disk = {{a, "before a\r\n", "after a\r\n"}, {b, "before b\n", "after b\n"}};
        return value;
    }
};
TEST_F(RefactoringTransactionTest, TwoFilesAndOpenCleanBufferUndoRestoreExactTreeBytes) {
    auto* doc = open(a);
    ASSERT_NE(doc, nullptr);
    const auto initial = doc->document().undoState().stateId;
    auto result = coordinator->apply(plan());
    ASSERT_TRUE(result) << result.error().describe();
    ASSERT_EQ(result->status, RefactoringStatus::Applied);
    EXPECT_EQ(bytes(a), "after a\r\n");
    EXPECT_EQ(bytes(b), "after b\n");
    EXPECT_EQ(doc->document().text().text(), "after a\r\n");
    EXPECT_FALSE(doc->dirty());
    EXPECT_EQ(changed, 1);
    EXPECT_FALSE(doc->document().undo()); // A local undo must not split global disk/document effects.
    auto undone = coordinator->undo(result->token);
    ASSERT_TRUE(undone) << undone.error().describe();
    EXPECT_EQ(undone->status, RefactoringStatus::Undone);
    EXPECT_EQ(bytes(a), "before a\r\n");
    EXPECT_EQ(bytes(b), "before b\n");
    EXPECT_EQ(doc->document().undoState().stateId, initial);
    EXPECT_FALSE(doc->dirty());
    EXPECT_FALSE(doc->document().redo());
    auto redone = coordinator->redo(result->token);
    ASSERT_TRUE(redone) << redone.error().describe();
    EXPECT_EQ(redone->status, RefactoringStatus::Redone);
    EXPECT_EQ(bytes(a), "after a\r\n");
}
TEST_F(RefactoringTransactionTest, DirtyAutomaticBufferIsNotSilentlyPersistedAndUndoRestoresDirtyBefore) {
    auto* doc = open(a);
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(doc->document().replace({ByteOffset{0}, ByteOffset{0}}, "unsaved ", text::EditContext{}));
    const auto before = doc->document().text().text();
    RefactoringPlan value;
    value.label = "dirty rename";
    value.buffers = {{a, before, "renamed unsaved\r\n"}};
    auto result = coordinator->apply(value);
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->status, RefactoringStatus::Applied);
    EXPECT_EQ(bytes(a), "before a\r\n");
    EXPECT_TRUE(doc->dirty());
    auto undone = coordinator->undo(result->token);
    ASSERT_TRUE(undone);
    EXPECT_EQ(undone->status, RefactoringStatus::Undone);
    EXPECT_EQ(doc->document().text().text(), before);
    EXPECT_TRUE(doc->dirty());
    EXPECT_EQ(bytes(a), "before a\r\n");
}
TEST_F(RefactoringTransactionTest, SavingBufferOnlyResultExpiresGlobalUndoInsteadOfRestoringStaleBase) {
    auto* doc = open(a);
    ASSERT_NE(doc, nullptr);
    RefactoringPlan value;
    value.label = "buffer only";
    value.buffers = {{a, "before a\r\n", "after a\r\n", BufferPersistence::BufferOnly}};
    auto applied = coordinator->apply(value);
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied->status, RefactoringStatus::Applied);
    auto saved = documents->save(*doc);
    ASSERT_TRUE(saved) << saved.error().describe();
    EXPECT_EQ(bytes(a), "after a\r\n");
    EXPECT_FALSE(doc->dirty());
    auto refused = coordinator->undo(applied->token);
    EXPECT_FALSE(refused);
    EXPECT_EQ(bytes(a), "after a\r\n");
    EXPECT_EQ(doc->document().text().text(), "after a\r\n");
    EXPECT_FALSE(doc->dirty());
}
TEST_F(RefactoringTransactionTest, ClosedAndReopenedBufferCannotReuseOldUndoAuthority) {
    auto* doc = open(a);
    ASSERT_NE(doc, nullptr);
    auto applied = coordinator->apply(plan());
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied->status, RefactoringStatus::Applied);
    const auto instance = doc->instanceId();
    ASSERT_TRUE(documents->close(*doc));
    auto* replacement = open(a);
    ASSERT_NE(replacement, nullptr);
    EXPECT_NE(replacement->instanceId(), instance);
    EXPECT_FALSE(coordinator->undo(applied->token));
    EXPECT_EQ(bytes(a), "after a\r\n");
    EXPECT_EQ(bytes(b), "after b\n");
}
TEST_F(RefactoringTransactionTest, FileMoveAndMembershipReplacementUndoTogetherWithDirtyDocumentRebind) {
    auto* doc = open(a);
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(doc->document().replace({ByteOffset{0}, ByteOffset{0}}, "dirty ", text::EditContext{}));
    const auto before = doc->document().text().text();
    const auto target = scratch.join("c.cpp");
    RefactoringPlan value;
    value.label = "Rename file";
    value.renames = {{a, target, "before a\r\n"}};
    value.disk = {{project, "a.cpp\nb.cpp\n", "c.cpp\nb.cpp\n"}};
    value.dependencies = {{a, "before a\r\n"}, {target, std::nullopt}, {project, "a.cpp\nb.cpp\n"}};
    auto applied = coordinator->apply(value);
    ASSERT_TRUE(applied) << applied.error().describe();
    ASSERT_EQ(applied->status, RefactoringStatus::Applied);
    EXPECT_EQ(doc->path(), target);
    EXPECT_TRUE(doc->dirty());
    EXPECT_EQ(doc->document().text().text(), before);
    EXPECT_EQ(bytes(target), "before a\r\n");
    EXPECT_EQ(bytes(project), "c.cpp\nb.cpp\n");
    EXPECT_EQ(renamed, 1);
    auto undone = coordinator->undo(applied->token);
    ASSERT_TRUE(undone) << undone.error().describe();
    EXPECT_EQ(undone->status, RefactoringStatus::Undone);
    EXPECT_EQ(doc->path(), a);
    EXPECT_EQ(bytes(a), "before a\r\n");
    EXPECT_EQ(bytes(project), "a.cpp\nb.cpp\n");
    EXPECT_TRUE(doc->dirty());
    EXPECT_EQ(renamed, 2);
}
TEST_F(RefactoringTransactionTest, ExistingMoveDestinationRefusesBeforeAnyOtherParticipantChanges) {
    RefactoringPlan value;
    value.label = "occupied rename";
    value.disk = {{a, "before a\r\n", "after a\r\n"}};
    value.renames = {{project, b, "a.cpp\nb.cpp\n"}};
    auto result = coordinator->apply(value);
    EXPECT_FALSE(result);
    EXPECT_EQ(bytes(a), "before a\r\n");
    EXPECT_EQ(bytes(b), "before b\n");
    EXPECT_EQ(bytes(project), "a.cpp\nb.cpp\n");
}
TEST_F(RefactoringTransactionTest, ForeignFileBeforeUndoRefusesAllParticipantsBeforeAnyInverse) {
    auto applied = coordinator->apply(plan());
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied->status, RefactoringStatus::Applied);
    ASSERT_TRUE(overwriteInterference(b, "foreign b"));
    auto refused = coordinator->undo(applied->token);
    EXPECT_FALSE(refused);
    EXPECT_EQ(bytes(a), "after a\r\n");
    EXPECT_EQ(bytes(b), "foreign b");
}
TEST_F(RefactoringTransactionTest, FinalContextMutationIsRecheckedBeforeDecision) {
    auto value = plan();
    value.revalidateContext = [this]() -> Result<void> { return overwriteInterference(b, "changed during admission"); };
    auto refused = coordinator->apply(value);
    EXPECT_FALSE(refused);
    EXPECT_EQ(bytes(a), "before a\r\n");
    EXPECT_EQ(bytes(b), "changed during admission");
    EXPECT_EQ(changed, 0);
}
TEST_F(RefactoringTransactionTest, UnchangedDirtyReadBufferParticipatesWithoutNewUndoEdge) {
    auto* doc = open(project);
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(doc->document().replace({ByteOffset{0}, ByteOffset{0}}, "#dirty\n", text::EditContext{}));
    const auto state = doc->document().undoState().stateId;
    auto value = plan();
    value.bufferDependencies = {{project, doc->document().text().text()}};
    auto applied = coordinator->apply(value);
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied->status, RefactoringStatus::Applied);
    EXPECT_EQ(doc->document().undoState().stateId, state);
    EXPECT_TRUE(doc->dirty());
    EXPECT_EQ(changed, 0);
}
TEST_F(RefactoringTransactionTest, PendingDecisionDefersFormatsWatchersAndSourceSavesUntilResume) {
    auto* doc = open(a);
    ASSERT_NE(doc, nullptr);
    const auto encoding = doc->encoding();
    bool occupied = false;
    io::detail::setReplacementObserver(
        [](void* context, io::detail::ReplacementBoundary boundary, const io::FileReplacementReceipt& receipt) {
            if (boundary != io::detail::ReplacementBoundary::OriginalRetired)
                return;
            auto& done = *static_cast<bool*>(context);
            if (done)
                return;
            done = io::realFileSystem().writeFileAtomic(receipt.images.targetPath, "foreign interruption").has_value();
        },
        &occupied);
    auto outcome = coordinator->apply(plan());
    io::detail::setReplacementObserver(nullptr, nullptr);
    ASSERT_TRUE(occupied);
    ASSERT_TRUE(outcome);
    ASSERT_EQ(outcome->status, RefactoringStatus::PendingRecovery);
    EXPECT_TRUE(coordinator->pending());
    EXPECT_EQ(coordinator->pendingToken(), outcome->token);
    EXPECT_EQ(changed, 0);
    doc->convertTo(text::Encoding::unicode(text::EncodingForm::Utf16Le), true);
    doc->setLineSeparator(text::LineSeparator::Cr);
    EXPECT_EQ(doc->encoding(), encoding);
    EXPECT_EQ(documents->checkDisk(*doc).kind, ExternalChangeKind::None);
    EXPECT_EQ(doc->pendingMerge(), nullptr);
    EXPECT_FALSE(documents->save(*doc));
    ASSERT_TRUE(fs.removeFile(a)); // Test owns this explicit foreign fixture; coordinator never deletes it.
    auto resumed = coordinator->resume(outcome->token);
    ASSERT_TRUE(resumed) << resumed.error().describe();
    EXPECT_EQ(resumed->status, RefactoringStatus::Applied);
    EXPECT_FALSE(coordinator->pending());
    EXPECT_EQ(bytes(a), "after a\r\n");
    EXPECT_EQ(bytes(b), "after b\n");
    EXPECT_EQ(changed, 1);
}
TEST_F(RefactoringTransactionTest, RestartReadsRealDecisionAndResumesWithoutCopyingOrTrustingOldHandles) {
    auto* doc = open(a);
    ASSERT_NE(doc, nullptr);
    bool occupied = false;
    io::detail::setReplacementObserver(
        [](void* context, io::detail::ReplacementBoundary boundary, const io::FileReplacementReceipt& receipt) {
            if (boundary != io::detail::ReplacementBoundary::OriginalRetired)
                return;
            auto& done = *static_cast<bool*>(context);
            if (done)
                return;
            done = io::realFileSystem().writeFileAtomic(receipt.images.targetPath, "foreign interruption").has_value();
        },
        &occupied);
    auto interrupted = coordinator->apply(plan());
    io::detail::setReplacementObserver(nullptr, nullptr);
    ASSERT_TRUE(interrupted);
    ASSERT_EQ(interrupted->status, RefactoringStatus::PendingRecovery);
    const auto token = interrupted->token;
    coordinator.reset();
    coordinator = std::make_unique<RefactoringCoordinator>(*documents, fs, options);
    auto scan = coordinator->recoverPending();
    ASSERT_TRUE(scan) << scan.error().describe();
    ASSERT_EQ(scan->size(), 1);
    EXPECT_EQ(scan->front().token, token);
    EXPECT_EQ(scan->front().status, RefactoringStatus::PendingRecovery);
    EXPECT_EQ(bytes(a), "foreign interruption");
    EXPECT_EQ(doc->document().text().text(), "before a\r\n");
    ASSERT_TRUE(fs.removeFile(a));
    auto resumed = coordinator->resume(token);
    ASSERT_TRUE(resumed) << resumed.error().describe();
    EXPECT_EQ(resumed->status, RefactoringStatus::Applied);
    EXPECT_FALSE(coordinator->pending());
    EXPECT_EQ(bytes(a), "after a\r\n");
    EXPECT_EQ(doc->document().text().text(), "after a\r\n");
}
TEST_F(RefactoringTransactionTest, DirectoryReadDependencyIsARealKindGuardAtFinalDecision) {
    const auto directory = scratch.join("include");
    ASSERT_TRUE(fs.createDirectories(directory));
    auto value = plan();
    value.dependencies.push_back({directory, std::nullopt, true});
    value.revalidateContext = [&]() -> Result<void> {
        auto removed = fs.removeDirectory(directory);
        if (!removed)
            return removed;
        return fs.writeFileAtomic(directory, "not a directory");
    };
    auto refused = coordinator->apply(value);
    EXPECT_FALSE(refused);
    EXPECT_EQ(bytes(a), "before a\r\n");
    EXPECT_EQ(bytes(b), "before b\n");
}
TEST_F(RefactoringTransactionTest, MissingJournalRefusesDirtyEffectsBeforeDecisionButAllowsCleanPersistedEdits) {
    SaveManager withoutJournal(SaveManagerServices{&fs, &systemClock()});
    RefactoringCoordinator transaction(withoutJournal, fs, options);
    auto doc = withoutJournal.open(a);
    ASSERT_TRUE(doc);
    RefactoringPlan dirty;
    dirty.label = "buffer-only";
    dirty.buffers = {{a, "before a\r\n", "after a\r\n", BufferPersistence::BufferOnly}};
    auto refused = transaction.apply(dirty);
    ASSERT_FALSE(refused);
    EXPECT_EQ(bytes(a), "before a\r\n");
    EXPECT_EQ((*doc)->document().text().text(), "before a\r\n");
    auto clean = transaction.apply(plan());
    ASSERT_TRUE(clean) << clean.error().describe();
    EXPECT_EQ(clean->status, RefactoringStatus::Applied);
    EXPECT_EQ(bytes(a), "after a\r\n");
    EXPECT_FALSE((*doc)->dirty());
}
TEST_F(RefactoringTransactionTest, JournalWriteFailureRetainsOldRenameKeyAndRetriesWithoutReplayingBuffer) {
    auto* doc = open(a);
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(doc->document().replace({ByteOffset{0}, ByteOffset{0}}, "dirty ", text::EditContext{}));
    auto initial = documents->journalDirty();
    ASSERT_TRUE(initial.errors.empty());
    ASSERT_TRUE(journal->database().execute("PRAGMA query_only=ON"));
    const auto target = scratch.join("c.cpp");
    RefactoringPlan value;
    value.label = "Rename";
    value.renames = {{a, target, "before a\r\n"}};
    value.disk = {{project, "a.cpp\nb.cpp\n", "c.cpp\nb.cpp\n"}};
    auto interrupted = coordinator->apply(value);
    ASSERT_TRUE(interrupted);
    ASSERT_EQ(interrupted->status, RefactoringStatus::PendingRecovery);
    const auto appliedState = doc->document().undoState().stateId;
    auto rows = journal->entries();
    ASSERT_TRUE(rows);
    EXPECT_TRUE(std::any_of(rows->begin(), rows->end(),
                            [&](const storage::JournalEntry& e) { return e.state.documentKey == a; }));
    EXPECT_FALSE(std::any_of(rows->begin(), rows->end(),
                             [&](const storage::JournalEntry& e) { return e.state.documentKey == target; }));
    EXPECT_EQ(doc->path(), target);
    EXPECT_TRUE(doc->dirty());
    EXPECT_EQ(renamed, 0);
    ASSERT_TRUE(journal->database().execute("PRAGMA query_only=OFF"));
    auto resumed = coordinator->resume(interrupted->token);
    ASSERT_TRUE(resumed) << resumed.error().describe();
    EXPECT_EQ(resumed->status, RefactoringStatus::Applied);
    EXPECT_EQ(doc->document().undoState().stateId, appliedState); // Durable retry did not replay memory/history.
    rows = journal->entries();
    ASSERT_TRUE(rows);
    EXPECT_FALSE(std::any_of(rows->begin(), rows->end(),
                             [&](const storage::JournalEntry& e) { return e.state.documentKey == a; }));
    EXPECT_TRUE(std::any_of(rows->begin(), rows->end(),
                            [&](const storage::JournalEntry& e) { return e.state.documentKey == target; }));
    EXPECT_EQ(renamed, 1);
    EXPECT_EQ(bytes(target), "before a\r\n");
    EXPECT_EQ(bytes(project), "c.cpp\nb.cpp\n");
}

TEST_F(RefactoringTransactionTest, GeneratedFilesAndProjectMembershipAreOneApplyUndoRedo) {
    const auto generated = scratch.join("generated.cpp");
    RefactoringPlan value;
    value.label = "Generate source";
    value.creates = {{generated, "int generated;\n"}};
    value.disk = {{project, "a.cpp\nb.cpp\n", "a.cpp\nb.cpp\ngenerated.cpp\n"}};
    value.dependencies = {{generated, std::nullopt}};
    auto applied = coordinator->apply(value);
    ASSERT_TRUE(applied) << applied.error().describe();
    ASSERT_EQ(applied->status, RefactoringStatus::Applied);
    EXPECT_EQ(bytes(generated), "int generated;\n");
    EXPECT_EQ(bytes(project), "a.cpp\nb.cpp\ngenerated.cpp\n");
    auto undone = coordinator->undo(applied->token);
    ASSERT_TRUE(undone);
    EXPECT_EQ(undone->status, RefactoringStatus::Undone);
    EXPECT_FALSE(fs.exists(generated).value_or(true));
    EXPECT_EQ(bytes(project), "a.cpp\nb.cpp\n");
    auto redone = coordinator->redo(applied->token);
    ASSERT_TRUE(redone);
    EXPECT_EQ(redone->status, RefactoringStatus::Redone);
    EXPECT_EQ(bytes(generated), "int generated;\n");
    EXPECT_EQ(bytes(project), "a.cpp\nb.cpp\ngenerated.cpp\n");
}

TEST_F(RefactoringTransactionTest, CreationConflictAndDuplicateEffectsRefuseBeforeMembershipChanges) {
    RefactoringPlan value;
    value.label = "Generate";
    value.creates = {{a, "new file"}};
    value.disk = {{project, "a.cpp\nb.cpp\n", "changed"}};
    EXPECT_FALSE(coordinator->apply(value));
    EXPECT_EQ(bytes(a), "before a\r\n");
    EXPECT_EQ(bytes(project), "a.cpp\nb.cpp\n");
    const auto generated = scratch.join("duplicate.cpp");
    value.creates = {{generated, "one"}, {generated, "two"}};
    EXPECT_FALSE(coordinator->apply(value));
    EXPECT_FALSE(fs.exists(generated).value_or(true));
    EXPECT_EQ(bytes(project), "a.cpp\nb.cpp\n");
    value.creates = {{generated, "new"}};
    value.dependencies = {{generated, std::string("existing")}};
    EXPECT_FALSE(coordinator->apply(value));
    EXPECT_FALSE(fs.exists(generated).value_or(true));
}

TEST_F(RefactoringTransactionTest, ForeignGeneratedFileBeforeUndoPreservesWholeAppliedProject) {
    const auto generated = scratch.join("generated.cpp");
    RefactoringPlan value;
    value.creates = {{generated, "created"}};
    value.disk = {{project, "a.cpp\nb.cpp\n", "generated.cpp\n"}};
    auto applied = coordinator->apply(value);
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied->status, RefactoringStatus::Applied);
    ASSERT_TRUE(overwriteInterference(generated, "foreign"));
    EXPECT_FALSE(coordinator->undo(applied->token));
    EXPECT_EQ(bytes(generated), "foreign");
    EXPECT_EQ(bytes(project), "generated.cpp\n");
}

TEST_F(RefactoringTransactionTest, CleanAutoOpenedCreationHasGlobalHistoryAndDirtyViewRefusesInverse) {
    const auto generated = scratch.join("generated.cpp");
    RefactoringPlan value;
    value.creates = {{generated, "created\n"}};
    auto applied = coordinator->apply(value);
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied->status, RefactoringStatus::Applied);
    auto* doc = open(generated);
    ASSERT_NE(doc, nullptr);
    EXPECT_FALSE(doc->dirty());
    EXPECT_EQ(doc->document().undoState().externalUndo, applied->token);
    auto undone = coordinator->undo(applied->token);
    ASSERT_TRUE(undone);
    ASSERT_EQ(undone->status, RefactoringStatus::Undone);
    EXPECT_EQ(doc->diskState(), DiskState::Deleted);
    EXPECT_EQ(doc->document().undoState().externalRedo, applied->token);
    EXPECT_EQ(doc->document().text().text(), "created\n");
    EXPECT_FALSE(doc->dirty());
    auto redone = coordinator->redo(applied->token);
    ASSERT_TRUE(redone);
    ASSERT_EQ(redone->status, RefactoringStatus::Redone);
    EXPECT_EQ(doc->diskState(), DiskState::InSync);
    EXPECT_FALSE(doc->dirty());
    auto edited = doc->document().replace({0, 0}, "dirty ", {});
    ASSERT_TRUE(edited);
    EXPECT_FALSE(coordinator->undo(applied->token));
    EXPECT_EQ(bytes(generated), "created\n");
    EXPECT_EQ(doc->document().text().text(), "dirty created\n");
}

TEST_F(RefactoringTransactionTest, SavingUndoneCreationCreatesForeignObjectAndRedoRefusesIt) {
    const auto generated = scratch.join("generated.cpp");
    RefactoringPlan value;
    value.creates = {{generated, "created\n"}};
    auto applied = coordinator->apply(value);
    ASSERT_TRUE(applied);
    auto* doc = open(generated);
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(coordinator->undo(applied->token));
    ASSERT_EQ(doc->diskState(), DiskState::Deleted);
    auto saved = documents->save(*doc);
    ASSERT_TRUE(saved);
    EXPECT_FALSE(coordinator->redo(applied->token));
    EXPECT_EQ(bytes(generated), "created\n");
}

TEST_F(RefactoringTransactionTest, RestartRecoversPartialCreationAndMembershipFromFrozenReceipt) {
    const auto generated = scratch.join("generated.cpp");
    RefactoringPlan value;
    value.disk = {{project, "a.cpp\nb.cpp\n", "generated.cpp\n"}};
    value.creates = {{generated, "created\n"}};
    struct Actor {
        std::string path;
        bool occupied = false;
    } actor{generated};
    io::detail::setReplacementObserver(
        [](void* context, io::detail::ReplacementBoundary boundary, const io::FileReplacementReceipt&) {
            auto& activeActor = *static_cast<Actor*>(context);
            if (boundary == io::detail::ReplacementBoundary::OriginalRetired && !activeActor.occupied)
                activeActor.occupied = io::realFileSystem().writeFileAtomic(activeActor.path, "foreign").has_value();
        },
        &actor);
    auto pending = coordinator->apply(value);
    io::detail::setReplacementObserver(nullptr, nullptr);
    ASSERT_TRUE(actor.occupied);
    ASSERT_TRUE(pending);
    ASSERT_EQ(pending->status, RefactoringStatus::PendingRecovery);
    const auto token = pending->token;
    EXPECT_EQ(bytes(generated), "foreign");
    coordinator.reset();
    coordinator = std::make_unique<RefactoringCoordinator>(*documents, fs, options);
    auto recovered = coordinator->recoverPending();
    ASSERT_TRUE(recovered);
    ASSERT_EQ(recovered->size(), 1U);
    EXPECT_EQ(recovered->front().status, RefactoringStatus::PendingRecovery);
    EXPECT_EQ(bytes(generated), "foreign");
    ASSERT_TRUE(fs.removeFile(generated)); // Explicit test actor removes only its known foreign file.
    auto resumed = coordinator->resume(token);
    ASSERT_TRUE(resumed) << resumed.error().describe();
    EXPECT_EQ(resumed->status, RefactoringStatus::Applied);
    EXPECT_EQ(bytes(generated), "created\n");
    EXPECT_EQ(bytes(project), "generated.cpp\n");
}

TEST_F(RefactoringTransactionTest, CreateAncestorAliasesRefuseBeforeAnyMembershipOrFileEffects) {
    const auto real = scratch.join("real");
    const auto alias = scratch.join("alias");
    ASSERT_TRUE(fs.createDirectories(scratch.join("real/sub")));
    frost::test::makeDirectoryLink(alias, real);
    const auto direct = scratch.join("real/sub/generated.cpp");
    const auto aliased = scratch.join("alias/sub/generated.cpp");
    RefactoringPlan value;
    value.disk = {{project, "a.cpp\nb.cpp\n", "generated.cpp\n"}};
    value.creates = {{direct, "first"}, {aliased, "second"}};
    auto refused = coordinator->apply(value);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(fs.exists(direct).value_or(true));
    EXPECT_FALSE(fs.exists(aliased).value_or(true));
    EXPECT_EQ(bytes(project), "a.cpp\nb.cpp\n");
    // A rename destination and Create must also not claim the same absent native endpoint.
    value.creates = {{aliased, "second"}};
    value.renames = {{a, direct, "before a\r\n"}};
    auto mixed = coordinator->apply(value);
    ASSERT_FALSE(mixed);
    EXPECT_EQ(mixed.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(bytes(a), "before a\r\n");
    EXPECT_FALSE(fs.exists(direct).value_or(true));
    EXPECT_EQ(bytes(project), "a.cpp\nb.cpp\n");
}

#ifdef _WIN32
TEST_F(RefactoringTransactionTest, UnicodeCaseAliasesCannotOwnTwoAbsentCreationEndpoints) {
    RefactoringPlan value;
    const auto upper = scratch.join("\xC3\x84.cpp");
    const auto lower = scratch.join("\xC3\xA4.cpp");
    value.creates = {{upper, "one"}, {lower, "two"}};
    value.disk = {{project, "a.cpp\nb.cpp\n", "changed"}};
    auto refused = coordinator->apply(value);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(fs.exists(upper).value_or(true));
    EXPECT_FALSE(fs.exists(lower).value_or(true));
    EXPECT_EQ(bytes(project), "a.cpp\nb.cpp\n");
}
#endif

TEST_F(RefactoringTransactionTest, AliasedCreatedViewFormatChangeAfterUndoRefusesRedoBeforeMembership) {
    const auto real = scratch.join("real");
    const auto alias = scratch.join("alias");
    ASSERT_TRUE(fs.createDirectories(scratch.join("real/sub")));
    frost::test::makeDirectoryLink(alias, real);
    const auto generated = scratch.join("real/sub/generated.cpp");
    const auto spelling = scratch.join("alias/sub/generated.cpp");
    RefactoringPlan value;
    value.creates = {{generated, "created\n"}};
    value.disk = {{project, "a.cpp\nb.cpp\n", "generated.cpp\n"}};
    auto applied = coordinator->apply(value);
    ASSERT_TRUE(applied);
    ASSERT_EQ(applied->status, RefactoringStatus::Applied);
    auto* doc = open(spelling);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->document().undoState().externalUndo, applied->token);
    ASSERT_TRUE(coordinator->undo(applied->token));
    EXPECT_EQ(doc->diskState(), DiskState::Deleted);
    EXPECT_EQ(doc->document().undoState().externalRedo, applied->token);
    doc->setLineSeparator(text::LineSeparator::Cr);
    ASSERT_TRUE(doc->dirty());
    EXPECT_FALSE(coordinator->redo(applied->token));
    EXPECT_FALSE(fs.exists(generated).value_or(true));
    EXPECT_EQ(bytes(project), "a.cpp\nb.cpp\n");
    EXPECT_EQ(doc->encoding().format.lineSeparator, text::LineSeparator::Cr);
}

TEST_F(RefactoringTransactionTest, MultipleOpenCreationSpellingsNeverBypassDirtyViewAdmission) {
    const auto real = scratch.join("real");
    const auto alias = scratch.join("alias");
    ASSERT_TRUE(fs.createDirectories(scratch.join("real/sub")));
    frost::test::makeDirectoryLink(alias, real);
    const auto generated = scratch.join("real/sub/generated.cpp");
    RefactoringPlan value;
    value.creates = {{generated, "created\n"}};
    auto applied = coordinator->apply(value);
    ASSERT_TRUE(applied);
    auto* direct = open(generated);
    auto* second = open(scratch.join("alias/sub/generated.cpp"));
    ASSERT_NE(direct, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(direct, second);
    ASSERT_TRUE(second->document().replace({0, 0}, "dirty ", {}));
    EXPECT_FALSE(coordinator->undo(applied->token));
    EXPECT_EQ(bytes(generated), "created\n");
    EXPECT_EQ(second->document().text().text(), "dirty created\n");
}

} // namespace
} // namespace frost::document::test
