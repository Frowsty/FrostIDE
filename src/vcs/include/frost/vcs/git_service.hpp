// SPDX-License-Identifier: Apache-2.0
//
// GitService (M5, ADR 0048): the Qt-free git service for one work tree that the Commit window, the
// VCS colours, the editor's change markers, the branch widget and the git console bind to. It owns
// the GitRunner and GitRepository, keeps the current status (and the per-file VcsStatusMap it
// provides), refreshes it debounced after file changes and at once after its own commands, and
// runs the mutating operations one at a time with their output streamed.
/*
 * Threads. Two workers: the read queue (status refreshes, file versions, branch lists, messages)
 * and the operation queue (commit, rollback, checkout, new branch, fetch, pull, push), so a long
 * fetch never holds up the colours. Every public member is thread-safe. Callbacks run on those
 * workers, never on the caller's thread; onOutput and onOperation* come from the operation queue in
 * order; the UI marshals what it needs (frost::ui's GitController).
 *
 * Refresh. applyChanges() (or follow() pumping a watch) looks at a file-watcher batch: changes in
 * the work tree, and in the git directory only to index, HEAD, packed-refs, refs/ and the merge,
 * rebase and cherry-pick state files, schedule a refresh after `debounce` of quiet, at most
 * `maxDelay` after the first change. Every finished operation refreshes at once. The status runs
 * with GIT_OPTIONAL_LOCKS=0, so it never writes the index and its own refresh never wakes it again.
 *
 * Lifetime. The file system must outlive the service. The destructor cancels the running
 * operation (its process tree ends), drops queued work and joins the workers; callbacks still
 * pending are not called.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/sched/task.hpp"
#include "frost/vcs/git_repository.hpp"
#include "frost/vcs/git_runner.hpp"
#include "frost/vcs/git_status.hpp"
#include "frost/vcs/vcs_status.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::vcs {

enum class GitOperationKind : std::uint8_t { Commit, Rollback, Checkout, CreateBranch, Fetch, Pull, Push };

// "Commit", "Rollback", "Checkout", "New Branch", "Fetch", "Pull", "Push" (English titles).
[[nodiscard]] std::string_view toString(GitOperationKind kind) noexcept;

// How one operation ended.
struct GitOperationResult {
    GitOperationKind kind = GitOperationKind::Commit;
    std::optional<Error> error; // Empty on success.
    std::string output;         // git's output (a commit's hook output included).
    std::string commit;         // Commit: the new HEAD.
    std::string target;         // Checkout and CreateBranch: the branch.

    [[nodiscard]] bool succeeded() const noexcept { return !error.has_value(); }
};

// One refreshed status of the work tree.
struct GitStatusSnapshot {
    GitStatus status;
    std::shared_ptr<const VcsStatusMap> files; // Never null.
    std::uint64_t generation = 0;              // 1, 2, ... per refresh that succeeded.
};

// Every callback is optional (header comment for the threads they run on).
struct GitServiceCallbacks {
    std::function<void(std::shared_ptr<const GitStatusSnapshot> snapshot)> onStatusChanged;
    std::function<void(const Error& error)> onStatusFailed;
    // The git console: each command line (GitStream::Command) and each line git printed.
    std::function<void(GitStream stream, std::string_view line)> onOutput;
    std::function<void(GitOperationKind kind)> onOperationStarted;
    std::function<void(const GitOperationResult& result)> onOperationFinished;
};

struct GitServiceOptions {
    IClock::Duration debounce = std::chrono::milliseconds(300);
    IClock::Duration maxDelay = std::chrono::seconds(2);
};

class GitService final : public IVcsStatusProvider {
public:
    // Starts the workers and the first status refresh. `fs` must outlive the service.
    [[nodiscard]] static std::unique_ptr<GitService> create(GitRunner runner, io::IFileSystem& fs, RepositoryInfo info,
                                                            GitServiceOptions options = {},
                                                            GitServiceCallbacks callbacks = {});
    ~GitService();
    GitService(const GitService&) = delete ("owns threads and a repository");
    GitService& operator=(const GitService&) = delete ("owns threads and a repository");

    [[nodiscard]] const RepositoryInfo& info() const noexcept;
    // The repository, for synchronous calls on the caller's own thread (tests, background code).
    [[nodiscard]] const GitRepository& repository() const noexcept;

    // The last status; nullptr before the first refresh finished.
    [[nodiscard]] std::shared_ptr<const GitStatusSnapshot> snapshot() const;
    // IVcsStatusProvider over the last status (nullopt before the first refresh).
    [[nodiscard]] std::optional<VcsFileStatus> statusOf(std::string_view absolutePath) const override;
    [[nodiscard]] std::uint64_t generation() const noexcept override;

    // Refreshes the status now (after work already queued on the read queue).
    void refreshStatus();
    // Looks at a watcher batch and schedules a debounced refresh when it matters (header comment).
    void applyChanges(const io::FileChangeBatch& batch);
    // Whether a change at `path` makes the status stale (header comment). Pure apart from info().
    [[nodiscard]] bool affectsStatus(std::string_view path) const;
    // Pumps `watch` into applyChanges until it ends (its error: Cancelled, NotFound). The service and
    // the watch must outlive the task.
    [[nodiscard]] sched::Task<Result<void>> follow(io::IDirectoryWatch& watch, std::stop_token token = {});

    // --- Operations: queued; each ends with onOperationFinished, then a status refresh. ---
    // `pushAfter`: Commit and Push (a Push operation follows a commit that succeeded).
    void commit(CommitRequest request, bool pushAfter = false);
    void rollback(std::vector<std::string> absolutePaths);
    void checkout(std::string branch);
    void createBranch(std::string name);
    void fetch();
    void pull(PullStrategy strategy = PullStrategy::Merge);
    void push(PushOptions options = {});
    // Stops the running operation (its process tree ends; the result is Cancelled).
    void cancelOperation();
    // True while an operation runs or waits.
    [[nodiscard]] bool operationBusy() const;

    // --- Reads: run on the read queue; `done` is called there. ---
    void readHeadContent(std::string absolutePath, std::function<void(Result<std::optional<std::string>>)> done);
    void readIndexContent(std::string absolutePath, std::function<void(Result<std::optional<std::string>>)> done);
    void readBranches(std::function<void(Result<std::vector<GitBranch>>)> done);
    void readLastCommitMessage(std::function<void(Result<std::string>)> done);
    void readRecentCommitMessages(std::size_t count, std::function<void(Result<std::vector<std::string>>)> done);

    // Waits until both queues are idle and no refresh is pending, or `timeout` passes; returns
    // whether it got there. Not from a callback.
    [[nodiscard]] bool waitForIdle(std::chrono::milliseconds timeout) const;

    struct State; // Defined in the implementation.

private:
    explicit GitService(std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> m_state;
};

} // namespace frost::vcs
