// SPDX-License-Identifier: Apache-2.0
//
// GitController (frost/ui/vcs/git_controller.hpp): the queued relays, the caches, the history,
// the background tasks and the notifications.

#include "frost/ui/vcs/git_controller.hpp"

#include "frost/ui/shell/background_tasks.hpp"
#include "frost/ui/shell/notifications.hpp"

#include <QCoreApplication>
#include <QMetaObject>

#include <algorithm>

namespace frost::ui {
namespace {

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

// The background task's title for an operation.
QString taskTitle(vcs::GitOperationKind kind) {
    using K = vcs::GitOperationKind;
    switch (kind) {
    case K::Commit: return QCoreApplication::translate("GitController", "Committing");
    case K::Rollback: return QCoreApplication::translate("GitController", "Rolling back");
    case K::Checkout: return QCoreApplication::translate("GitController", "Checking out");
    case K::CreateBranch: return QCoreApplication::translate("GitController", "Creating a branch");
    case K::Fetch: return QCoreApplication::translate("GitController", "Fetching");
    case K::Pull: return QCoreApplication::translate("GitController", "Updating the project");
    case K::Push: return QCoreApplication::translate("GitController", "Pushing");
    case K::CherryPick: return QCoreApplication::translate("GitController", "Cherry-picking");
    case K::Revert: return QCoreApplication::translate("GitController", "Reverting commit");
    case K::Reset: return QCoreApplication::translate("GitController", "Resetting branch");
    case K::Tag: return QCoreApplication::translate("GitController", "Creating tag");
    case K::Merge: return QCoreApplication::translate("GitController", "Merging branch");
    case K::Rebase: return QCoreApplication::translate("GitController", "Rebasing branch");
    case K::DeleteBranch: return QCoreApplication::translate("GitController", "Deleting branch");
    case K::RenameBranch: return QCoreApplication::translate("GitController", "Renaming branch");
    case K::Stash: return QCoreApplication::translate("GitController", "Stashing changes");
    case K::Unstash: return QCoreApplication::translate("GitController", "Unstashing changes");
    case K::DropStash: return QCoreApplication::translate("GitController", "Dropping stash");
    }
    return QCoreApplication::translate("GitController", "Running git");
}

// The worker side of every queued call to a controller: posts `call` unless the controller's
// destructor has closed `gate`. Holding the gate's mutex while posting orders the post before the
// destructor, which closes the gate under the same mutex before anything of the controller goes.
template<class Call>
void postTo(GitController* controller, const std::shared_ptr<detail::GitCallbackGate>& gate, Call&& call,
            Qt::ConnectionType type) {
    LockGuard lock(gate->mutex);
    if (gate->open)
        QMetaObject::invokeMethod(controller, std::forward<Call>(call), type);
}

} // namespace

GitController::GitController(vcs::GitRunner runner, io::IFileSystem& fs, vcs::RepositoryInfo info,
                             vcs::GitServiceOptions options, QObject* parent)
    : QObject(parent) {
    // The callbacks run on the service's workers; each posts to this object through postTo, which
    // posts only while the gate is open (the destructor closes it first), and Qt drops posts that
    // are still queued when the object goes.
    const auto gate = m_gate;
    vcs::GitServiceCallbacks callbacks;
    callbacks.onStatusChanged = [this, gate](std::shared_ptr<const vcs::GitStatusSnapshot> snapshot) {
        postTo(this, gate, [this, snapshot] { onStatus(snapshot); }, Qt::QueuedConnection);
    };
    callbacks.onStatusFailed = [this, gate](const Error& error) {
        postTo(this, gate, [this, message = qs(error.message())] { emit statusFailed(message); }, Qt::QueuedConnection);
    };
    callbacks.onOutput = [this, gate](vcs::GitStream stream, std::string_view line) {
        postTo(
            this, gate, [this, stream, text = qs(line)] { emit outputLine(static_cast<int>(stream), text); },
            Qt::QueuedConnection);
    };
    callbacks.onOperationStarted = [this, gate](vcs::GitOperationKind kind) {
        postTo(this, gate, [this, kind] { onStarted(kind); }, Qt::QueuedConnection);
    };
    callbacks.onOperationFinished = [this, gate](const vcs::GitOperationResult& result) {
        postTo(this, gate, [this, result] { onFinished(result); }, Qt::QueuedConnection);
    };
    m_service = vcs::GitService::create(std::move(runner), fs, std::move(info), options, std::move(callbacks));
    m_service->readRecentCommitMessages(kMessageHistory, [this, gate](Result<std::vector<std::string>> messages) {
        if (!messages) {
            return;
        }
        postTo(
            this, gate,
            [this, list = std::move(*messages)] {
                for (const std::string& message : list) {
                    const QString text = qs(message);
                    if (!m_history.contains(text) && m_history.size() < static_cast<qsizetype>(kMessageHistory)) {
                        m_history.push_back(text);
                    }
                }
                emit historyChanged();
            },
            Qt::QueuedConnection);
    });
}

GitController::~GitController() {
    {
        LockGuard lock(m_gate->mutex);
        m_gate->open = false; // From here no worker posts to this object.
    }
    if (m_service && m_retire) {
        m_service->cancelOperation();
        m_retire(std::move(m_service)); // Joined off the GUI thread (FND-9b).
    }
    // Otherwise: cancels the running operation, drops queued work and joins the workers here.
    m_service.reset();
    for (const std::shared_ptr<BackgroundTask>& task : m_running) {
        task->finish();
    }
}

QString GitController::currentBranch() const { return m_snapshot ? qs(m_snapshot->status.branch.head) : QString(); }

bool GitController::detached() const { return m_snapshot && m_snapshot->status.branch.detached(); }

bool GitController::busy() const { return m_pendingOperation.has_value(); }

bool GitController::allow(vcs::GitOperationKind kind, const std::vector<std::string>& paths) {
    // Remain busy until the GUI receives completion, not only until the worker becomes idle.
    if (busy()) {
        if (m_notifications)
            m_notifications->notify(Notification{
                tr("Git is busy"), tr("Wait for the current operation."), NotificationKind::Warning, false, {}});
        return false;
    }
    auto allowed = m_operationGate ? m_operationGate(kind, paths) : Result<void>{};
    if (allowed) {
        m_pendingOperation = kind;
        return true;
    }
    vcs::GitOperationResult result;
    result.kind = kind;
    result.error = std::move(allowed.error());
    m_lastResult = result;
    emit operationFinished(static_cast<int>(kind), false, qs(result.error->message()));
    return false;
}

void GitController::commit(std::vector<std::string> paths, const QString& message, bool amend, bool pushAfter) {
    if (!allow(vcs::GitOperationKind::Commit, paths))
        return;
    m_pendingMessage = message.trimmed();
    vcs::CommitRequest request;
    request.paths = std::move(paths);
    request.message = message.toStdString();
    request.amend = amend;
    m_pushAfterCommit = pushAfter;
    m_service->commit(std::move(request), false);
}

void GitController::rollback(std::vector<std::string> paths) {
    if (allow(vcs::GitOperationKind::Rollback, paths))
        m_service->rollback(std::move(paths));
}

void GitController::checkout(const QString& branch) {
    if (allow(vcs::GitOperationKind::Checkout))
        m_service->checkout(branch.toStdString());
}

void GitController::createBranch(const QString& name) {
    if (allow(vcs::GitOperationKind::CreateBranch))
        m_service->createBranch(name.trimmed().toStdString());
}

void GitController::fetch() {
    if (allow(vcs::GitOperationKind::Fetch))
        m_service->fetch();
}

void GitController::pull(vcs::PullStrategy strategy) {
    if (allow(vcs::GitOperationKind::Pull))
        m_service->pull(strategy);
}

void GitController::push(bool forceWithLease, bool confirmed) {
    if (!allow(vcs::GitOperationKind::Push))
        return;
    m_service->push(vcs::PushOptions{.forceWithLease = forceWithLease, .confirmed = confirmed});
}

void GitController::cherryPick(const QString& commitHash) {
    if (allow(vcs::GitOperationKind::CherryPick))
        m_service->cherryPick(commitHash.trimmed().toStdString());
}

void GitController::revertCommit(const QString& commitHash) {
    if (allow(vcs::GitOperationKind::Revert))
        m_service->revertCommit(commitHash.trimmed().toStdString());
}

void GitController::resetBranch(const QString& commitHash, const QString& mode) {
    if (allow(vcs::GitOperationKind::Reset))
        m_service->resetBranch(commitHash.trimmed().toStdString(), mode.trimmed().toStdString());
}

void GitController::createTag(const QString& tagName, const QString& commitHash) {
    if (allow(vcs::GitOperationKind::Tag))
        m_service->createTag(tagName.trimmed().toStdString(), commitHash.trimmed().toStdString());
}

void GitController::createBranchAt(const QString& name, const QString& startPoint) {
    if (allow(vcs::GitOperationKind::CreateBranch))
        m_service->createBranchAt(name.trimmed().toStdString(), startPoint.trimmed().toStdString());
}

void GitController::mergeBranch(const QString& branch) {
    if (allow(vcs::GitOperationKind::Merge))
        m_service->mergeBranch(branch.trimmed().toStdString());
}

void GitController::rebaseBranch(const QString& ontoBranch) {
    if (allow(vcs::GitOperationKind::Rebase))
        m_service->rebaseBranch(ontoBranch.trimmed().toStdString());
}

void GitController::deleteBranch(const QString& branch, bool force) {
    if (allow(vcs::GitOperationKind::DeleteBranch))
        m_service->deleteBranch(branch.trimmed().toStdString(), force);
}

void GitController::renameBranch(const QString& oldName, const QString& newName) {
    if (allow(vcs::GitOperationKind::RenameBranch))
        m_service->renameBranch(oldName.trimmed().toStdString(), newName.trimmed().toStdString());
}

void GitController::stashPush(const QString& message, bool includeUntracked) {
    if (allow(vcs::GitOperationKind::Stash))
        m_service->stashPush(message.trimmed().toStdString(), includeUntracked);
}

void GitController::stashApply(int index, bool pop) {
    if (allow(vcs::GitOperationKind::Unstash))
        m_service->stashApply(index, pop);
}

void GitController::stashDrop(int index) {
    if (allow(vcs::GitOperationKind::DropStash))
        m_service->stashDrop(index);
}

void GitController::cancelOperation() { m_service->cancelOperation(); }

void GitController::refreshStatus() { m_service->refreshStatus(); }

void GitController::readHeadContent(const std::string& absolutePath, ContentCallback done) {
    m_service->readHeadContent(
        absolutePath, [this, gate = m_gate, done = std::move(done)](Result<std::optional<std::string>> content) {
            postTo(this, gate, [done, content] { done(content); }, Qt::QueuedConnection);
        });
}

void GitController::readIndexContent(const std::string& absolutePath, ContentCallback done) {
    m_service->readIndexContent(
        absolutePath, [this, gate = m_gate, done = std::move(done)](Result<std::optional<std::string>> content) {
            postTo(this, gate, [done, content] { done(content); }, Qt::QueuedConnection);
        });
}

void GitController::readLastCommitMessage(std::function<void(Result<std::string>)> done) {
    m_service->readLastCommitMessage([this, gate = m_gate, done = std::move(done)](Result<std::string> message) {
        postTo(this, gate, [done, message] { done(message); }, Qt::QueuedConnection);
    });
}

void GitController::readBranches() {
    m_service->readBranches([this, gate = m_gate](Result<std::vector<vcs::GitBranch>> branches) {
        if (!branches) {
            return;
        }
        postTo(
            this, gate,
            [this, list = std::move(*branches)] {
                m_branches = list;
                emit branchesChanged();
            },
            Qt::QueuedConnection);
    });
}

void GitController::readLog(std::size_t maxCount, const QString& branchFilter, const QString& searchFilter,
                            std::function<void(Result<std::vector<vcs::GitCommitEntry>>)> done) {
    m_service->readLog(maxCount, branchFilter.trimmed().toStdString(), searchFilter.trimmed().toStdString(),
                       [this, gate = m_gate, done = std::move(done)](Result<std::vector<vcs::GitCommitEntry>> commits) {
                           postTo(
                               this, gate, [done, result = std::move(commits)]() mutable { done(std::move(result)); },
                               Qt::QueuedConnection);
                       });
}

void GitController::readCommitFiles(const QString& commitHash,
                                    std::function<void(Result<std::vector<vcs::GitCommitFileChange>>)> done) {
    m_service->readCommitFiles(
        commitHash.trimmed().toStdString(),
        [this, gate = m_gate, done = std::move(done)](Result<std::vector<vcs::GitCommitFileChange>> files) {
            postTo(
                this, gate, [done, result = std::move(files)]() mutable { done(std::move(result)); },
                Qt::QueuedConnection);
        });
}

void GitController::readStashes(std::function<void(Result<std::vector<vcs::GitStashEntry>>)> done) {
    m_service->readStashes(
        [this, gate = m_gate, done = std::move(done)](Result<std::vector<vcs::GitStashEntry>> stashes) {
            postTo(
                this, gate, [done, result = std::move(stashes)]() mutable { done(std::move(result)); },
                Qt::QueuedConnection);
        });
}

void GitController::readBlame(const QString& relativeOrAbsolutePath,
                              std::function<void(Result<std::vector<vcs::GitBlameLine>>)> done) {
    m_service->readBlame(relativeOrAbsolutePath.toStdString(), [this, gate = m_gate, done = std::move(done)](
                                                                   Result<std::vector<vcs::GitBlameLine>> lines) {
        postTo(
            this, gate, [done, result = std::move(lines)]() mutable { done(std::move(result)); }, Qt::QueuedConnection);
    });
}

bool GitController::waitForIdle(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const bool idle = m_service->waitForIdle(std::chrono::milliseconds(50));
        QCoreApplication::processEvents();
        // A delivered result may queue more work (the branch list after a checkout): look again.
        if (idle && m_service->waitForIdle(std::chrono::milliseconds(0))) {
            QCoreApplication::processEvents();
            return true;
        }
    }
    return false;
}

void GitController::onStatus(std::shared_ptr<const vcs::GitStatusSnapshot> snapshot) {
    const bool branchMoved = !m_snapshot || m_snapshot->status.branch.head != snapshot->status.branch.head;
    m_snapshot = std::move(snapshot);
    emit statusChanged();
    if (branchMoved) {
        readBranches();
    }
}

void GitController::onStarted(vcs::GitOperationKind kind) {
    if (m_tasks != nullptr) {
        m_running.push_back(m_tasks->start(taskTitle(kind), false));
    }
    emit operationStarted(static_cast<int>(kind));
}

void GitController::onFinished(const vcs::GitOperationResult& result) {
    m_pendingOperation.reset();
    using K = vcs::GitOperationKind;
    if (!m_running.empty()) {
        m_running.front()->finish();
        m_running.erase(m_running.begin());
    }
    m_lastResult = result;
    if (result.kind == K::Commit && result.succeeded()) {
        remember(m_pendingMessage);
    }
    const bool pushAfter = result.kind == K::Commit && result.succeeded() && m_pushAfterCommit;
    if (result.kind == K::Commit)
        m_pushAfterCommit = false;
    if (result.kind == K::Checkout || result.kind == K::CreateBranch || result.kind == K::Fetch ||
        result.kind == K::Pull || result.kind == K::Push || result.kind == K::DeleteBranch ||
        result.kind == K::RenameBranch || result.kind == K::Merge || result.kind == K::Rebase ||
        result.kind == K::Reset || result.kind == K::Tag) {
        readBranches();
    }
    const QString message = result.error ? qs(result.error->message()) : QString();
    if (m_notifications != nullptr) {
        const QString title = qs(vcs::toString(result.kind));
        if (!result.succeeded()) {
            m_notifications->notify(
                Notification{tr("%1 failed").arg(title), message, NotificationKind::Error, true, {}});
        } else if (result.kind == K::Fetch || result.kind == K::Pull || result.kind == K::Push ||
                   result.kind == K::Commit) {
            m_notifications->notify(Notification{tr("%1 finished").arg(title), {}, NotificationKind::Info, false, {}});
        }
    }
    if (pushAfter)
        push(); // Recheck live trust after the commit and its hooks finish.
    emit operationFinished(static_cast<int>(result.kind), result.succeeded(), message);
}

void GitController::remember(const QString& message) {
    if (message.isEmpty()) {
        return;
    }
    m_history.removeAll(message);
    m_history.push_front(message);
    while (m_history.size() > static_cast<qsizetype>(kMessageHistory)) {
        m_history.pop_back();
    }
    emit historyChanged();
}

} // namespace frost::ui
