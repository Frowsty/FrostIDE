// SPDX-License-Identifier: Apache-2.0
/*
 * Tests for RUN-3's widgets in frost::ui_run, offscreen (no window is shown): the Run tool window
 * v2's tabs (reuse by key, the empty first tab, the running title, closing stops the run), the
 * argument and environment text of Run/Debug Configurations, the dialog itself (editing a native
 * configuration, what a generated cmake-target and msbuild configuration allow, validation on OK),
 * RUN-5's before-launch tasks, compound members and attach process in the dialog, and RunController
 * running a native configuration, and a compound one's members, through a RunManager over the
 * scripted launcher into their own tabs, with the configuration combo and the compound and attach
 * refusals.
 */

#include "frost/io/scripted_process_launcher.hpp"
#include "frost/run/run_configuration_set.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/run/run_types.hpp"
#include "frost/ui/run_configurations_dialog.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/run_tool_window.hpp"

#include "build/build_test_util.hpp"
#include <QComboBox>
#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace frost::ui {
namespace {

using namespace std::chrono_literals;
using build::test::MemTree;

bool spinUntil(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

build::RunOutcome exited(int code) {
    build::RunOutcome outcome;
    outcome.exitCode = code;
    return outcome;
}

TEST(RunToolWindow, ARunOfAKeyReusesItsFinishedTabAndOthersGetTheirOwn) {
    RunToolWindow window;
    ASSERT_EQ(window.count(), 1);
    RunConsole* empty = window.currentConsole();
    ASSERT_NE(empty, nullptr);
    EXPECT_FALSE(empty->used());

    int stops = 0;
    RunConsole* first = window.openRun(QStringLiteral("config:a"), QStringLiteral("A"), {.stop = [&] { ++stops; }});
    EXPECT_EQ(first, empty) << "the empty first tab takes the first run";
    EXPECT_EQ(window.currentConsole(), first);
    EXPECT_TRUE(first->active());
    EXPECT_EQ(window.tabs()->tabText(0), QStringLiteral("A (running)"));

    // A second run of A while A runs, and a run of B: tabs of their own.
    RunConsole* second = window.openRun(QStringLiteral("config:a"), QStringLiteral("A"), {});
    RunConsole* other = window.openRun(QStringLiteral("config:b"), QStringLiteral("B"), {});
    EXPECT_NE(second, first);
    EXPECT_NE(other, first);
    EXPECT_NE(other, second);
    EXPECT_EQ(window.count(), 3);
    EXPECT_EQ(window.consoleFor(QStringLiteral("config:b")), other);
    EXPECT_EQ(window.keyOf(other), QStringLiteral("config:b"));

    // Finished, A's newest tab is reused by A's next run, and its old output goes.
    second->appendOutput(build::RunOutputKind::Stdout, "old output\n");
    second->finish(exited(0));
    EXPECT_FALSE(second->active());
    EXPECT_EQ(window.tabs()->tabText(window.tabs()->indexOf(second)), QStringLiteral("A"));
    RunConsole* again = window.openRun(QStringLiteral("config:a"), QStringLiteral("A"), {});
    EXPECT_EQ(again, second);
    EXPECT_FALSE(again->text().contains(QStringLiteral("old output")));
    EXPECT_EQ(window.count(), 3);

    // The console's Stop calls the tab's handler; closing an active tab stops its run first.
    emit first->stopRequested();
    EXPECT_EQ(stops, 1);
    window.closeTab(window.tabs()->indexOf(first));
    EXPECT_EQ(stops, 2);
    EXPECT_EQ(window.count(), 2);
    window.closeTab(0);
    window.closeTab(0);
    EXPECT_EQ(window.count(), 1) << "the last tab closed leaves an empty one";
    ASSERT_NE(window.currentConsole(), nullptr);
    EXPECT_FALSE(window.currentConsole()->used());
}

TEST(RunToolWindow, RerunInTheEmptyTabAsksTheOwner) {
    RunToolWindow window;
    int asked = 0;
    QObject::connect(&window, &RunToolWindow::rerunWithoutRunRequested, [&] { ++asked; });
    emit window.currentConsole()->rerunRequested();
    EXPECT_EQ(asked, 1);
    int reruns = 0;
    RunConsole* console = window.openRun(QStringLiteral("k"), QStringLiteral("K"), {.rerun = [&] { ++reruns; }});
    console->finish(exited(1));
    emit console->rerunRequested();
    EXPECT_EQ(reruns, 1);
    EXPECT_EQ(asked, 1);
    EXPECT_TRUE(window.rerunCurrent());
    EXPECT_EQ(reruns, 2);
}

TEST(RunToolWindow, ADetachedTabCallsNoHandlerAndDoesNotAskTheOwner) {
    // detach(): the tab's owner is gone (a closed folder's CMake integration); the tab stays inert.
    RunToolWindow window;
    int asked = 0;
    int calls = 0;
    QObject::connect(&window, &RunToolWindow::rerunWithoutRunRequested, [&] { ++asked; });
    RunConsole* console = window.openRun(
        QStringLiteral("cmake:t"), QStringLiteral("t"),
        {.stop = [&] { ++calls; }, .rerun = [&] { ++calls; }, .input = [&](const QString&) { ++calls; }});
    window.detach(console);
    window.detach(nullptr); // Null: nothing.
    emit console->stopRequested();
    emit console->inputSubmitted(QStringLiteral("x"));
    console->finish(exited(0));
    emit console->rerunRequested();
    EXPECT_FALSE(window.rerunCurrent());
    window.closeTab(window.tabs()->indexOf(console));
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(asked, 0) << "only a tab that never ran falls back to the owner's Rerun";
    EXPECT_EQ(window.count(), 1);
}

TEST(RunConfigurationsText, ArgumentsAndEnvironmentRoundTrip) {
    const std::vector<std::string> arguments{"--offscreen", "two words", "quote\"inside", ""};
    const QString text = formatRunArguments(arguments);
    EXPECT_EQ(parseRunArguments(text), arguments) << text.toStdString();
    EXPECT_EQ(parseRunArguments(QStringLiteral("  a   \"b c\"  ")), (std::vector<std::string>{"a", "b c"}));
    EXPECT_TRUE(parseRunArguments(QString()).empty());

    const std::vector<run::EnvironmentEntry> environment{
        {"A", std::string("1")}, {"EQ", std::string("x=y")}, {"GONE", std::nullopt}};
    const QString lines = formatRunEnvironment(environment);
    EXPECT_EQ(lines, QStringLiteral("A=1\nEQ=x=y\nGONE"));
    EXPECT_EQ(parseRunEnvironment(lines + QStringLiteral("\n\n")), environment);
}

run::RunConfiguration cmakeTarget(std::string name) {
    run::RunConfiguration configuration;
    configuration.id = run::runConfigurationIdFor(run::kCMakeIdPrefix, name);
    configuration.name = name;
    configuration.type = run::RunType::CMakeTarget;
    configuration.target = std::move(name);
    configuration.workingDirectory = MemTree::path("src");
    configuration.beforeLaunch = run::BeforeLaunch::Build;
    return configuration;
}

template<class W>
W* child(QWidget& parent, const char* name) {
    auto* widget = parent.findChild<W*>(QLatin1String(name));
    EXPECT_NE(widget, nullptr) << name;
    return widget;
}

TEST(RunConfigurationsDialog, EditsANativeConfigurationAndKeepsGeneratedOnes) {
    run::RunConfigurationSet set;
    run::RunConfiguration msbuild;
    msbuild.id = "msbuild.App";
    msbuild.name = "App";
    msbuild.type = run::RunType::MSBuild;
    msbuild.target = MemTree::path("Run/App/App.vcxproj");
    set.setGenerated({cmakeTarget("editor"), msbuild});
    RunConfigurationsDialog dialog(set, "cmake.editor");
    auto* list = child<QListWidget>(dialog, "runConfigurationList");
    auto* name = child<QLineEdit>(dialog, "runConfigurationName");
    auto* program = child<QLineEdit>(dialog, "runConfigurationProgram");
    auto* arguments = child<QLineEdit>(dialog, "runConfigurationArguments");
    auto* cwd = child<QLineEdit>(dialog, "runConfigurationWorkingDirectory");
    auto* environment = child<QPlainTextEdit>(dialog, "runConfigurationEnvironment");
    auto* remove = child<QPushButton>(dialog, "runConfigurationRemove");
    auto* error = child<QLabel>(dialog, "runConfigurationError");
    ASSERT_TRUE(list && name && program && arguments && cwd && environment && remove && error);
    ASSERT_EQ(list->count(), 2);

    // A generated cmake-target: no program to type, and it cannot be removed.
    EXPECT_EQ(dialog.selectedId(), "cmake.editor");
    EXPECT_TRUE(program->isReadOnly());
    EXPECT_FALSE(arguments->isReadOnly());
    EXPECT_FALSE(remove->isEnabled());
    EXPECT_FALSE(dialog.removeSelected());
    arguments->setText(QStringLiteral("--offscreen \"two words\""));

    // msbuild: only the name; the debugger settings are SLN-11's.
    list->setCurrentRow(1);
    EXPECT_EQ(dialog.selectedId(), "msbuild.App");
    EXPECT_TRUE(arguments->isReadOnly());
    EXPECT_TRUE(cwd->isReadOnly());
    EXPECT_TRUE(child<QLabel>(dialog, "runConfigurationNote")->text().contains(QStringLiteral("SLN-11")));

    // A native one: every field.
    const std::string id = dialog.addNative();
    EXPECT_EQ(dialog.selectedId(), id);
    EXPECT_FALSE(program->isReadOnly());
    EXPECT_TRUE(remove->isEnabled());
    name->setText(QStringLiteral("Tool"));
    program->setText(QStringLiteral("tool.exe")); // Relative: OK refuses it.
    environment->setPlainText(QStringLiteral("A=1\nGONE"));
    dialog.accept();
    EXPECT_EQ(dialog.result(), QDialog::Rejected) << "not accepted yet";
    EXPECT_FALSE(error->text().isEmpty());
    program->setText(QString::fromStdString(MemTree::path("bin/tool.exe")));
    cwd->setText(QString::fromStdString(MemTree::path("work")));
    dialog.accept();
    EXPECT_EQ(dialog.result(), QDialog::Accepted) << error->text().toStdString();

    const std::vector<run::RunConfiguration> edited = dialog.edited();
    ASSERT_EQ(edited.size(), 3U);
    EXPECT_EQ(edited[0].arguments, (std::vector<std::string>{"--offscreen", "two words"}));
    EXPECT_EQ(edited[0].type, run::RunType::CMakeTarget);
    EXPECT_EQ(edited[2].id, id);
    EXPECT_EQ(edited[2].name, "Tool");
    EXPECT_EQ(edited[2].type, run::RunType::Native);
    EXPECT_EQ(edited[2].program, MemTree::path("bin/tool.exe"));
    EXPECT_EQ(edited[2].workingDirectory, MemTree::path("work"));
    EXPECT_EQ(edited[2].environment,
              (std::vector<run::EnvironmentEntry>{{"A", std::string("1")}, {"GONE", std::nullopt}}));
    // Stored, the edits come back through the set's merge.
    set.replace(edited);
    ASSERT_NE(set.find("cmake.editor"), nullptr);
    EXPECT_EQ(set.find("cmake.editor")->arguments, edited[0].arguments);
    EXPECT_NE(set.find(id), nullptr);
}

// RUN-5: without a workspace nothing can be stored, so compound and attach configurations cannot be
// added either; the compound button says that compound debugging is DBG-10's. (Replaces R2's test of
// the disabled RUN-5 placeholder, which is now the feature.)
TEST(RunConfigurationsDialog, CompoundAndAttachConfigurationsNeedAWorkspace) {
    run::RunConfigurationSet set;
    RunConfigurationsDialog dialog(set, "", false);
    auto* group = child<QWidget>(dialog, "compoundRunGroup");
    auto* compound = child<QPushButton>(dialog, "compoundLaunchButton");
    auto* attach = child<QPushButton>(dialog, "attachConfigurationButton");
    ASSERT_TRUE(group && compound && attach);
    EXPECT_FALSE(compound->isEnabled());
    EXPECT_FALSE(attach->isEnabled());
    EXPECT_TRUE(compound->toolTip().contains(QStringLiteral("DBG-10")));
    EXPECT_EQ(dialog.addCompound(), "");
    EXPECT_EQ(dialog.addAttach(), "");
    // No workspace: nowhere to store a native configuration.
    EXPECT_FALSE(child<QPushButton>(dialog, "runConfigurationAdd")->isEnabled());
}

// RUN-5 in the dialog: a native configuration's before-launch tasks, a compound configuration's
// members and an attach configuration's process, each validated on OK.
TEST(RunConfigurationsDialog, EditsTasksCompoundMembersAndTheAttachProcess) {
    run::RunConfigurationSet set;
    run::RunConfiguration msbuild;
    msbuild.id = "msbuild.App";
    msbuild.name = "App";
    msbuild.type = run::RunType::MSBuild;
    msbuild.target = MemTree::path("Run/App/App.vcxproj");
    run::RunConfiguration server =
        run::nativeRunConfiguration("Server", MemTree::path("bin/server.exe"), std::vector<run::RunConfiguration>{});
    server.beforeLaunchTasks = {
        {.program = MemTree::path("tools/gen.exe"), .arguments = {"--out"}, .workingDirectory = MemTree::path("gen")}};
    set.setGenerated({cmakeTarget("editor"), msbuild});
    set.setStored({server});
    RunConfigurationsDialog dialog(set, server.id);
    auto* tasks = child<QPlainTextEdit>(dialog, "runConfigurationTasks");
    auto* members = child<QListWidget>(dialog, "runConfigurationMembers");
    auto* process = child<QLineEdit>(dialog, "runConfigurationProcess");
    auto* name = child<QLineEdit>(dialog, "runConfigurationName");
    auto* list = child<QListWidget>(dialog, "runConfigurationList");
    auto* error = child<QLabel>(dialog, "runConfigurationError");
    ASSERT_TRUE(tasks && members && process && name && list && error);

    // The native one's task is one line; an unchanged line keeps its working directory.
    EXPECT_EQ(tasks->toPlainText(), QString::fromStdString(MemTree::path("tools/gen.exe")) + QStringLiteral(" --out"));
    EXPECT_FALSE(tasks->isReadOnly());
    tasks->setPlainText(tasks->toPlainText() + QStringLiteral("\n\"") +
                        QString::fromStdString(MemTree::path("tools/check tool.exe")) + QStringLiteral("\" -q\n"));
    // msbuild configurations take no tasks.
    list->setCurrentRow(1);
    EXPECT_TRUE(tasks->isReadOnly());

    // A compound of the CMake target and the native server; the list offers the three others.
    const std::string compoundId = dialog.addCompound();
    ASSERT_FALSE(compoundId.empty());
    EXPECT_EQ(dialog.selectedId(), compoundId);
    ASSERT_EQ(members->count(), 3);
    name->setText(QStringLiteral("Editor and server"));
    dialog.accept();
    EXPECT_EQ(dialog.result(), QDialog::Rejected) << "a compound with no member is refused";
    EXPECT_TRUE(error->text().contains(QStringLiteral("at least one"))) << error->text().toStdString();
    for (int i = 0; i < members->count(); ++i) {
        const QString id = members->item(i)->data(Qt::UserRole).toString();
        if (id == QStringLiteral("cmake.editor") || id == QString::fromStdString(server.id))
            members->item(i)->setCheckState(Qt::Checked);
    }

    // An attach configuration naming the process by its executable.
    const std::string attachId = dialog.addAttach();
    ASSERT_FALSE(attachId.empty());
    EXPECT_TRUE(members->count() == 0) << "the member list belongs to compound configurations";
    process->setText(QStringLiteral("FrostEditor.exe"));
    dialog.accept();
    EXPECT_EQ(dialog.result(), QDialog::Accepted) << error->text().toStdString();

    const std::vector<run::RunConfiguration> edited = dialog.edited();
    ASSERT_EQ(edited.size(), 5U);
    const auto find = [&](const std::string& id) { return std::ranges::find(edited, id, &run::RunConfiguration::id); };
    ASSERT_NE(find(server.id), edited.end());
    const auto& serverTasks = find(server.id)->beforeLaunchTasks;
    ASSERT_EQ(serverTasks.size(), 2U);
    EXPECT_EQ(serverTasks[0], server.beforeLaunchTasks[0]) << "the unchanged task kept its working directory";
    EXPECT_EQ(serverTasks[1].program, MemTree::path("tools/check tool.exe"));
    EXPECT_EQ(serverTasks[1].arguments, std::vector<std::string>{"-q"});
    ASSERT_NE(find(compoundId), edited.end());
    EXPECT_EQ(find(compoundId)->type, run::RunType::Compound);
    EXPECT_EQ(find(compoundId)->name, "Editor and server");
    EXPECT_EQ(find(compoundId)->configurations, (std::vector<std::string>{"cmake.editor", server.id}));
    ASSERT_NE(find(attachId), edited.end());
    EXPECT_EQ(find(attachId)->type, run::RunType::Attach);
    EXPECT_EQ(find(attachId)->target, "FrostEditor.exe");
    // Stored and merged again, both are the user's configurations.
    set.replace(edited);
    EXPECT_NE(set.find(compoundId), nullptr);
    EXPECT_NE(set.find(attachId), nullptr);
    EXPECT_FALSE(set.isGenerated(compoundId));
}

// RUN-5: a compound runs its native members, each in its own tab through the RunManager; it is
// checked before anything starts; an attach configuration does not run.
TEST(RunControllerConfigurations, ACompoundRunsItsMembersAndAttachConfigurationsDoNotRun) {
    io::ScriptedProcessLauncher launcher;
    auto manager = run::RunManager::create(launcher);
    RunToolWindow window;
    RunControllerHooks hooks;
    int cmakeRuns = 0;
    hooks.generatedConfigurations = [] {
        return std::vector<run::RunConfiguration>{cmakeTarget("editor"), cmakeTarget("runtime")};
    };
    hooks.saveConfigurations = [](const std::vector<run::RunConfiguration>&) -> Result<void> { return {}; };
    hooks.alternateRun = [&]() -> Result<build::RunId> {
        ++cmakeRuns;
        return build::RunId{1000};
    };
    std::vector<QString> errors;
    hooks.reportError = [&](const QString& message) { errors.push_back(message); };
    RunController controller(&window, std::move(hooks));
    controller.setRunManager(manager.get());
    controller.refreshStartupCombo();

    std::vector<run::RunConfiguration> edited = controller.configurations().all();
    run::RunConfiguration server = run::nativeRunConfiguration("Server", MemTree::path("bin/server.exe"), edited);
    edited.push_back(server);
    run::RunConfiguration client = run::nativeRunConfiguration("Client", MemTree::path("bin/client.exe"), edited);
    client.arguments = {"--connect"};
    edited.push_back(client);
    run::RunConfiguration both;
    both.id = "compound.both";
    both.name = "Both";
    both.type = run::RunType::Compound;
    both.configurations = {server.id, client.id, "cmake.editor"};
    edited.push_back(both);
    run::RunConfiguration twoTargets = both;
    twoTargets.id = "compound.targets";
    twoTargets.name = "Two targets";
    twoTargets.configurations = {"cmake.editor", "cmake.runtime"};
    edited.push_back(twoTargets);
    run::RunConfiguration dangling = both;
    dangling.id = "compound.dangling";
    dangling.name = "Dangling";
    dangling.configurations = {server.id, "native.gone"};
    edited.push_back(dangling);
    run::RunConfiguration attach;
    attach.id = "attach.editor";
    attach.name = "Attach to the editor";
    attach.type = run::RunType::Attach;
    attach.target = "FrostEditor.exe";
    edited.push_back(attach);
    ASSERT_TRUE(controller.replaceConfigurations(edited).has_value());

    // Refused before anything starts: two CMake targets, a missing member, an attach configuration.
    auto refused = controller.runConfiguration(twoTargets.id);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message().find("one target at a time"), std::string::npos) << refused.error().message();
    refused = controller.runConfiguration(dangling.id);
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message().find("native.gone"), std::string::npos) << refused.error().message();
    ASSERT_TRUE(controller.selectConfiguration(attach.id).has_value());
    EXPECT_FALSE(controller.canRun());
    refused = controller.run();
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::Unsupported);
    EXPECT_NE(refused.error().message().find("Debug"), std::string::npos);
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_EQ(cmakeRuns, 0);

    // The compound: both native programs start (each in its tab) and the CMake target's runner runs.
    launcher.expect({.expectExecutable = server.program, .exitCode = 0, .stdoutBytes = "server up\n"});
    launcher.expect({.expectExecutable = client.program, .exitCode = 0, .stdoutBytes = "client done\n"});
    ASSERT_TRUE(controller.selectConfiguration(both.id).has_value());
    EXPECT_TRUE(controller.canRun());
    ASSERT_TRUE(controller.run().has_value());
    EXPECT_EQ(cmakeRuns, 1);
    RunConsole* serverTab = nullptr;
    RunConsole* clientTab = nullptr;
    ASSERT_TRUE(spinUntil([&] {
        serverTab = window.consoleFor(QStringLiteral("config:") + QString::fromStdString(server.id));
        clientTab = window.consoleFor(QStringLiteral("config:") + QString::fromStdString(client.id));
        return serverTab && clientTab && serverTab->text().contains(QStringLiteral("exit code 0")) &&
               clientTab->text().contains(QStringLiteral("exit code 0"));
    }));
    EXPECT_NE(serverTab, clientTab);
    EXPECT_TRUE(serverTab->text().contains(QStringLiteral("server up"))) << serverTab->text().toStdString();
    EXPECT_TRUE(clientTab->text().contains(QStringLiteral("client done"))) << clientTab->text().toStdString();
    ASSERT_EQ(launcher.launches().size(), 2U);
    EXPECT_EQ(launcher.launches()[1].spec.arguments, std::vector<std::string>{"--connect"});
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(manager->waitForIdle(30s));
}

// A native configuration through the RunManager into its own tab; the combo lists the configurations.
TEST(RunControllerConfigurations, ANativeConfigurationRunsIntoItsTab) {
    io::ScriptedProcessLauncher launcher;
    auto manager = run::RunManager::create(launcher);
    RunToolWindow window;
    std::vector<run::RunConfiguration> saved;
    RunControllerHooks hooks;
    hooks.generatedConfigurations = [] { return std::vector<run::RunConfiguration>{cmakeTarget("editor")}; };
    hooks.saveConfigurations = [&](const std::vector<run::RunConfiguration>& stored) -> Result<void> {
        saved = stored;
        return {};
    };
    RunController controller(&window, std::move(hooks));
    controller.setRunManager(manager.get());
    QComboBox combo;
    controller.bindStartupCombo(&combo);
    controller.refreshStartupCombo();
    ASSERT_EQ(controller.configurations().all().size(), 1U);
    EXPECT_GE(combo.findText(QStringLiteral("editor")), 0);

    run::RunConfiguration tool =
        run::nativeRunConfiguration("Tool", MemTree::path("bin/tool.exe"), controller.configurations().all());
    tool.arguments = {"--flag"};
    std::vector<run::RunConfiguration> edited = controller.configurations().all();
    edited.push_back(tool);
    ASSERT_TRUE(controller.replaceConfigurations(edited).has_value());
    ASSERT_EQ(saved.size(), 1U) << "only the native configuration is stored";
    EXPECT_EQ(saved.front().id, tool.id);
    EXPECT_GE(combo.findText(QStringLiteral("Tool")), 0);

    launcher.expect({.expectExecutable = tool.program, .exitCode = 5, .stdoutBytes = "tool output\n"});
    ASSERT_TRUE(controller.selectConfiguration(tool.id).has_value());
    EXPECT_EQ(combo.currentText(), QStringLiteral("Tool"));
    ASSERT_TRUE(controller.run().has_value());
    RunConsole* tab = nullptr;
    ASSERT_TRUE(spinUntil([&] {
        tab = window.consoleFor(QStringLiteral("config:") + QString::fromStdString(tool.id));
        return tab && tab->text().contains(QStringLiteral("Process finished with exit code 5"));
    })) << (tab ? tab->text().toStdString() : std::string("no tab"));
    EXPECT_TRUE(tab->text().contains(QStringLiteral("tool output\n"))) << tab->text().toStdString();
    ASSERT_EQ(launcher.launches().size(), 1U);
    EXPECT_EQ(launcher.launches()[0].spec.arguments, std::vector<std::string>{"--flag"});
    EXPECT_TRUE(manager->waitForIdle(30s));
    EXPECT_FALSE(controller.running());
}

TEST(RunConfigurationsDialog, LuaScriptAndReplCopyExplicitInterpreterWithoutChangingAnalysisRuntime) {
    run::RunConfigurationSet set;
    RunConfigurationsDialog dialog(set, "");
    const auto interpreter = MemTree::path("sdk/lua54/lua");
    const auto script = MemTree::path("project/main.lua");
    dialog.setLuaDefaults(interpreter, script, MemTree::path("project"));
    ASSERT_FALSE(dialog.addLuaScript().empty());
    auto* program = child<QLineEdit>(dialog, "runConfigurationProgram");
    auto* target = child<QLineEdit>(dialog, "runConfigurationLuaScript");
    auto* flags = child<QLineEdit>(dialog, "runConfigurationInterpreterArguments");
    ASSERT_NE(program, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(flags, nullptr);
    EXPECT_EQ(program->text().toStdString(), interpreter);
    EXPECT_EQ(target->text().toStdString(), script);
    flags->setText(QStringLiteral("-E -e \"print('startup')\""));
    auto configurations = dialog.edited();
    ASSERT_EQ(configurations.size(), 1U);
    EXPECT_EQ(configurations.front().type, run::RunType::LuaScript);
    EXPECT_EQ(configurations.front().interpreterArguments, (std::vector<std::string>{"-E", "-e", "print('startup')"}));
    const auto copy = dialog.copySelected();
    ASSERT_FALSE(copy.empty());
    configurations = dialog.edited();
    EXPECT_EQ(configurations.back().type, run::RunType::LuaScript);
    EXPECT_EQ(configurations.back().target, script);
    EXPECT_EQ(configurations.back().program, interpreter);
    ASSERT_FALSE(dialog.addLuaRepl().empty());
    configurations = dialog.edited();
    const auto& repl = configurations.back();
    EXPECT_EQ(repl.type, run::RunType::LuaRepl);
    EXPECT_EQ(repl.program, interpreter);
    EXPECT_TRUE(repl.target.empty());
    EXPECT_TRUE(repl.arguments.empty());
    EXPECT_TRUE(run::validateRunConfiguration(repl));
}

} // namespace
} // namespace frost::ui
