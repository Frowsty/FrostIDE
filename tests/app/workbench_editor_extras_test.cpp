// SPDX-License-Identifier: Apache-2.0
// UX-16: restore only genuine durable revisions to the original buffer, preserving Save and Undo.
#include "../../src/app/src/project_properties_dialog.hpp"
#include "workbench_rig.hpp"
#include <QApplication>
#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <gtest/gtest.h>
namespace {
using namespace frost;
using app::test::WorkbenchRig;
using app::test::WorkDir;
QDialog* historyDialog() {
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->objectName() == QStringLiteral("localHistoryDialog"))
            return qobject_cast<QDialog*>(widget);
    return nullptr;
}
TEST(WorkbenchEditorExtras, StoredRevisionRestoresWithUndoAndDoesNotSilentlySave) {
    WorkDir dir("history-editor-restore");
    dir.write("file.txt", "one\n");
    WorkbenchRig rig(dir);
    ASSERT_TRUE(rig.workbench->openPath(dir.join("file.txt")));
    auto* view = rig.view();
    ASSERT_NE(view, nullptr);
    ASSERT_TRUE(view->editor().typeText("first "));
    ASSERT_TRUE(rig.workbench->save(*view));
    view->editor().goToLine(1, 1);
    ASSERT_TRUE(view->editor().typeText("second "));
    ASSERT_TRUE(rig.workbench->save(*view));
    view->editor().goToLine(1, 1);
    ASSERT_TRUE(view->editor().typeText("dirty "));
    bool selected = false;
    bool restored = false;
    bool timeout = false;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        auto* dialog = historyDialog();
        if (!dialog)
            return;
        auto* list = dialog->findChild<QListWidget*>(QStringLiteral("localHistoryRevisionList"));
        auto* revert = dialog->findChild<QPushButton*>(QStringLiteral("localHistoryRevertButton"));
        if (!list || !revert || list->count() < 2)
            return;
        if (!selected) {
            selected = true;
            list->setCurrentRow(1);
            return;
        }
        if (revert->isEnabled()) {
            restored = true;
            revert->click();
        }
    });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [&] {
        timeout = true;
        if (auto* dialog = historyDialog())
            dialog->reject();
    });
    poll.start();
    watchdog.start(5000);
    ASSERT_EQ(rig.window->invoke("vcs.showLocalHistory"), input::InvokeResult::Ran);
    poll.stop();
    watchdog.stop();
    ASSERT_FALSE(timeout);
    ASSERT_TRUE(restored);
    EXPECT_EQ(view->editor().editor().text().text(), "first one\n");
    EXPECT_EQ(dir.read("file.txt"), "second first one\n");
    EXPECT_TRUE(view->document().dirty());
    ASSERT_TRUE(view->editor().perform(ui::EditorAction::Undo));
    EXPECT_EQ(view->editor().editor().text().text(), "dirty second first one\n");
}
TEST(WorkbenchEditorExtras, HistoryWorkerStopsBeforeOwnerDestructionCompletes) {
    WorkDir dir("history-owner-close");
    const auto path = dir.join("history.db");
    {
        auto store = storage::LocalHistory::open(path);
        ASSERT_TRUE(store);
    }
    auto* owner = new QWidget;
    QTimer::singleShot(0, [owner] { delete owner; });
    app::showLocalHistoryDialog(owner, path, "missing-key", dir.join("file.txt"), "current", {});
    EXPECT_EQ(historyDialog(), nullptr);
}
} // namespace
