// SPDX-License-Identifier: Apache-2.0
//
// BackgroundTask, BackgroundTaskManager and ProcessesIndicator (frost/ui/shell/background_tasks.hpp).
/*
 * Cross-thread updates go through a TaskHub shared by the manager and every task (weakly). A task
 * that changed asks the hub for a refresh; the hub posts one queued refresh() to the manager per
 * event-loop turn (an atomic flag coalesces), under its mutex, and the manager's destructor clears
 * the hub's receiver under the same mutex, so no post can target a destroyed manager (Qt drops a
 * destroyed object's pending posted events).
 */

#include "frost/ui/shell/background_tasks.hpp"

#include "frost/ui/icon_ids.hpp"

#include "shell_util.hpp"
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QProgressBar>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <utility>

namespace frost::ui {

namespace detail {

struct TaskHub {
    Mutex mutex;
    BackgroundTaskManager* receiver FROST_GUARDED_BY(mutex) = nullptr;
    std::atomic<bool> queued{false};

    void requestRefresh() {
        if (queued.exchange(true)) {
            return;
        }
        LockGuard lock(mutex);
        if (receiver != nullptr) {
            QMetaObject::invokeMethod(receiver, &BackgroundTaskManager::refresh, Qt::QueuedConnection);
        }
    }
};

} // namespace detail

using shell_detail::text;

BackgroundTask::BackgroundTask(QString title, bool cancellable, std::weak_ptr<detail::TaskHub> hub)
    : m_hub(std::move(hub)) {
    LockGuard lock(m_mutex);
    m_state.title = std::move(title);
    m_state.cancellable = cancellable;
}

BackgroundTask::~BackgroundTask() {
    // The manager holds weak references; a refresh drops the expired one.
    changed();
}

void BackgroundTask::setProgress(std::optional<double> fraction) {
    {
        LockGuard lock(m_mutex);
        if (fraction && std::isfinite(*fraction)) {
            m_state.fraction = std::clamp(*fraction, 0.0, 1.0);
        } else {
            m_state.fraction.reset();
        }
    }
    changed();
}

void BackgroundTask::setDetail(QString detail) {
    {
        LockGuard lock(m_mutex);
        m_state.detail = std::move(detail);
    }
    changed();
}

void BackgroundTask::finish() {
    {
        LockGuard lock(m_mutex);
        if (m_state.finished) {
            return;
        }
        m_state.finished = true;
    }
    changed();
}

void BackgroundTask::cancel() {
    {
        LockGuard lock(m_mutex);
        if (!m_state.cancellable || m_state.finished || m_state.cancelling) {
            return;
        }
        m_state.cancelling = true;
    }
    m_stop.request_stop();
    changed();
}

BackgroundTask::State BackgroundTask::state() const {
    LockGuard lock(m_mutex);
    return m_state;
}

void BackgroundTask::changed() {
    if (const std::shared_ptr<detail::TaskHub> hub = m_hub.lock(); hub != nullptr) {
        hub->requestRefresh();
    }
}

BackgroundTaskManager::BackgroundTaskManager(QObject* parent)
    : QObject(parent), m_hub(std::make_shared<detail::TaskHub>()) {
    setObjectName(QStringLiteral("backgroundTasks"));
    LockGuard lock(m_hub->mutex);
    m_hub->receiver = this;
}

BackgroundTaskManager::~BackgroundTaskManager() {
    LockGuard lock(m_hub->mutex);
    m_hub->receiver = nullptr;
}

std::shared_ptr<BackgroundTask> BackgroundTaskManager::start(const QString& title, bool cancellable) {
    auto task = std::make_shared<BackgroundTask>(title, cancellable, m_hub);
    m_tasks.push_back(task);
    emit changed();
    return task;
}

std::vector<std::shared_ptr<BackgroundTask>> BackgroundTaskManager::tasks() {
    std::vector<std::shared_ptr<BackgroundTask>> running;
    std::vector<std::weak_ptr<BackgroundTask>> kept;
    for (const std::weak_ptr<BackgroundTask>& weak : m_tasks) {
        if (std::shared_ptr<BackgroundTask> task = weak.lock(); task != nullptr && !task->state().finished) {
            running.push_back(task);
            kept.push_back(weak);
        }
    }
    m_tasks = std::move(kept);
    return running;
}

void BackgroundTaskManager::cancelAll() {
    for (const std::shared_ptr<BackgroundTask>& task : tasks()) {
        task->cancel();
    }
}

void BackgroundTaskManager::refresh() {
    m_hub->queued.store(false);
    static_cast<void>(tasks());
    emit changed();
}

ProcessesIndicator::ProcessesIndicator(BackgroundTaskManager& manager, QWidget* parent)
    : QWidget(parent), m_manager(manager), m_label(new QLabel(this)), m_progress(new QProgressBar(this)),
      m_cancel(new QToolButton(this)) {
    setObjectName(QStringLiteral("processesIndicator"));
    m_label->setObjectName(QStringLiteral("processesLabel"));
    m_progress->setObjectName(QStringLiteral("processesProgress"));
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->setFixedWidth(120);
    m_progress->setMaximumHeight(10);
    m_cancel->setObjectName(QStringLiteral("processesCancel"));
    m_cancel->setAutoRaise(true);
    m_cancel->setToolTip(text("Cancel"));
    const QIcon stop = shell_detail::themedIcon(icons::kRunStop);
    if (stop.isNull()) {
        m_cancel->setText(QStringLiteral("×"));
    } else {
        m_cancel->setIcon(stop);
    }
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(6);
    layout->addWidget(m_label);
    layout->addWidget(m_progress);
    layout->addWidget(m_cancel);
    connect(m_cancel, &QToolButton::clicked, this, [this] {
        if (const std::shared_ptr<BackgroundTask> task = m_shown.lock(); task != nullptr) {
            task->cancel();
        }
    });
    connect(&m_manager, &BackgroundTaskManager::changed, this, &ProcessesIndicator::update);
    update();
}

void ProcessesIndicator::update() {
    const std::vector<std::shared_ptr<BackgroundTask>> running = m_manager.tasks();
    if (running.empty()) {
        m_shown.reset();
        hide();
        return;
    }
    const std::shared_ptr<BackgroundTask>& first = running.front();
    m_shown = first;
    const BackgroundTask::State state = first->state();
    QString label = state.cancelling ? text("Cancelling %1...").arg(state.title) : state.title;
    if (!state.detail.isEmpty()) {
        label += QStringLiteral(": ") + state.detail;
    }
    if (running.size() > 1) {
        label += QStringLiteral(" ") + text("(+%1 more)").arg(static_cast<qsizetype>(running.size() - 1));
    }
    m_label->setText(label);
    if (state.fraction) {
        m_progress->setRange(0, 100);
        m_progress->setValue(static_cast<int>(std::lround(*state.fraction * 100.0)));
    } else {
        m_progress->setRange(0, 0);
    }
    m_cancel->setVisible(state.cancellable);
    m_cancel->setEnabled(!state.cancelling);
    show();
}

} // namespace frost::ui
