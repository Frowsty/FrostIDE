// SPDX-License-Identifier: Apache-2.0
/*
 * Tests for RUN-3's widgets in frost::ui_run, offscreen (no window is shown): the Run tool window
 * v2's tabs (reuse by key, the empty first tab, the running title, closing stops the run), the
 * argument and environment text of Run/Debug Configurations, the dialog itself (editing a native
 * configuration, what a generated cmake-target and msbuild configuration allow, the RUN-5 compound
 * section, validation on OK), and RunController running a native configuration through a
 * RunManager over the scripted launcher into its own tab, with the configuration combo.
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

TEST(RunConfigurationsDialog, CompoundRunsAreDisabledNamingRunFive) {
    run::RunConfigurationSet set;
    RunConfigurationsDialog dialog(set, "", false);
    auto* group = child<QWidget>(dialog, "compoundRunGroup");
    auto* launch = child<QPushButton>(dialog, "compoundLaunchButton");
    ASSERT_TRUE(group && launch);
    EXPECT_FALSE(group->isEnabled());
    EXPECT_FALSE(launch->isEnabled());
    EXPECT_TRUE(launch->toolTip().contains(QStringLiteral("RUN-5")));
    // No workspace: nowhere to store a native configuration.
    EXPECT_FALSE(child<QPushButton>(dialog, "runConfigurationAdd")->isEnabled());
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

} // namespace
} // namespace frost::ui
