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
    }
    return QCoreApplication::translate("GitController", "Running git");
}

} // namespace

GitController::GitController(vcs::GitRunner runner, io::IFileSystem& fs, vcs::RepositoryInfo info,
                             vcs::GitServiceOptions options, QObject* parent)
    : QObject(parent) {
    // The callbacks run on the service's workers; each posts to this object, and Qt drops the
    // posts once it is gone (the destructor ends the workers first, so `this` is valid when they run).
    vcs::GitServiceCallbacks callbacks;
    callbacks.onStatusChanged = [this](std::shared_ptr<const vcs::GitStatusSnapshot> snapshot) {
        QMetaObject::invokeMethod(this, [this, snapshot] { onStatus(snapshot); }, Qt::QueuedConnection);
    };
    callbacks.onStatusFailed = [this](const Error& error) {
        QMetaObject::invokeMethod(
            this, [this, message = qs(error.message())] { emit statusFailed(message); }, Qt::QueuedConnection);
    };
    callbacks.onOutput = [this](vcs::GitStream stream, std::string_view line) {
        QMetaObject::invokeMethod(
            this, [this, stream, text = qs(line)] { emit outputLine(static_cast<int>(stream), text); },
            Qt::QueuedConnection);
    };
    callbacks.onOperationStarted = [this](vcs::GitOperationKind kind) {
        QMetaObject::invokeMethod(this, [this, kind] { onStarted(kind); }, Qt::QueuedConnection);
    };
    callbacks.onOperationFinished = [this](const vcs::GitOperationResult& result) {
        QMetaObject::invokeMethod(this, [this, result] { onFinished(result); }, Qt::QueuedConnection);
    };
    m_service = vcs::GitService::create(std::move(runner), fs, std::move(info), options, std::move(callbacks));
    m_service->readRecentCommitMessages(kMessageHistory, [this](Result<std::vector<std::string>> messages) {
        if (!messages) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
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
    // Cancels the running operation, drops queued work and joins the workers before the QObject goes.
    m_service.reset();
    for (const std::shared_ptr<BackgroundTask>& task : m_running) {
        task->finish();
    }
}

QString GitController::currentBranch() const { return m_snapshot ? qs(m_snapshot->status.branch.head) : QString(); }

bool GitController::detached() const { return m_snapshot && m_snapshot->status.branch.detached(); }

bool GitController::busy() const { return m_service->operationBusy(); }

void GitController::commit(std::vector<std::string> paths, const QString& message, bool amend, bool pushAfter) {
    m_pendingMessage = message.trimmed();
    vcs::CommitRequest request;
    request.paths = std::move(paths);
    request.message = message.toStdString();
    request.amend = amend;
    m_service->commit(std::move(request), pushAfter);
}

void GitController::rollback(std::vector<std::string> paths) { m_service->rollback(std::move(paths)); }

void GitController::checkout(const QString& branch) { m_service->checkout(branch.toStdString()); }

void GitController::createBranch(const QString& name) { m_service->createBranch(name.trimmed().toStdString()); }

void GitController::fetch() { m_service->fetch(); }

void GitController::pull(vcs::PullStrategy strategy) { m_service->pull(strategy); }

void GitController::push(bool forceWithLease, bool confirmed) {
    m_service->push(vcs::PushOptions{.forceWithLease = forceWithLease, .confirmed = confirmed});
}

void GitController::cancelOperation() { m_service->cancelOperation(); }

void GitController::refreshStatus() { m_service->refreshStatus(); }

void GitController::readHeadContent(const std::string& absolutePath, ContentCallback done) {
    m_service->readHeadContent(
        absolutePath, [this, done = std::move(done)](Result<std::optional<std::string>> content) {
            QMetaObject::invokeMethod(this, [done, content] { done(content); }, Qt::QueuedConnection);
        });
}

void GitController::readIndexContent(const std::string& absolutePath, ContentCallback done) {
    m_service->readIndexContent(
        absolutePath, [this, done = std::move(done)](Result<std::optional<std::string>> content) {
            QMetaObject::invokeMethod(this, [done, content] { done(content); }, Qt::QueuedConnection);
        });
}

void GitController::readLastCommitMessage(std::function<void(Result<std::string>)> done) {
    m_service->readLastCommitMessage([this, done = std::move(done)](Result<std::string> message) {
        QMetaObject::invokeMethod(this, [done, message] { done(message); }, Qt::QueuedConnection);
    });
}

void GitController::readBranches() {
    m_service->readBranches([this](Result<std::vector<vcs::GitBranch>> branches) {
        if (!branches) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, list = std::move(*branches)] {
                m_branches = list;
                emit branchesChanged();
            },
            Qt::QueuedConnection);
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
    using K = vcs::GitOperationKind;
    if (!m_running.empty()) {
        m_running.front()->finish();
        m_running.erase(m_running.begin());
    }
    m_lastResult = result;
    if (result.kind == K::Commit && result.succeeded()) {
        remember(m_pendingMessage);
    }
    if (result.kind == K::Checkout || result.kind == K::CreateBranch || result.kind == K::Fetch ||
        result.kind == K::Pull || result.kind == K::Push) {
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
