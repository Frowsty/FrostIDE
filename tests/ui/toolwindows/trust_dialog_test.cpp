// SPDX-License-Identifier: Apache-2.0
//
// Tests for the trust prompt (frost/ui/toolwindows/trust_dialog.hpp): Trust calls trust() for each
// listed folder, a failure keeps the dialog open with the error, Cancel cancels the waiting build,
// and TrustPrompter opens one dialog per NeedsTrust and closes it when the build moves on.
// Offscreen: the dialogs are opened on the offscreen platform, never on a screen.
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "toolwindows/toolwindow_test_util.hpp"
#include <QListWidget>
#include <QPushButton>
#include <QWidget>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace frost::ui {
namespace {

using build::BuildState;
using test::FakeBuildBackend;
using test::runPostedEvents;

#ifdef _WIN32
constexpr const char* kApp = R"(C:\src\SdkApp)";
constexpr const char* kShared = R"(C:\src\shared)";
#else
constexpr const char* kApp = "/src/SdkApp";
constexpr const char* kShared = "/src/shared";
#endif

struct Rig {
    FakeBuildBackend backend;
    BuildController controller;
    Rig() {
        controller.setBackend(&backend);
        EXPECT_TRUE(controller.openSolution(test::loadSdk()).has_value());
    }
    // The service waiting for the user's answer about these folders.
    void needTrust(std::vector<std::string> roots) {
        backend.currentState = BuildState::NeedsTrust;
        backend.untrusted = roots;
        const build::BuildServiceCallbacks callbacks = controller.serviceCallbacks();
        callbacks.onStateChanged(BuildState::NeedsTrust);
        callbacks.onNeedsTrust(roots);
        runPostedEvents();
    }
};

TEST(TrustDialog, TrustCallsTrustForEachRootAndCloses) {
    Rig rig;
    rig.needTrust({kApp, kShared});
    TrustDialog dialog(rig.controller, {QString::fromUtf8(kApp), QString::fromUtf8(kShared)});
    EXPECT_EQ(dialog.windowTitle(), QStringLiteral("Trust and Open Project?"));
    const auto* list = dialog.findChild<QListWidget*>(QStringLiteral("trustRoots"));
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->count(), 2);
    EXPECT_EQ(list->item(1)->text(), QString::fromUtf8(kShared));

    dialog.trustButton()->click();
    EXPECT_EQ(rig.backend.trusted, (std::vector<std::string>{kApp, kShared}));
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
    EXPECT_EQ(rig.backend.cancels, 0);
}

TEST(TrustDialog, AFailedTrustStaysOpenWithTheError) {
    Rig rig;
    rig.needTrust({kApp, kShared});
    rig.backend.failTrustOn = kShared;
    TrustDialog dialog(rig.controller, {QString::fromUtf8(kApp), QString::fromUtf8(kShared)});
    dialog.trustButton()->click();
    EXPECT_EQ(rig.backend.trusted, std::vector<std::string>{kApp});
    EXPECT_TRUE(dialog.errorText().contains(QStringLiteral("read-only")));
    EXPECT_EQ(dialog.roots(), QStringList{QString::fromUtf8(kShared)}); // What is still untrusted.
    EXPECT_NE(dialog.result(), QDialog::Accepted);

    rig.backend.failTrustOn.clear();
    dialog.trustButton()->click(); // Try again.
    EXPECT_EQ(rig.backend.trusted, (std::vector<std::string>{kApp, kShared}));
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
}

TEST(TrustDialog, CancelCancelsTheWaitingBuild) {
    Rig rig;
    rig.needTrust({kApp});
    TrustDialog dialog(rig.controller, {QString::fromUtf8(kApp)});
    dialog.cancelButton()->click();
    EXPECT_TRUE(rig.backend.trusted.empty());
    EXPECT_EQ(rig.backend.cancels, 1);
    EXPECT_EQ(dialog.result(), QDialog::Rejected);
}

TEST(TrustPrompter, OpensOneDialogPerPromptAndClosesWhenTheBuildMovesOn) {
    Rig rig;
    QWidget window;
    TrustPrompter prompter(rig.controller, &window);
    EXPECT_EQ(prompter.dialog(), nullptr);

    rig.needTrust({kApp});
    TrustDialog* dialog = prompter.dialog();
    ASSERT_NE(dialog, nullptr);
    EXPECT_EQ(dialog->roots(), QStringList{QString::fromUtf8(kApp)});
    // The service asks again (after a trust that left some): the same dialog shows the new list.
    rig.needTrust({kShared});
    EXPECT_EQ(prompter.dialog(), dialog);
    EXPECT_EQ(dialog->roots(), QStringList{QString::fromUtf8(kShared)});

    // Trusted elsewhere (or cancelled from the toolbar): the dialog goes away without cancelling.
    rig.backend.currentState = BuildState::Running;
    rig.controller.serviceCallbacks().onStateChanged(BuildState::Running);
    runPostedEvents();
    EXPECT_EQ(prompter.dialog(), nullptr); // Closed and deleted.
    EXPECT_EQ(rig.backend.cancels, 0);

    // A new prompt opens a new dialog, whose Trust answers the service.
    rig.backend.currentState = BuildState::Idle;
    rig.controller.serviceCallbacks().onStateChanged(BuildState::Idle);
    rig.needTrust({kApp});
    ASSERT_NE(prompter.dialog(), nullptr);
    prompter.dialog()->trustButton()->click();
    runPostedEvents();
    EXPECT_EQ(rig.backend.trusted, std::vector<std::string>{kApp});
    EXPECT_EQ(prompter.dialog(), nullptr);
}

} // namespace
} // namespace frost::ui
