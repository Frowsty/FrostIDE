// SPDX-License-Identifier: Apache-2.0
//
// Tests for the trust prompt (frost/ui/toolwindows/trust_dialog.hpp): Trust calls trust() for each
// listed folder, a failure keeps the dialog open with the error, Cancel cancels the waiting build
// (only for a Build prompt: Lua, managed and Attach to Process prompts leave it alone), a session
// grant trusts nothing through the controller, and TrustPrompter opens one dialog per NeedsTrust
// and closes it when the build moves on.
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

TEST(TrustDialog, DecliningLuaTrustDoesNotCancelAnUnrelatedWaitingBuild) {
    Rig rig;
    rig.needTrust({kApp});
    const QStringList roots{QString::fromUtf8(kApp)};
    {
        TrustDialog lua(rig.controller, roots, nullptr, TrustPurpose::LuaLanguageServer);
        lua.cancelButton()->click();
        EXPECT_EQ(lua.result(), QDialog::Rejected);
        EXPECT_EQ(rig.backend.cancels, 0);
        EXPECT_EQ(rig.controller.state(), BuildState::NeedsTrust);
    }
    {
        TrustDialog lua(rig.controller, roots, nullptr, TrustPurpose::LuaLanguageServer);
        lua.show();
        lua.close();
        EXPECT_EQ(lua.result(), QDialog::Rejected);
        EXPECT_EQ(rig.backend.cancels, 0);
    }
    {
        TrustDialog lua(rig.controller, roots, nullptr, TrustPurpose::LuaLanguageServer);
        lua.show();
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(&lua, &escape);
        EXPECT_EQ(lua.result(), QDialog::Rejected);
        EXPECT_EQ(rig.backend.cancels, 0);
    }
    EXPECT_EQ(rig.controller.state(), BuildState::NeedsTrust);
    TrustDialog build(rig.controller, roots, nullptr, TrustPurpose::Build);
    build.cancelButton()->click();
    EXPECT_EQ(rig.backend.cancels, 1);
}

TEST(TrustDialog, DecliningManagedTrustDoesNotCancelAnUnrelatedWaitingBuild) {
    Rig rig;
    rig.needTrust({kApp});
    const QStringList roots{QString::fromUtf8(kApp)};
    {
        TrustDialog managed(rig.controller, roots, nullptr, TrustPurpose::ManagedLanguageServer);
        managed.cancelButton()->click();
        EXPECT_EQ(managed.result(), QDialog::Rejected);
        EXPECT_EQ(rig.backend.cancels, 0);
        EXPECT_EQ(rig.controller.state(), BuildState::NeedsTrust);
    }
    {
        TrustDialog managed(rig.controller, roots, nullptr, TrustPurpose::ManagedLanguageServer);
        managed.show();
        managed.close();
        EXPECT_EQ(managed.result(), QDialog::Rejected);
        EXPECT_EQ(rig.backend.cancels, 0);
    }
    {
        TrustDialog managed(rig.controller, roots, nullptr, TrustPurpose::ManagedLanguageServer);
        managed.show();
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(&managed, &escape);
        EXPECT_EQ(managed.result(), QDialog::Rejected);
        EXPECT_EQ(rig.backend.cancels, 0);
    }
    EXPECT_EQ(rig.controller.state(), BuildState::NeedsTrust);
    TrustDialog build(rig.controller, roots, nullptr, TrustPurpose::Build);
    build.cancelButton()->click();
    EXPECT_EQ(rig.backend.cancels, 1);
}

// R0 follow-up 1: the Attach to Process prompt has its own wording, and declining it (Cancel,
// closing, Escape) never cancels a build waiting in NeedsTrust.
TEST(TrustDialog, DecliningDebugAttachDoesNotCancelAWaitingBuild) {
    Rig rig;
    rig.needTrust({kApp});
    const QStringList roots{QString::fromUtf8(kApp)};
    {
        TrustDialog attach(rig.controller, roots, nullptr, TrustPurpose::DebugAttach);
        EXPECT_EQ(attach.windowTitle(), QStringLiteral("Attach Debugger?"));
        EXPECT_TRUE(attach.description().contains(QStringLiteral("runs no project code")))
            << attach.description().toStdString();
        EXPECT_TRUE(attach.description().contains(QStringLiteral("debugger full access")));
        EXPECT_EQ(attach.trustButton()->text(), QStringLiteral("Trust and Attach"));
        EXPECT_FALSE(attach.sessionOnly());
        attach.cancelButton()->click();
        EXPECT_EQ(attach.result(), QDialog::Rejected);
        EXPECT_EQ(rig.backend.cancels, 0);
        EXPECT_EQ(rig.controller.state(), BuildState::NeedsTrust);
    }
    {
        TrustDialog attach(rig.controller, roots, nullptr, TrustPurpose::DebugAttach);
        attach.show();
        attach.close();
        EXPECT_EQ(attach.result(), QDialog::Rejected);
        EXPECT_EQ(rig.backend.cancels, 0);
    }
    {
        TrustDialog attach(rig.controller, roots, nullptr, TrustPurpose::DebugAttach);
        attach.show();
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(&attach, &escape);
        EXPECT_EQ(attach.result(), QDialog::Rejected);
        EXPECT_EQ(rig.backend.cancels, 0);
    }
    EXPECT_EQ(rig.controller.state(), BuildState::NeedsTrust);
    EXPECT_TRUE(rig.backend.trusted.empty());
}

// With a session grant (a one-off attach, no project open) Trust calls the grant only: nothing is
// trusted through the controller, the wording says the permission is not saved, and a failing
// grant keeps the dialog open with its error.
TEST(TrustDialog, SessionGrantTrustsNothingThroughTheController) {
    Rig rig;
    rig.needTrust({kApp});
    TrustDialog attach(rig.controller, {QString::fromUtf8(kShared)}, nullptr, TrustPurpose::DebugAttach);
    std::vector<std::string> granted;
    bool refuse = true;
    attach.setSessionGrant([&](std::string_view root) -> frost::Result<void> {
        if (refuse)
            return frost::fail(frost::ErrorCode::InvalidArgument, "not a folder");
        granted.emplace_back(root);
        return {};
    });
    EXPECT_TRUE(attach.sessionOnly());
    EXPECT_TRUE(attach.description().contains(QStringLiteral("not saved"))) << attach.description().toStdString();
    EXPECT_EQ(attach.trustButton()->text(), QStringLiteral("Attach Once"));
    attach.trustButton()->click();
    EXPECT_TRUE(attach.errorText().contains(QStringLiteral("not a folder")));
    EXPECT_NE(attach.result(), QDialog::Accepted);
    refuse = false;
    attach.trustButton()->click();
    EXPECT_EQ(granted, std::vector<std::string>{kShared});
    EXPECT_EQ(attach.result(), QDialog::Accepted);
    EXPECT_TRUE(rig.backend.trusted.empty());
    EXPECT_EQ(rig.backend.cancels, 0);
    EXPECT_EQ(rig.controller.state(), BuildState::NeedsTrust);
}

TEST(TrustDialog, AcceptingLuaTrustStillTrustsTheProject) {
    Rig rig;
    rig.needTrust({kApp});
    TrustDialog lua(rig.controller, {QString::fromUtf8(kApp)}, nullptr, TrustPurpose::LuaLanguageServer);
    lua.trustButton()->click();
    EXPECT_EQ(rig.backend.trusted, std::vector<std::string>{kApp});
    EXPECT_EQ(lua.result(), QDialog::Accepted);
    EXPECT_EQ(rig.backend.cancels, 0);
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
