// SPDX-License-Identifier: Apache-2.0
// One owner-thread multi-file/source/project operation and routed global Undo/Redo (REF-1).
#pragma once
#include "frost/core/error.hpp"
#include "frost/core/transaction_id.hpp"
#include "frost/document/save_manager.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace frost::document {
struct DiskReplacement {
    std::string path;
    std::string expectedBytes;
    std::string afterBytes;
};
// Absent, closed endpoint in an existing directory. Bytes are staged privately; never prewrite.
struct FileCreation {
    std::string path;
    std::string afterBytes;
};
struct FileRename {
    std::string from;
    std::string to;
    std::string expectedBytes;
};
enum class BufferPersistence { Automatic, BufferOnly, Persist };
struct BufferReplacement {
    std::string path;
    std::string expectedText;
    std::string afterText;
    BufferPersistence persistence = BufferPersistence::Automatic;
};
struct ReadDependency {
    std::string path;
    std::optional<std::string> expectedBytes; // nullopt means observed absence, not unreadable.
    bool directory = false;                   // True requires null expectedBytes and an observed actual directory.
};
struct BufferDependency {
    std::string path;
    std::string expectedText;
};
struct RefactoringPlan {
    std::string label;
    std::vector<DiskReplacement> disk;
    std::vector<BufferReplacement> buffers;
    std::vector<ReadDependency> dependencies;
    std::vector<FileRename> renames;
    std::vector<BufferDependency> bufferDependencies;
    std::vector<FileCreation> creates;
    // Final initial admission only; caller freezes captured preview/config/trust generations.
    std::function<Result<void>()> revalidateContext;
};
enum class RefactoringStatus { Applied, Undone, Redone, PendingRecovery };
struct RefactoringOutcome {
    TransactionId token;
    RefactoringStatus status = RefactoringStatus::PendingRecovery;
    std::vector<Error> warnings;
};
struct RefactoringOptions {
    // Existing owner-private directory, outside the workspace. Records are retained, never swept
    // automatically. The callback supplies an existing restricted SAME-VOLUME store for each path.
    std::string recoveryDirectory;
    std::function<Result<std::string>(std::string_view)> privateStoreForPath;
    std::size_t maxFileBytes = 64 * 1024 * 1024;
    std::size_t maxParticipants = 4096;
    std::size_t maxReadDependencies = 32768;
    std::size_t maxTotalBytes = 512 * 1024 * 1024;
    // Current project/trust barrier before every new decision; not called to abandon a decided recovery.
    std::function<Result<void>()> admit;
    // Owner thread, after ALL disk and document effects settle. Each ChangeSet must be followed
    // by all views/LSP synchronizers of that FileDocument. Not called on predecision refusal or
    // while a committed operation is pending recovery; callback must not throw or reenter Apply.
    std::function<void(FileDocument&, const text::ChangeSet&)> changed;
    // After settlement and SaveManager rebind, before changed(). Close old URI/open new URI,
    // update tabs/project navigation. oldPath is valid for this synchronous callback only.
    std::function<void(FileDocument&, std::string_view oldPath)> renamed;
};
/*
 * SaveManager/fs outlive the coordinator; one coordinator per SaveManager. Owner-thread only; synchronous native
 * preparation. All predecision failures return unexpected with workspace/buffers unchanged. After a durable decision,
 * any incomplete settlement returns a VALUE with PendingRecovery and retained token; never treat it as unapplied.
 * resume() retries the frozen operation, never a changed preview. External filesystem readers may see per-file
 * transitions/gaps. Native participants enforce their explicitly limited metadata/platform profile; no universal
 * metadata or power-loss claim. Undo/redo revalidate every disk participant and exact document boundary before any
 * inverse. Created files can be opened clean after Apply. Global Undo leaves that view intact in the ordinary Deleted
 * disk state; Redo returns it InSync. Dirty/foreign views refuse history before effects. Saving a Deleted view creates
 * a foreign object and makes Redo refuse. Local Document Undo/Redo cannot split a global edge even if this coordinator
 * is destroyed.
 */
class RefactoringCoordinator {
public:
    RefactoringCoordinator(SaveManager& documents, io::IFileSystem& fs, RefactoringOptions options);
    ~RefactoringCoordinator();
    RefactoringCoordinator(const RefactoringCoordinator&) = delete;
    RefactoringCoordinator& operator=(const RefactoringCoordinator&) = delete;
    [[nodiscard]] Result<RefactoringOutcome> apply(const RefactoringPlan& plan);
    [[nodiscard]] Result<RefactoringOutcome> undo(TransactionId token);
    [[nodiscard]] Result<RefactoringOutcome> redo(TransactionId token);
    [[nodiscard]] Result<RefactoringOutcome> resume(TransactionId token);
    [[nodiscard]] TransactionId latestUndo() const noexcept;
    [[nodiscard]] TransactionId latestRedo() const noexcept;
    [[nodiscard]] bool pending() const noexcept;
    [[nodiscard]] TransactionId pendingToken() const noexcept;
    // Explicit startup/lazy admission before a new operation. Rebuilds only undecided/unfinished
    // owned journal entries; corrupt/foreign layouts remain reported PendingRecovery conflicts.
    [[nodiscard]] Result<std::vector<RefactoringOutcome>> recoverPending();

private:
    struct State;
    std::unique_ptr<State> m_state;
};
} // namespace frost::document
