// SPDX-License-Identifier: Apache-2.0
// Checks scene merge revalidation and cancellation when the dialog owner is destroyed.
#include "scene_merge_work.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QPushButton>
#include <QTimer>
#include <gtest/gtest.h>

#include <thread>
namespace frost::app {
namespace {
TEST(SceneMergeDialog, ChangedChoiceMustBeRevalidatedBeforeApply) {
    SceneMergeSnapshot source;
    source.request.images.path = "Assets/Scene.fscene";
    source.request.images.ours = R"({"Entities":[]})";
    source.report.result = source.request.images.ours;
    source.report.valid = true;
    source.report.unresolved = 1;
    source.report.conflicts.push_back({"/Future", "value", "", rpc::Json(1), rpc::Json(2), rpc::Json(3)});
    QWidget owner;
    int phase = 0, refreshes = 0, applies = 0;
    QTimer drive;
    drive.setInterval(1);
    QObject::connect(&drive, &QTimer::timeout, &owner, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<QDialog*>(widget);
            if (!dialog || dialog->objectName() != QStringLiteral("semanticSceneMergeDialog"))
                continue;
            auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("sceneMergeApply"));
            if (phase == 0) {
                ASSERT_FALSE(apply->isEnabled());
                auto* choice = dialog->findChild<QComboBox*>();
                ASSERT_NE(choice, nullptr);
                choice->setCurrentIndex(2);
                EXPECT_FALSE(apply->isEnabled());
                phase = 1;
                dialog->findChild<QPushButton*>(QStringLiteral("sceneMergeValidate"))->click();
            } else if (phase == 1 && apply->isEnabled()) {
                phase = 2;
                apply->click();
            }
        }
    });
    // Fail closed rather than hang if a UI regression prevents the intended path.
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &owner, [&] {
        for (auto* w : QApplication::topLevelWidgets())
            if (auto* d = qobject_cast<QDialog*>(w))
                d->reject();
    });
    watchdog.start(5000);
    drive.start();
    auto merged = showSceneMerge(
        &owner, std::move(source),
        [&](SceneMergeSnapshot snapshot, std::stop_token) -> Result<SceneMergeSnapshot> {
            ++refreshes;
            EXPECT_EQ(snapshot.request.choices.size(), 1U);
            if (snapshot.request.choices.size() != 1)
                return fail(ErrorCode::Corrupt, "missing choice");
            EXPECT_EQ(snapshot.request.choices[0].side, "theirs");
            snapshot.report.conflicts[0].choice = "theirs";
            snapshot.report.unresolved = 0;
            snapshot.report.clean = true;
            return snapshot;
        },
        [&](const SceneMergeSnapshot& snapshot) -> Result<void> {
            ++applies;
            EXPECT_TRUE(snapshot.report.clean);
            return {};
        });
    ASSERT_TRUE(merged);
    EXPECT_TRUE(*merged);
    EXPECT_EQ(phase, 2);
    EXPECT_EQ(refreshes, 1);
    EXPECT_EQ(applies, 1);
}
TEST(SceneMergeDialog, OwnerDestructionCancelsWorkerAndWaitsForExit) {
    auto owner = std::make_unique<QWidget>();
    bool exited = false;
    QTimer::singleShot(0, [&] { owner.reset(); });
    auto result = sceneMergeInBackground(owner.get(), [&](std::stop_token stop) -> Result<SceneMergeSnapshot> {
        while (!stop.stop_requested())
            std::this_thread::yield();
        exited = true;
        return fail(ErrorCode::Cancelled, "cancelled");
    });
    EXPECT_FALSE(result);
    EXPECT_TRUE(exited);
}
} // namespace
} // namespace frost::app
