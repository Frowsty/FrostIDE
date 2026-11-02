// SPDX-License-Identifier: Apache-2.0
//
// Functional tests of the notifications (frost/ui/shell/notifications.hpp) and the background
// tasks (frost/ui/shell/background_tasks.hpp), offscreen: a notification shows a balloon and a log
// entry, balloons expire unless sticky, a posted notification arrives from another thread, and a
// background task's progress reaches the status-bar indicator and its cancel button stops it.

#include "frost/ui/shell/background_tasks.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/shell/status_bar.hpp"

#include "shell/shell_rig.hpp"
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QToolButton>
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace {

using frost::ui::BackgroundTaskManager;
using frost::ui::Notification;
using frost::ui::NotificationCenter;
using frost::ui::NotificationKind;

Notification info(const char* title) {
    return Notification{QString::fromUtf8(title), QStringLiteral("details"), NotificationKind::Info, std::nullopt, {}};
}

TEST(Notifications, NotifyShowsABalloonAndLogsIt) {
    QWidget host;
    host.resize(800, 600);
    host.show();
    NotificationCenter center;
    center.setBalloonHost(&host);
    int changes = 0;
    QObject::connect(&center, &NotificationCenter::changed, [&changes] { ++changes; });

    center.notify(info("Indexing finished"));
    ASSERT_EQ(center.log().size(), 1U);
    EXPECT_EQ(center.log().front().notification.title, QStringLiteral("Indexing finished"));
    EXPECT_EQ(center.unreadCount(), 1);
    EXPECT_GE(changes, 1);
    ASSERT_EQ(center.balloons().size(), 1U);
    frost::ui::NotificationBalloon* balloon = center.balloons().front();
    EXPECT_TRUE(balloon->isVisible());
    EXPECT_EQ(balloon->parentWidget(), &host); // A child widget, never a top-level window.
    EXPECT_FALSE(balloon->isWindow());
    EXPECT_EQ(balloon->findChild<QLabel*>(QStringLiteral("balloonTitle"))->text(), QStringLiteral("Indexing finished"));
    // Inside the host's bottom-right corner.
    EXPECT_TRUE(host.rect().contains(balloon->geometry()));

    // The close button dismisses the balloon; the log keeps the entry.
    balloon->findChild<QToolButton*>(QStringLiteral("balloonClose"))->click();
    frost::ui::test::waitFor([&center] { return center.balloons().empty(); });
    EXPECT_TRUE(center.balloons().empty());
    EXPECT_EQ(center.log().size(), 1U);
    center.markAllRead();
    EXPECT_EQ(center.unreadCount(), 0);
}

TEST(Notifications, BalloonsExpireUnlessSticky) {
    QWidget host;
    host.resize(800, 600);
    NotificationCenter center;
    center.setBalloonHost(&host);
    center.notify(info("Saved"));
    center.notify(Notification{QStringLiteral("Build failed"), {}, NotificationKind::Error, std::nullopt, {}});
    ASSERT_EQ(center.balloons().size(), 2U);
    center.expireBalloons();
    ASSERT_EQ(center.balloons().size(), 1U); // Errors are sticky by default.
    EXPECT_TRUE(center.balloons().front()->sticky());

    // A short timeout removes a non-sticky balloon on its own.
    center.setBalloonTimeout(std::chrono::milliseconds(20));
    center.notify(info("Quick"));
    EXPECT_EQ(center.balloons().size(), 2U);
    EXPECT_TRUE(frost::ui::test::waitFor([&center] { return center.balloons().size() == 1U; }));
}

TEST(Notifications, ActionsRunAndDismiss) {
    QWidget host;
    host.resize(800, 600);
    NotificationCenter center;
    center.setBalloonHost(&host);
    int runs = 0;
    Notification withAction = info("Theme missing");
    withAction.actions.push_back({QStringLiteral("Retry"), [&runs] { ++runs; }});
    center.notify(std::move(withAction));
    ASSERT_EQ(center.balloons().size(), 1U);
    auto* button = center.balloons().front()->findChild<QPushButton*>(QStringLiteral("balloonAction"));
    ASSERT_NE(button, nullptr);
    button->click();
    EXPECT_EQ(runs, 1);
    EXPECT_TRUE(frost::ui::test::waitFor([&center] { return center.balloons().empty(); }));
}

TEST(Notifications, PostArrivesFromAnotherThread) {
    NotificationCenter center;
    std::thread worker([&center] { center.post(info("From a worker")); });
    worker.join();
    EXPECT_TRUE(center.log().empty()); // Queued, not run on the worker.
    EXPECT_TRUE(frost::ui::test::waitFor([&center] { return center.log().size() == 1U; }));
}

TEST(Notifications, LogViewListsEntriesAndClears) {
    NotificationCenter center;
    center.notify(info("First"));
    center.notify(info("Second"));
    frost::ui::NotificationLogView view(center);
    EXPECT_EQ(view.list()->count(), 2);
    view.show();
    QApplication::processEvents();
    EXPECT_EQ(center.unreadCount(), 0); // Showing the log reads it.
    center.clear();
    EXPECT_EQ(view.list()->count(), 0);
}

TEST(BackgroundTasks, ProgressReachesTheIndicatorAndCancelStopsTheTask) {
    BackgroundTaskManager manager;
    frost::ui::ProcessesIndicator indicator(manager);
    EXPECT_TRUE(indicator.isHidden()); // Idle.

    std::shared_ptr<frost::ui::BackgroundTask> task = manager.start(QStringLiteral("Indexing"));
    // Progress from a worker thread is delivered on the GUI thread.
    std::thread worker([task] {
        task->setDetail(QStringLiteral("main.cpp"));
        task->setProgress(0.25);
    });
    worker.join();
    EXPECT_TRUE(frost::ui::test::waitFor([&indicator] { return indicator.progressBar()->value() == 25; }));
    EXPECT_FALSE(indicator.isHidden());
    EXPECT_EQ(indicator.label()->text(), QStringLiteral("Indexing: main.cpp"));
    EXPECT_EQ(indicator.progressBar()->maximum(), 100);

    // Indeterminate progress.
    task->setProgress(std::nullopt);
    EXPECT_TRUE(frost::ui::test::waitFor([&indicator] { return indicator.progressBar()->maximum() == 0; }));

    // Cancel from the status bar: the task sees the stop request and says so until it finishes.
    ASSERT_FALSE(indicator.cancelButton()->isHidden());
    indicator.cancelButton()->click();
    EXPECT_TRUE(task->cancelRequested());
    EXPECT_TRUE(task->stopToken().stop_requested());
    EXPECT_TRUE(frost::ui::test::waitFor(
        [&indicator] { return indicator.label()->text().startsWith(QStringLiteral("Cancelling Indexing")); }));
    EXPECT_FALSE(indicator.cancelButton()->isEnabled());
    task->finish();
    EXPECT_TRUE(frost::ui::test::waitFor([&indicator] { return indicator.isHidden(); }));
    EXPECT_TRUE(manager.tasks().empty());
}

TEST(BackgroundTasks, SeveralTasksAndDroppedHandles) {
    BackgroundTaskManager manager;
    frost::ui::ProcessesIndicator indicator(manager);
    auto first = manager.start(QStringLiteral("Building"), false);
    auto second = manager.start(QStringLiteral("Indexing"));
    manager.refresh();
    EXPECT_EQ(manager.tasks().size(), 2U);
    EXPECT_TRUE(indicator.label()->text().contains(QStringLiteral("(+1 more)")));
    EXPECT_TRUE(indicator.cancelButton()->isHidden()); // The first task cannot be cancelled.
    first.reset();                                     // Dropped without finish(): it ends.
    EXPECT_TRUE(frost::ui::test::waitFor([&manager] { return manager.tasks().size() == 1U; }));
    manager.cancelAll();
    EXPECT_TRUE(second->cancelRequested());
}

TEST(BackgroundTasks, ShellStatusBarHostsTheIndicator) {
    frost::ui::test::ShellRig rig;
    frost::ui::ShellWindow window(rig.environment());
    auto task = window.backgroundTasks().start(QStringLiteral("Loading solution"));
    task->setProgress(0.5);
    EXPECT_TRUE(frost::ui::test::waitFor(
        [&window] { return window.statusWidgets().processes()->progressBar()->value() == 50; }));
    task->finish();
}

} // namespace
