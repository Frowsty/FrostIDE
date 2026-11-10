// SPDX-License-Identifier: Apache-2.0
//
// GitController (M5, ADR 0048): the GUI-thread face of one frost::vcs::GitService. It marshals the
// service's worker callbacks to the GUI thread as signals, caches the last status and branch list,
// keeps the commit message history, turns each operation into a background task and a notification,
// and offers the file reads (HEAD and index versions) with GUI-thread completions. The Commit
// window, the diff viewer, the change markers, the branch widget and the git console bind to it.
/*
 * Threads. Every member runs on the GUI thread (the controller's thread). The service's callbacks
 * run on its workers and reach the controller through queued calls, which Qt drops once the
 * controller is gone. The destructor first closes the callbacks' gate, so no worker reaches the
 * controller afterwards, then cancels the running operation and either hands the service to the
 * retirer (setServiceRetirer: it is joined off the GUI thread, FND-9b) or joins its workers itself.
 *
 * Errors of operations are not returned: operationFinished carries them, a notification shows them
 * (when a NotificationCenter is set) and lastResult() keeps the last one. A checkout the work tree
 * blocks finishes with its refusal message ("Checkout of 'x' was refused: ..."); a commit whose
 * pre-commit hook fails finishes with the hook's output, and the Commit window keeps its selection.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/file_system.hpp"
#include "frost/vcs/git_repository.hpp"
#include "frost/vcs/git_runner.hpp"
#include "frost/vcs/git_service.hpp"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace frost::ui {

class BackgroundTask;
class BackgroundTaskManager;
class NotificationCenter;

namespace detail {
// Shared by a GitController and the callbacks it gives its service: a worker posts to the
// controller only while `open`, under `mutex`, and the destructor closes it first.
struct GitCallbackGate {
    Mutex mutex;
    bool open FROST_GUARDED_BY(mutex) = true;
};
} // namespace detail

class GitController final : public QObject {
    Q_OBJECT

public:
    // How many commit messages messageHistory() keeps.
    static constexpr std::size_t kMessageHistory = 20;

    // Starts a service for `info` (see GitService::create). `fs` must outlive the controller.
    GitController(vcs::GitRunner runner, io::IFileSystem& fs, vcs::RepositoryInfo info,
                  vcs::GitServiceOptions options = {}, QObject* parent = nullptr);
    ~GitController() override;

    /*
     * Receives the service when the controller is destroyed, after the callbacks' gate is closed and
     * the running operation is cancelled, to destroy it (join its workers) off the GUI thread: a git
     * process that is just starting or ending can hold that join for hundreds of milliseconds
     * (FND-9b; R3's j2_run measured 600 ms). The retirer must destroy the service before the file
     * system and launcher it uses go. Unset, the destructor destroys the service itself.
     */
    using ServiceRetirer = std::function<void(std::unique_ptr<vcs::GitService>)>;
    void setServiceRetirer(ServiceRetirer retire) { m_retire = std::move(retire); }

    [[nodiscard]] vcs::GitService& service() noexcept { return *m_service; }
    [[nodiscard]] const vcs::RepositoryInfo& info() const noexcept { return m_service->info(); }
    // The per-file statuses (thread-safe; the colour providers read it).
    [[nodiscard]] const vcs::IVcsStatusProvider& statuses() const noexcept { return *m_service; }

    // Optional shell services: operations show as background tasks and end with notifications.
    // Both must outlive the controller or be reset to nullptr.
    void setTaskManager(BackgroundTaskManager* tasks) noexcept { m_tasks = tasks; }
    void setNotifications(NotificationCenter* notifications) noexcept { m_notifications = notifications; }

    // The last status that reached the GUI thread (nullptr before the first).
    [[nodiscard]] const std::shared_ptr<const vcs::GitStatusSnapshot>& snapshot() const noexcept { return m_snapshot; }
    // The current branch ("" when detached or before the first status) and whether HEAD is detached.
    [[nodiscard]] QString currentBranch() const;
    [[nodiscard]] bool detached() const;
    // The last branch list readBranches() delivered.
    [[nodiscard]] const std::vector<vcs::GitBranch>& branches() const noexcept { return m_branches; }
    // Commit messages, newest first: the ones committed here, then the repository's recent ones.
    [[nodiscard]] const QStringList& messageHistory() const noexcept { return m_history; }
    // The last operation result that reached the GUI thread.
    [[nodiscard]] const std::optional<vcs::GitOperationResult>& lastResult() const noexcept { return m_lastResult; }
    // True while an operation runs or waits.
    [[nodiscard]] bool busy() const;

    // --- Operations (queued on the service; each ends with operationFinished). ---
    void commit(std::vector<std::string> paths, const QString& message, bool amend, bool pushAfter = false);
    void rollback(std::vector<std::string> paths);
    void checkout(const QString& branch);
    void createBranch(const QString& name);
    void fetch();
    void pull(vcs::PullStrategy strategy = vcs::PullStrategy::Merge);
    // `forceWithLease` needs `confirmed` (the UI's confirmation), or the push fails at once.
    void push(bool forceWithLease = false, bool confirmed = false);
    void cherryPick(const QString& commitHash);
    void revertCommit(const QString& commitHash);
    void resetBranch(const QString& commitHash, const QString& mode = QStringLiteral("mixed"));
    void createTag(const QString& tagName, const QString& commitHash = {});
    void createBranchAt(const QString& name, const QString& startPoint);
    void mergeBranch(const QString& branch);
    void rebaseBranch(const QString& ontoBranch);
    void deleteBranch(const QString& branch, bool force = false);
    void renameBranch(const QString& oldName, const QString& newName);
    void stashPush(const QString& message = {}, bool includeUntracked = true);
    void stashApply(int index = 0, bool pop = true);
    void stashDrop(int index = 0);
    void cancelOperation();
    void refreshStatus();
    // Owner-thread gate applied to every mutating operation, including direct widget calls.
    using OperationGate = std::function<Result<void>(vcs::GitOperationKind, const std::vector<std::string>&)>;
    void setOperationGate(OperationGate gate) { m_operationGate = std::move(gate); }

    // --- Reads with GUI-thread completions (dropped when the controller is gone first). ---
    using ContentCallback = std::function<void(Result<std::optional<std::string>>)>;
    void readHeadContent(const std::string& absolutePath, ContentCallback done);
    void readIndexContent(const std::string& absolutePath, ContentCallback done);
    void readLastCommitMessage(std::function<void(Result<std::string>)> done);
    // Refreshes branches() and emits branchesChanged.
    void readBranches();
    void readLog(std::size_t maxCount, const QString& branchFilter, const QString& searchFilter,
                 std::function<void(Result<std::vector<vcs::GitCommitEntry>>)> done);
    void readCommitFiles(const QString& commitHash,
                         std::function<void(Result<std::vector<vcs::GitCommitFileChange>>)> done);
    void readStashes(std::function<void(Result<std::vector<vcs::GitStashEntry>>)> done);
    void readBlame(const QString& relativeOrAbsolutePath,
                   std::function<void(Result<std::vector<vcs::GitBlameLine>>)> done);

    // Tests: waits until the service is idle, then delivers the queued signals. False on timeout.
    [[nodiscard]] bool waitForIdle(std::chrono::milliseconds timeout);

signals:
    void statusChanged();
    void statusFailed(const QString& message);
    void branchesChanged();
    // One console line: stream is a vcs::GitStream (Stdout, Stderr, Command).
    void outputLine(int stream, const QString& line);
    void operationStarted(int kind);
    // `kind` is a vcs::GitOperationKind; `message` is the error's message, or "" on success.
    void operationFinished(int kind, bool succeeded, const QString& message);
    void historyChanged();

private:
    [[nodiscard]] bool allow(vcs::GitOperationKind kind, const std::vector<std::string>& paths = {});
    void onStatus(std::shared_ptr<const vcs::GitStatusSnapshot> snapshot);
    void onStarted(vcs::GitOperationKind kind);
    void onFinished(const vcs::GitOperationResult& result);
    void remember(const QString& message);

    OperationGate m_operationGate;
    // Before m_service: the service's callbacks capture it.
    std::shared_ptr<detail::GitCallbackGate> m_gate = std::make_shared<detail::GitCallbackGate>();
    ServiceRetirer m_retire;
    std::unique_ptr<vcs::GitService> m_service;
    std::shared_ptr<const vcs::GitStatusSnapshot> m_snapshot;
    std::vector<vcs::GitBranch> m_branches;
    QStringList m_history;
    std::optional<vcs::GitOperationResult> m_lastResult;
    QString m_pendingMessage; // The message of the commit in flight, for the history.
    bool m_pushAfterCommit = false;
    std::optional<vcs::GitOperationKind> m_pendingOperation;
    BackgroundTaskManager* m_tasks = nullptr;
    NotificationCenter* m_notifications = nullptr;
    std::vector<std::shared_ptr<BackgroundTask>> m_running; // Oldest first.
};

} // namespace frost::ui
