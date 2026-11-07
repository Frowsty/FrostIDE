// SPDX-License-Identifier: Apache-2.0
//
// Attach to Process and trust (R0 follow-up 1, FND-3): the attach prompt is its own
// TrustPurpose::DebugAttach, which never cancels a build waiting for trust; with no project open
// it confirms one attach through a session grant, so the program's (system) folder is never saved
// as a trusted root. The attach itself runs the pinned lldb-dap against a real child process
// (PING.EXE in System32 on Windows, /bin/sleep on Linux); the waiting build is a real CMake
// configure of an untrusted folder held in NeedsTrust (no cmake runs before trust).

#include "frost/build/cmake_service.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "../../src/app/src/project_properties_dialog.hpp"
#include "workbench_rig.hpp"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QTableWidget>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>

namespace {
using namespace std::chrono_literals;
using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

// A real child that sleeps in a system folder (never trusted by a test). Killed by the returned
// guard's owner (terminate()).
std::unique_ptr<frost::io::IProcess> startSleeper(std::string& executable) {
    frost::io::ProcessSpec spec;
#ifdef _WIN32
    spec.executable =
        QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32/PING.EXE")).toStdString();
    spec.arguments = {"-n", "120", "127.0.0.1"};
#else
    spec.executable = "/bin/sleep";
    spec.arguments = {"120"};
#endif
    spec.executable = QDir::toNativeSeparators(QString::fromStdString(spec.executable)).toStdString();
    spec.stdoutMode = frost::io::Stdio::Null;
    spec.stderrMode = frost::io::Stdio::Null;
    executable = spec.executable;
    auto child = frost::io::realProcessLauncher().launch(spec);
    EXPECT_TRUE(child) << child.error().describe();
    return child ? std::move(*child) : nullptr;
}

// Picks `pid` in the Attach to Process dialog that debug.attachToProcess opens.
void pickProcess(WorkbenchRig& rig, std::uint32_t pid) {
    ASSERT_TRUE(waitFor(
        [&] {
            return std::ranges::any_of(frost::app::enumerateRunningProcesses(),
                                       [&](const auto& p) { return p.pid == pid; });
        },
        10s));
    QTimer::singleShot(0, rig.window.get(), [&rig, pid] {
        auto* dialog = rig.window->findChild<QDialog*>(QStringLiteral("attachToProcessDialog"));
        ASSERT_NE(dialog, nullptr);
        auto* table = dialog->findChild<QTableWidget*>(QStringLiteral("attachProcessTable"));
        ASSERT_NE(table, nullptr);
        dialog->findChild<QLineEdit*>(QStringLiteral("attachProcessFilterEdit"))->setText(QString::number(pid));
        for (int row = 0; row < table->rowCount(); ++row)
            if (table->item(row, 0)->text() == QString::number(pid))
                table->setCurrentCell(row, 0);
        dialog->accept();
    });
    auto id = rig.workbench->registry().find("debug.attachToProcess");
    ASSERT_TRUE(id);
    ASSERT_EQ(rig.workbench->registry().tryInvoke(*id, frost::input::ActionContext{}), frost::input::InvokeResult::Ran);
}

// The open trust prompt of `purpose`, or nullptr.
frost::ui::TrustDialog* trustPrompt(WorkbenchRig& rig, frost::ui::TrustPurpose purpose) {
    for (auto* dialog : rig.window->findChildren<frost::ui::TrustDialog*>())
        if (dialog->purpose() == purpose && dialog->isVisible())
            return dialog;
    return nullptr;
}

std::string lastNotification(WorkbenchRig& rig) {
    const auto& log = rig.window->notifications().log();
    return log.empty() ? std::string("no notification") : log.back().notification.text.toStdString();
}

// No project open: the prompt confirms one attach of the pinned lldb-dap to the real process, and
// the program's system folder is trusted neither in the store's file nor for this session after it.
TEST(WorkbenchAttach, WithoutAProjectAttachesOnceAndSavesNoTrust) {
    auto lldb = frost::debug::findLldbDap(frost::io::realFileSystem(), frost::debug::defaultLldbDapSearch());
    ASSERT_TRUE(lldb.has_value()) << "the pinned lldb-dap is required: " << lldb.error().describe();
    WorkDir dir("attach-one-off");
    RigOptions options;
    options.workbench.debugAdapterFactory =
        frost::debug::lldbDapAdapterFactory(frost::io::realProcessLauncher(), *lldb);
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    std::string executable;
    auto child = startSleeper(executable);
    ASSERT_NE(child, nullptr);
    const auto killChild = qScopeGuard([&] { static_cast<void>(child->terminate()); });
    const std::string folder = std::string(frost::io::parentPath(executable));
    ASSERT_FALSE(rig.trust->isTrusted(folder));
    const std::string storeBefore = dir.read("data/trusted-roots.txt");

    // Cancel first: nothing is granted or attached.
    pickProcess(rig, static_cast<std::uint32_t>(child->pid()));
    auto* prompt = trustPrompt(rig, frost::ui::TrustPurpose::DebugAttach);
    ASSERT_NE(prompt, nullptr) << lastNotification(rig);
    EXPECT_TRUE(prompt->sessionOnly());
    EXPECT_TRUE(prompt->description().contains(QStringLiteral("not saved"))) << prompt->description().toStdString();
    // The OS reports the image path in its own case (C:\Windows vs %SystemRoot%'s C:\WINDOWS).
    ASSERT_EQ(prompt->roots().size(), 1);
    EXPECT_EQ(frost::lang::documentKey(prompt->roots().front().toStdString()), frost::lang::documentKey(folder));
    prompt->cancelButton()->click();
    QApplication::processEvents();
    EXPECT_FALSE(rig.trust->isTrusted(folder));
    EXPECT_FALSE(rig.workbench->debugController()->debugging());

    // Attach Once: the debugger attaches, and the grant is gone right after.
    pickProcess(rig, static_cast<std::uint32_t>(child->pid()));
    prompt = trustPrompt(rig, frost::ui::TrustPurpose::DebugAttach);
    ASSERT_NE(prompt, nullptr) << lastNotification(rig);
    prompt->trustButton()->click();
    auto* debug = rig.workbench->debugController();
    ASSERT_TRUE(waitFor([&] { return debug->debugging(); }, 60s)) << lastNotification(rig);
    EXPECT_FALSE(rig.trust->isTrusted(folder)) << "the one-off grant outlived the attach";
    // lldb-dap answered the attach: the session leaves Starting only on the adapter's reply.
    ASSERT_TRUE(waitFor(
        [&] {
            return debug->state() == frost::debug::DebugState::Running ||
                   debug->state() == frost::debug::DebugState::Stopped;
        },
        60s))
        << "state " << frost::debug::toString(debug->state()) << ", " << debug->statusText().toStdString() << ", "
        << lastNotification(rig);
    EXPECT_TRUE(rig.trust->roots().empty());
    EXPECT_EQ(dir.read("data/trusted-roots.txt"), storeBefore) << "the attach wrote the trust store";
    {
        auto reopened =
            frost::project::TrustStore::open(frost::io::realFileSystem(), dir.join("data/trusted-roots.txt"));
        ASSERT_TRUE(reopened);
        EXPECT_FALSE((*reopened)->isTrusted(folder));
    }
    debug->stop();
    ASSERT_TRUE(waitFor([&] { return !debug->debugging(); }, 60s));
    // Detaching leaves the process running (disconnect never terminates an attached host).
    const auto exited = child->wait(std::chrono::milliseconds(0));
    EXPECT_FALSE(exited.has_value()) << "the attached process ended with the session";
}

// A CMake folder whose configure waits in NeedsTrust: declining the attach prompt (which asks for
// the project, the open project being the attach's trust root) leaves that build waiting.
TEST(WorkbenchAttach, CancellingTheAttachPromptLeavesAWaitingBuildUntouched) {
    WorkDir dir("attach-waiting-build");
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.21)\nproject(Attach C)\n");
    WorkbenchRig rig(dir);
    rig.show();
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    auto* service = rig.workbench->cmakeService();
    ASSERT_NE(service, nullptr);
    ASSERT_TRUE(service->configure());
    ASSERT_TRUE(waitFor([&] { return rig.workbench->build().state() == frost::build::BuildState::NeedsTrust; }, 30s));
    const auto cleanup = qScopeGuard([&] { rig.workbench->cmakeService()->cancel(); });
    std::string executable;
    auto child = startSleeper(executable);
    ASSERT_NE(child, nullptr);
    const auto killChild = qScopeGuard([&] { static_cast<void>(child->terminate()); });

    pickProcess(rig, static_cast<std::uint32_t>(child->pid()));
    auto* prompt = trustPrompt(rig, frost::ui::TrustPurpose::DebugAttach);
    ASSERT_NE(prompt, nullptr) << lastNotification(rig);
    EXPECT_FALSE(prompt->sessionOnly()) << "with a project open the prompt trusts the project";
    EXPECT_EQ(prompt->roots(), QStringList{QString::fromStdString(dir.path())});
    prompt->cancelButton()->click();
    QApplication::processEvents();
    EXPECT_EQ(rig.workbench->build().state(), frost::build::BuildState::NeedsTrust);
    EXPECT_EQ(service->state(), frost::build::BuildState::NeedsTrust);
    EXPECT_FALSE(rig.trust->isTrusted(dir.path()));
    EXPECT_FALSE(rig.workbench->debugController()->debugging());

    // Closing it with Escape (QDialog::reject) leaves it too.
    pickProcess(rig, static_cast<std::uint32_t>(child->pid()));
    prompt = trustPrompt(rig, frost::ui::TrustPurpose::DebugAttach);
    ASSERT_NE(prompt, nullptr) << lastNotification(rig);
    prompt->reject();
    QApplication::processEvents();
    EXPECT_EQ(service->state(), frost::build::BuildState::NeedsTrust);
    EXPECT_FALSE(rig.workbench->debugController()->debugging());
}

} // namespace
