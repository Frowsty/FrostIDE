// SPDX-License-Identifier: Apache-2.0
// Workbench terminal composition: live trust, settings, actions and session shutdown.
#include "frost/term/fake_pty.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/terminal/terminal_tool_window.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/solution_tree_model.hpp"

#include "workbench_rig.hpp"
#include <QAction>
#include <QMenu>
#include <gtest/gtest.h>

#include <algorithm>

namespace {
using frost::app::test::RigOptions;
using frost::app::test::waitFor;
using frost::app::test::WorkbenchRig;
using frost::app::test::WorkDir;
namespace actions = frost::ui::terminal_actions;

TEST(WorkbenchTerminal, UsesCurrentTrustAndStopsSessions) {
    WorkDir dir("terminal");
    frost::term::FakePtyLauncher launcher;
    RigOptions options;
    options.workbench.clangd = false;
    options.workbench.terminalLauncher = &launcher;
    frost::term::ShellProbe probe;
    const std::string home = dir.join("home");
    probe.environment = [home](std::string_view key) -> std::optional<std::string> {
        return key == "HOME" || key == "USERPROFILE" ? std::optional<std::string>(home) : std::nullopt;
    };
    options.workbench.terminalProbe = std::move(probe);
    WorkbenchRig rig(dir, std::move(options));
    const std::string shell = dir.join("shell.exe");
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::terminalShell, shell));
    ASSERT_TRUE(rig.workbench->openPath(dir.path()));
    rig.show();
    ASSERT_EQ(rig.window->invoke(actions::kTerminalWindow), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return launcher.count() == 1; }));
    EXPECT_EQ(launcher.specs()[0].executable, shell);
    EXPECT_EQ(launcher.specs()[0].workingDirectory, home);
#ifdef _WIN32
    const auto restrictedEnv = launcher.specs()[0].environment;
    const auto hardened = std::ranges::find_if(
        restrictedEnv, [](const auto& entry) { return entry.first == "NoDefaultCurrentDirectoryInExePath"; });
    ASSERT_NE(hardened, restrictedEnv.end());
    EXPECT_EQ(hardened->second, std::optional<std::string>("1"));
#endif
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_EQ(rig.window->invoke(actions::kNewTab), frost::input::InvokeResult::Ran);
    ASSERT_TRUE(waitFor([&] { return launcher.count() == 2; }));
    EXPECT_EQ(launcher.specs()[1].workingDirectory, dir.path());
#ifdef _WIN32
    const auto trustedEnv = launcher.specs()[1].environment;
    const auto restored = std::ranges::find_if(
        trustedEnv, [](const auto& entry) { return entry.first == "NoDefaultCurrentDirectoryInExePath"; });
    ASSERT_NE(restored, trustedEnv.end());
    EXPECT_EQ(restored->second, std::nullopt);
#endif
    auto first = launcher.pty(0);
    auto second = launcher.pty(1);
    rig.workbench->shutdown();
    EXPECT_TRUE(first->closed());
    EXPECT_TRUE(second->closed());
}

TEST(WorkbenchTerminal, ShutdownCancelsQueuedFirstTab) {
    WorkDir dir("terminal-shutdown");
    frost::term::FakePtyLauncher launcher;
    RigOptions options;
    options.workbench.clangd = false;
    options.workbench.terminalLauncher = &launcher;
    WorkbenchRig rig(dir, std::move(options));
    rig.show();
    ASSERT_EQ(rig.window->invoke(actions::kTerminalWindow), frost::input::InvokeResult::Ran);
    rig.workbench->shutdown();
    QApplication::processEvents();
    EXPECT_EQ(launcher.count(), 0U);
}

TEST(WorkbenchTerminal, ExplorerOpensTheSelectedFilesFolder) {
    WorkDir dir("terminal-explorer", "tiny_sln");
    frost::term::FakePtyLauncher launcher;
    RigOptions options;
    options.workbench.clangd = false;
    options.workbench.terminalLauncher = &launcher;
    frost::term::ShellProbe probe;
    probe.environment = [home = dir.path()](std::string_view key) -> std::optional<std::string> {
        return key == "HOME" || key == "USERPROFILE" ? std::optional<std::string>(home) : std::nullopt;
    };
    options.workbench.terminalProbe = std::move(probe);
    WorkbenchRig rig(dir, std::move(options));
    ASSERT_TRUE(rig.settings->set(frost::settings::Scope::Application, frost::settings::key::terminalShell,
                                  dir.join("shell.exe")));
    ASSERT_TRUE(rig.trust->trust(dir.path()));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("Tiny.sln")));
    rig.show();
    auto* explorer = rig.workbench->solutionExplorer();
    ASSERT_NE(explorer, nullptr);
    const auto source = explorer->model()->indexOfPath(dir.join("App/main.cpp"));
    ASSERT_TRUE(source.isValid());
    const auto index = explorer->filterModel()->mapFromSource(source);
    std::unique_ptr<QMenu> menu(explorer->createContextMenu(index));
    QAction* action = menu->findChild<QAction*>(QStringLiteral("openInTerminal"));
    ASSERT_NE(action, nullptr);
    action->trigger();
    ASSERT_TRUE(waitFor([&] { return launcher.count() == 1; }));
    EXPECT_EQ(launcher.specs()[0].workingDirectory, dir.join("App"));
}

} // namespace
