// SPDX-License-Identifier: Apache-2.0
//
// Background tasks (PLAN.md row 219, ADR 0035): long operations report progress to the status
// bar's processes indicator and can be cancelled from it. A BackgroundTask handle is thread-safe,
// so work on scheduler threads reports directly; the manager and its indicator live on the GUI
// thread and coalesce updates into one refresh per event-loop turn.
#pragma once

#include "frost/core/mutex.hpp"

#include <QObject>
#include <QString>
#include <QWidget>

#include <atomic>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

class QLabel;
class QProgressBar;
class QToolButton;

namespace frost::ui {

namespace detail {
struct TaskHub;
}

/*
 * One running task, shared by the code doing the work and the manager. Every member is
 * thread-safe. Cancelling requests a stop on stopToken(); the work notices, cleans up and calls
 * finish(), and only then the task leaves the indicator (it shows "Cancelling..." until then).
 * Dropping the last handle without finish() also ends it at the next refresh.
 */
class BackgroundTask {
public:
    struct State {
        QString title;
        QString detail;
        // In [0, 1]; nullopt: indeterminate.
        std::optional<double> fraction;
        bool cancellable = true;
        bool cancelling = false;
        bool finished = false;
    };

    BackgroundTask(QString title, bool cancellable, std::weak_ptr<detail::TaskHub> hub);
    BackgroundTask(const BackgroundTask&) = delete ("a task has identity");
    BackgroundTask& operator=(const BackgroundTask&) = delete ("a task has identity");
    ~BackgroundTask();

    // Progress in [0, 1] (clamped), or nullopt for indeterminate.
    void setProgress(std::optional<double> fraction);
    // The secondary text ("Indexing 12 of 340 files").
    void setDetail(QString detail);
    // Ends the task; later calls do nothing.
    void finish();
    // Requests a stop (ignored when the task is not cancellable or already finished).
    void cancel();

    [[nodiscard]] std::stop_token stopToken() const noexcept { return m_stop.get_token(); }
    [[nodiscard]] bool cancelRequested() const noexcept { return m_stop.stop_requested(); }
    [[nodiscard]] State state() const;

private:
    void changed();

    mutable Mutex m_mutex;
    State m_state FROST_GUARDED_BY(m_mutex);
    std::stop_source m_stop;
    std::weak_ptr<detail::TaskHub> m_hub;
};

/*
 * Starts tasks and keeps the running ones. GUI thread only (except through task handles).
 */
class BackgroundTaskManager final : public QObject {
    Q_OBJECT

public:
    explicit BackgroundTaskManager(QObject* parent = nullptr);
    ~BackgroundTaskManager() override;

    // Starts a task titled `title` (already translated) and returns its handle.
    [[nodiscard]] std::shared_ptr<BackgroundTask> start(const QString& title, bool cancellable = true);
    // The running tasks, oldest first (refreshed state: finished tasks are dropped first).
    [[nodiscard]] std::vector<std::shared_ptr<BackgroundTask>> tasks();
    void cancelAll();
    // Applies queued updates now (normally a queued call does it); emits changed().
    void refresh();

signals:
    void changed();

private:
    std::shared_ptr<detail::TaskHub> m_hub;
    std::vector<std::weak_ptr<BackgroundTask>> m_tasks;
};

/*
 * The status bar's processes indicator: hidden while nothing runs; otherwise the oldest task's
 * title (with "+N more"), a progress bar and a cancel button for it. GUI thread only; the manager
 * must outlive it.
 */
class ProcessesIndicator final : public QWidget {
    Q_OBJECT

public:
    explicit ProcessesIndicator(BackgroundTaskManager& manager, QWidget* parent = nullptr);

    [[nodiscard]] QLabel* label() const noexcept { return m_label; }
    [[nodiscard]] QProgressBar* progressBar() const noexcept { return m_progress; }
    [[nodiscard]] QToolButton* cancelButton() const noexcept { return m_cancel; }

private:
    void update();

    BackgroundTaskManager& m_manager;
    QLabel* m_label;
    QProgressBar* m_progress;
    QToolButton* m_cancel;
    std::weak_ptr<BackgroundTask> m_shown;
};

} // namespace frost::ui
