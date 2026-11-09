// SPDX-License-Identifier: Apache-2.0
//
// Attach to Process and trust (R0 follow-up 1, FND-3): the attach prompt is its own
// TrustPurpose::DebugAttach, which never cancels a build waiting for trust; with no project open
// it confirms one attach through a session grant, so the program's (system) folder is never saved
// as a trusted root. The attach itself runs the pinned lldb-dap against a real child process
// (PING.EXE in System32 on Windows, the system python3 on Linux); the waiting build is a real CMake
// configure of an untrusted folder held in NeedsTrust (no cmake runs before trust). RUN-5: an
// attach configuration attaches to the one running process it names (a uniquely named copy of
// io_test_child), and Debug runs a native configuration's before-launch tasks first, only in a
// trusted workspace (an untrusted one asks first and starts nothing).

#include "frost/build/cmake_service.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/uri.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/run/run_types.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "../../src/app/src/project_properties_dialog.hpp"
#include "io/process_env.hpp"
#include "workbench_rig.hpp"
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QTableWidget>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <system_error>

namespace {
using namespace std::chrono_literals;
using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;

/*
 * A real child that sleeps in a system folder (never trusted by a test). Killed by the returned
 * guard's owner (terminate()). Returns once the child is ready to be attached to.
 *
 * On Linux the debugger and the child are siblings (both children of this test), and the Yama
 * ptrace policy of Ubuntu and WSL (kernel.yama.ptrace_scope = 1) lets a process attach only to its
 * own descendants. So the child is the system python3, which first allows any tracer
 * (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY), what a debuggee does for an IDE under that policy),
 * then writes `readyMarker` and sleeps. Without it lldb-dap's attach is refused by the kernel.
 */
std::unique_ptr<frost::io::IProcess> startSleeper(std::string& executable, const std::string& readyMarker) {
    frost::io::ProcessSpec spec;
#ifdef _WIN32
    static_cast<void>(readyMarker);
    spec.executable =
        QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32/PING.EXE")).toStdString();
    spec.arguments = {"-n", "120", "127.0.0.1"};
#else
    spec.executable = "/usr/bin/python3";
    spec.arguments = {"-I", "-c",
                      "import ctypes, sys, time\n"
                      "if ctypes.CDLL(None, use_errno=True).prctl(0x59616d61, ctypes.c_ulong(-1), 0, 0, 0) != 0:\n"
                      "    sys.exit('prctl(PR_SET_PTRACER) failed')\n"
                      "open(sys.argv[1], 'w').close()\n"
                      "time.sleep(120)\n",
                      readyMarker};
#endif
    spec.executable = QDir::toNativeSeparators(QString::fromStdString(spec.executable)).toStdString();
    spec.stdoutMode = frost::io::Stdio::Null;
    spec.stderrMode = frost::io::Stdio::Null;
    executable = spec.executable;
    auto child = frost::io::realProcessLauncher().launch(spec);
    EXPECT_TRUE(child) << child.error().describe();
    if (!child)
        return nullptr;
#ifndef _WIN32
    EXPECT_TRUE(waitFor([&] { return frost::io::realFileSystem().exists(readyMarker).value_or(false); }, 30s))
        << "the python3 sleeper did not start";
#endif
    return std::move(*child);
}

// The folder the OS reports for the running image of `executable`: on Linux the kernel resolves
// symlinks (/bin/x and /usr/bin/python3 are links on Ubuntu), so it is the realpath's folder.
std::string imageFolder(const std::string& executable) {
#ifdef _WIN32
    return std::string(frost::io::parentPath(executable));
#else
    std::error_code error;
    const std::filesystem::path resolved = std::filesystem::canonical(executable, error);
    EXPECT_FALSE(error) << executable << ": " << error.message();
    return resolved.parent_path().string();
#endif
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
    auto child = startSleeper(executable, dir.join("sleeper-ready"));
    ASSERT_NE(child, nullptr);
    const auto killChild = qScopeGuard([&] { static_cast<void>(child->terminate()); });
    const std::string folder = imageFolder(executable);
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
    auto child = startSleeper(executable, dir.join("sleeper-ready"));
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

/*
 * RUN-5 helpers. A copy of io_test_child under a name no other process has (the test's own pid in
 * it), so "the one running process this attach configuration names" is decidable even while other
 * suites run io_test_child in parallel.
 */
std::string uniqueChildCopy(const WorkDir& dir, std::string_view tag) {
#ifdef _WIN32
    constexpr std::string_view kSuffix = ".exe";
#else
    constexpr std::string_view kSuffix = "";
#endif
    const std::string copy = dir.join(std::format("frost-{}-{}{}", tag, QCoreApplication::applicationPid(), kSuffix));
    std::error_code error;
    std::filesystem::copy_file(std::filesystem::path(frost::test::childPath()), std::filesystem::path(copy),
                               std::filesystem::copy_options::overwrite_existing, error);
    EXPECT_FALSE(error) << copy << ": " << error.message();
    return copy;
}

// Starts `executable` (an io_test_child) as "traceable-sleep": a debugger may attach once it returns.
std::unique_ptr<frost::io::IProcess> startTraceable(const std::string& executable, const std::string& readyMarker) {
    frost::io::ProcessSpec spec;
    spec.executable = executable;
    spec.arguments = {"traceable-sleep", "120000", readyMarker};
    spec.stdoutMode = frost::io::Stdio::Null;
    spec.stderrMode = frost::io::Stdio::Null;
    auto child = frost::io::realProcessLauncher().launch(spec);
    EXPECT_TRUE(child) << child.error().describe();
    if (!child)
        return nullptr;
    EXPECT_TRUE(waitFor([&] { return frost::io::realFileSystem().exists(readyMarker).value_or(false); }, 30s))
        << "the traceable sleeper did not start";
    return std::move(*child);
}

bool notified(WorkbenchRig& rig, const QString& fragment) {
    for (const auto& entry : rig.window->notifications().log())
        if (entry.notification.text.contains(fragment))
            return true;
    return false;
}

// Opens `dir` as a trusted workspace and stores `configurations` as its run configurations.
void openWithConfigurations(WorkbenchRig& rig, const WorkDir& dir, std::vector<frost::run::RunConfiguration> added) {
    ASSERT_TRUE(rig.trust->trust(dir.path()).has_value());
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    auto* controller = rig.workbench->runController();
    ASSERT_NE(controller, nullptr);
    std::vector<frost::run::RunConfiguration> edited = controller->configurations().all();
    edited.insert(edited.end(), added.begin(), added.end());
    auto replaced = controller->replaceConfigurations(edited);
    ASSERT_TRUE(replaced.has_value()) << replaced.error().describe();
}

/*
 * RUN-5: Debug on an attach configuration attaches the pinned lldb-dap to the one running process
 * whose executable it names. The running processes are the OS's (enumerateRunningProcesses); none
 * or two matching refuse with a notification naming them, and nothing attaches.
 */
TEST(WorkbenchAttach, AnAttachConfigurationAttachesToTheOneProcessItNames) {
    auto lldb = frost::debug::findLldbDap(frost::io::realFileSystem(), frost::debug::defaultLldbDapSearch());
    ASSERT_TRUE(lldb.has_value()) << "the pinned lldb-dap is required: " << lldb.error().describe();
    WorkDir dir("attach-configuration");
    dir.write("readme.txt", "a workspace with an attach configuration\n");
    RigOptions options;
    options.workbench.debugAdapterFactory =
        frost::debug::lldbDapAdapterFactory(frost::io::realProcessLauncher(), *lldb);
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    const std::string executable = uniqueChildCopy(dir, "attachee");
    frost::run::RunConfiguration attach;
    attach.id = "attach.attachee";
    attach.name = "Attach to the attachee";
    attach.type = frost::run::RunType::Attach;
    attach.target = std::string(frost::io::fileName(executable));
    openWithConfigurations(rig, dir, {attach});
    auto* controller = rig.workbench->runController();
    ASSERT_TRUE(controller->selectConfiguration(attach.id).has_value());
    auto* debug = rig.workbench->debugController();

    // Nothing runs under that name yet.
    auto refused = debug->debug();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::NotFound) << refused.error().describe();
    EXPECT_TRUE(notified(rig, QStringLiteral("No running process matches"))) << lastNotification(rig);
    EXPECT_FALSE(debug->debugging());

    // Two of them: which one is the user's choice (Attach to Process...).
    auto first = startTraceable(executable, dir.join("first-ready"));
    ASSERT_NE(first, nullptr);
    const auto killFirst = qScopeGuard([&] { static_cast<void>(first->terminate()); });
    auto second = startTraceable(executable, dir.join("second-ready"));
    ASSERT_NE(second, nullptr);
    {
        const auto killSecond = qScopeGuard([&] { static_cast<void>(second->terminate()); });
        refused = debug->debug();
        ASSERT_FALSE(refused.has_value());
        EXPECT_NE(refused.error().message().find(std::to_string(second->pid())), std::string::npos)
            << refused.error().message();
        EXPECT_FALSE(debug->debugging());
    }
    ASSERT_TRUE(second->wait(30s).has_value()) << "the second sleeper did not end";

    // One: lldb-dap attaches to it (the session leaves Starting only on the adapter's reply).
    ASSERT_TRUE(waitFor(
        [&] {
            return std::ranges::count_if(frost::app::enumerateRunningProcesses(), [&](const auto& process) {
                       return frost::run::attachTargetMatches(attach.target, process.executablePath);
                   }) == 1;
        },
        30s));
    auto started = debug->debug();
    ASSERT_TRUE(started.has_value()) << started.error().describe();
    ASSERT_TRUE(waitFor([&] { return debug->debugging(); }, 60s)) << lastNotification(rig);
    ASSERT_TRUE(waitFor(
        [&] {
            return debug->state() == frost::debug::DebugState::Running ||
                   debug->state() == frost::debug::DebugState::Stopped;
        },
        60s))
        << "state " << frost::debug::toString(debug->state()) << ", " << debug->statusText().toStdString() << ", "
        << lastNotification(rig);
    debug->stop();
    ASSERT_TRUE(waitFor([&] { return !debug->debugging(); }, 60s));
    EXPECT_FALSE(first->wait(std::chrono::milliseconds(0)).has_value()) << "detaching ended the attached process";
}

/*
 * RUN-5: Debug runs a native configuration's before-launch tasks before lldb-dap launches its
 * program (the task, a real process, leaves a file behind before the session starts); a failing
 * task ends the Debug with the task's exit code, and no session starts.
 */
TEST(WorkbenchAttach, DebugRunsTheBeforeLaunchTasksFirst) {
    auto lldb = frost::debug::findLldbDap(frost::io::realFileSystem(), frost::debug::defaultLldbDapSearch());
    ASSERT_TRUE(lldb.has_value()) << "the pinned lldb-dap is required: " << lldb.error().describe();
    WorkDir dir("debug-tasks");
    dir.write("readme.txt", "a workspace with before-launch tasks\n");
    RigOptions options;
    options.workbench.debugAdapterFactory =
        frost::debug::lldbDapAdapterFactory(frost::io::realProcessLauncher(), *lldb);
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    const std::string marker = dir.join("task-ran");
    frost::run::RunConfiguration program;
    program.id = "native.sleeper";
    program.name = "Sleeper";
    program.type = frost::run::RunType::Native;
    program.program = frost::test::childPath();
    program.arguments = {"sleep", "60000"};
    program.beforeLaunchTasks = {
        {.program = frost::test::childPath(), .arguments = {"traceable-sleep", "0", marker}, .workingDirectory = {}}};
    frost::run::RunConfiguration failing = program;
    failing.id = "native.failing";
    failing.name = "Failing";
    failing.beforeLaunchTasks = {
        {.program = frost::test::childPath(), .arguments = {"exit", "3"}, .workingDirectory = {}}};
    openWithConfigurations(rig, dir, {program, failing});
    auto* controller = rig.workbench->runController();
    auto* debug = rig.workbench->debugController();

    ASSERT_TRUE(controller->selectConfiguration(failing.id).has_value());
    ASSERT_TRUE(debug->debug().has_value());
    ASSERT_TRUE(waitFor([&] { return notified(rig, QStringLiteral("exited with code 3")); }, 60s))
        << lastNotification(rig);
    EXPECT_FALSE(debug->debugging());
    EXPECT_TRUE(rig.workbench->runManager()->waitForIdle(30s));

    ASSERT_FALSE(frost::io::realFileSystem().exists(marker).value_or(true));
    ASSERT_TRUE(controller->selectConfiguration(program.id).has_value());
    ASSERT_TRUE(debug->debug().has_value());
    // lldb-dap launched the program (the session leaves Starting only on the adapter's reply); the
    // task had finished before: its file is there.
    ASSERT_TRUE(waitFor(
        [&] {
            return debug->state() == frost::debug::DebugState::Running ||
                   debug->state() == frost::debug::DebugState::Stopped;
        },
        60s))
        << "state " << frost::debug::toString(debug->state()) << ", " << lastNotification(rig);
    EXPECT_TRUE(frost::io::realFileSystem().exists(marker).value_or(false)) << "the debugger started before the task";
    debug->stop();
    ASSERT_TRUE(waitFor([&] { return !debug->debugging(); }, 60s));
}

/*
 * RUN-5 trust: a workspace that ships a native configuration with a before-launch task (as a
 * freshly cloned repository may) starts nothing on Debug until the user trusts it. The task is a
 * real io_test_child that would leave a file behind; the OS file system is the oracle. Declining
 * the prompt starts nothing; trusting the workspace in it starts that Debug: the task runs first,
 * then the pinned lldb-dap launches the program.
 */
TEST(WorkbenchAttach, DebugInAnUntrustedWorkspaceRunsNoBeforeLaunchTaskUntilTrusted) {
    auto lldb = frost::debug::findLldbDap(frost::io::realFileSystem(), frost::debug::defaultLldbDapSearch());
    ASSERT_TRUE(lldb.has_value()) << "the pinned lldb-dap is required: " << lldb.error().describe();
    WorkDir dir("debug-tasks-untrusted");
    dir.write("readme.txt", "an untrusted workspace that ships a run configuration\n");
    const std::string marker = dir.join("task-ran");
    frost::run::RunConfiguration shipped;
    shipped.id = "native.shipped";
    shipped.name = "Shipped";
    shipped.type = frost::run::RunType::Native;
    shipped.program = frost::test::childPath();
    shipped.arguments = {"sleep", "60000"};
    shipped.beforeLaunchTasks = {
        {.program = frost::test::childPath(), .arguments = {"traceable-sleep", "0", marker}, .workingDirectory = {}}};
    const std::vector<frost::run::RunConfiguration> stored{shipped};
    ASSERT_TRUE(
        frost::run::saveWorkspaceRunConfigurations(frost::io::realFileSystem(), dir.path(), stored).has_value());
    RigOptions options;
    options.workbench.debugAdapterFactory =
        frost::debug::lldbDapAdapterFactory(frost::io::realProcessLauncher(), *lldb);
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    ASSERT_FALSE(rig.trust->isTrusted(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()).has_value());
    auto* controller = rig.workbench->runController();
    ASSERT_NE(controller->configurations().find(shipped.id), nullptr) << "the shipped configuration was not loaded";
    ASSERT_TRUE(controller->selectConfiguration(shipped.id).has_value());
    auto* debug = rig.workbench->debugController();

    // Debug is refused before anything starts, and the workspace trust prompt asks.
    auto refused = debug->debug();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::Untrusted) << refused.error().describe();
    EXPECT_TRUE(rig.workbench->runManager()->activeRuns().empty());
    EXPECT_FALSE(debug->debugging());
    frost::ui::TrustDialog* prompt = nullptr;
    ASSERT_TRUE(waitFor([&] { return (prompt = trustPrompt(rig, frost::ui::TrustPurpose::Build)) != nullptr; }))
        << lastNotification(rig);
    EXPECT_EQ(prompt->roots(), QStringList{QString::fromStdString(dir.path())});
    prompt->cancelButton()->click();
    QApplication::processEvents();
    EXPECT_FALSE(rig.trust->isTrusted(dir.path()));
    EXPECT_TRUE(rig.workbench->runManager()->waitForIdle(30s));
    EXPECT_FALSE(debug->debugging());
    EXPECT_FALSE(frost::io::realFileSystem().exists(marker).value_or(true)) << "a before-launch task ran untrusted";

    // Trusting the workspace in the prompt starts this Debug: the task first, then the program.
    refused = debug->debug();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), frost::ErrorCode::Untrusted) << refused.error().describe();
    ASSERT_TRUE(waitFor([&] { return (prompt = trustPrompt(rig, frost::ui::TrustPurpose::Build)) != nullptr; }))
        << lastNotification(rig);
    EXPECT_FALSE(frost::io::realFileSystem().exists(marker).value_or(true));
    prompt->trustButton()->click();
    EXPECT_TRUE(rig.trust->isTrusted(dir.path()));
    ASSERT_TRUE(waitFor(
        [&] {
            return debug->state() == frost::debug::DebugState::Running ||
                   debug->state() == frost::debug::DebugState::Stopped;
        },
        60s))
        << "state " << frost::debug::toString(debug->state()) << ", " << lastNotification(rig);
    EXPECT_TRUE(frost::io::realFileSystem().exists(marker).value_or(false)) << "the debugger started before the task";
    debug->stop();
    ASSERT_TRUE(waitFor([&] { return !debug->debugging(); }, 60s));
}

} // namespace
