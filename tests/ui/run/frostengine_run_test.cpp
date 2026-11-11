// SPDX-License-Identifier: Apache-2.0
// RUN-11 UI roundtrip and lifecycle contracts. Scripted processes exercise the UI relay, private
// credential-bearing startup output, trust refusal and owned Stop; they do not claim real session acceptance.

#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/ui/run_configurations_dialog.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/run_tool_window.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QLineEdit>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace frost::ui {
namespace {
using namespace std::chrono_literals;

std::string path(std::string_view leaf) {
    const std::string root = io::kNativePathStyle == io::PathStyle::Windows ? "C:/run11" : "/run11";
    return io::normalizePath(root + "/" + std::string(leaf)).value_or("");
}
bool spinUntil(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(2ms);
    }
    return true;
}
run::RunConfiguration engineConfiguration() {
    run::RunConfiguration configuration;
    configuration.id = "editor.Debug";
    configuration.name = "Editor Debug";
    configuration.type = run::RunType::FrostEditor;
    configuration.program = path("Debug/FrostEditor.exe");
    configuration.engineProjectPath = path("My game/Game.frostproj");
    return configuration;
}

TEST(FrostEngineRunUi, AddEditCopyAndStorePreserveEditorAndPlayerTypes) {
    run::RunConfigurationSet set;
    RunConfigurationsDialog dialog(set, "");
    const auto editor = dialog.addFrostEditor();
    ASSERT_FALSE(editor.empty());
    auto* program = dialog.findChild<QLineEdit*>(QStringLiteral("runConfigurationProgram"));
    auto* project = dialog.findChild<QLineEdit*>(QStringLiteral("runConfigurationEngineProject"));
    ASSERT_NE(program, nullptr);
    ASSERT_NE(project, nullptr);
    EXPECT_FALSE(program->isReadOnly());
    program->setText(QString::fromStdString(path("Debug/FrostEditor.exe")));
    project->setText(QString::fromStdString(path("My game/Game.frostproj")));
    const auto copy = dialog.copySelected();
    ASSERT_FALSE(copy.empty());
    const auto player = dialog.addFrostPlayer();
    ASSERT_FALSE(player.empty());
    program->setText(QString::fromStdString(path("Release/FrostRuntime.exe")));
    project->setText(QString::fromStdString(path("My game")));
    dialog.accept();
    ASSERT_EQ(dialog.result(), QDialog::Accepted);
    const auto edited = dialog.edited();
    ASSERT_EQ(edited.size(), 3U);
    EXPECT_EQ(edited[0].type, run::RunType::FrostEditor);
    EXPECT_EQ(edited[1].type, run::RunType::FrostEditor);
    EXPECT_EQ(edited[1].engineProjectPath, edited[0].engineProjectPath);
    EXPECT_EQ(edited[2].type, run::RunType::FrostPlayer);
    EXPECT_EQ(edited[2].program, path("Release/FrostRuntime.exe"));
    auto parsed = run::parseRunConfigurations(run::serializeRunConfigurations(edited));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, edited);
}

TEST(FrostEngineRunUi, ProcessIdentityAndPrivateOutputReachSessionHooksAndStopOwnsChild) {
    io::ScriptedProcessLauncher launcher;
    auto configuration = engineConfiguration();
    configuration.beforeLaunchTasks = {{.program = path("prepare.exe")}};
    launcher.expect({.expectExecutable = path("prepare.exe"), .stdoutBytes = "task-not-an-endpoint\n"});
    launcher.expect({.expectExecutable = configuration.program,
                     .stdoutBytes = "startup-secret\n",
                     .stderrBytes = "private stderr\n",
                     .neverExits = true});
    auto manager = run::RunManager::create(launcher);
    RunToolWindow window;
    run::RunId acceptedId = 0;
    run::RunId startedId = 0;
    run::RunId outputId = 0;
    std::string privateOutput;
    bool finished = false;
    RunControllerHooks hooks;
    hooks.prepareEngineConfiguration = [](const auto& original) -> Result<run::RunConfiguration> {
        auto prepared = original;
        prepared.arguments = {"--automation-port", "0"};
        return prepared;
    };
    hooks.engineLaunchAdmissionFor = [](const auto&) -> std::function<Result<void>()> {
        return []() -> Result<void> { return {}; };
    };
    hooks.engineLaunchPlanFor = [](const auto&) -> std::function<Result<run::LaunchPlan>(run::LaunchPlan)> {
        return [](run::LaunchPlan plan) -> Result<run::LaunchPlan> {
            plan.arguments[1] = path("physical project/Game.frostproj");
            return plan;
        };
    };
    hooks.engineRunAccepted = [&](auto id, const auto&) { acceptedId = id; };
    hooks.engineProcessStarted = [&](const auto& status) { startedId = status.id; };
    hooks.engineProcessOutput = [&](auto id, auto, std::string_view bytes) {
        outputId = id;
        privateOutput += bytes;
    };
    hooks.engineProcessFinished = [&](const auto&) { finished = true; };
    RunController controller(&window, std::move(hooks));
    controller.setRunManager(manager.get());
    controller.setStoredConfigurations({configuration});
    const auto started = controller.runConfiguration(configuration.id);
    ASSERT_TRUE(started.has_value()) << started.error().describe();
    EXPECT_EQ(acceptedId, *started) << "accepted identity arrives before queued lifecycle events";
    ASSERT_TRUE(spinUntil([&] { return startedId != 0 && privateOutput.find("private stderr") != std::string::npos; }));
    EXPECT_EQ(startedId, *started);
    EXPECT_EQ(outputId, *started);
    EXPECT_EQ(privateOutput.find("task-not-an-endpoint"), std::string::npos);
    auto* console = window.consoleFor(QStringLiteral("config:editor.Debug"));
    ASSERT_NE(console, nullptr);
    EXPECT_FALSE(console->text().contains(QStringLiteral("startup-secret")));
    EXPECT_FALSE(console->text().contains(QStringLiteral("private stderr")));
    EXPECT_TRUE(console->text().contains(QStringLiteral("Engine Console")));
    controller.stop();
    ASSERT_TRUE(spinUntil([&] { return finished; }));
    EXPECT_TRUE(manager->waitForIdle(30s));
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 2U);
    EXPECT_TRUE(launches[1].terminated);
    EXPECT_EQ(
        launches[1].spec.arguments,
        (std::vector<std::string>{"--project", path("physical project/Game.frostproj"), "--automation-port", "0"}));
    EXPECT_TRUE(launches[1].spec.killOnClose);
    EXPECT_EQ(controller.configurations().all().front(), configuration) << "transient session args are never stored";
}

TEST(FrostEngineRunUi, MissingSessionOrRefusedTrustStartsNothing) {
    io::ScriptedProcessLauncher launcher;
    auto manager = run::RunManager::create(launcher);
    const auto configuration = engineConfiguration();
    RunController missing(nullptr);
    missing.setRunManager(manager.get());
    missing.setStoredConfigurations({configuration});
    const auto unsupported = missing.runConfiguration(configuration.id);
    ASSERT_FALSE(unsupported.has_value());
    EXPECT_EQ(unsupported.error().code(), ErrorCode::Unsupported);

    bool prepared = false;
    RunControllerHooks hooks;
    hooks.mayRunConfiguration = [](const auto&) -> Result<void> {
        return fail(ErrorCode::PermissionDenied, "untrusted");
    };
    hooks.prepareEngineConfiguration = [&](const auto& value) -> Result<run::RunConfiguration> {
        prepared = true;
        return value;
    };
    RunController untrusted(nullptr, std::move(hooks));
    untrusted.setRunManager(manager.get());
    untrusted.setStoredConfigurations({configuration});
    const auto refused = untrusted.runConfiguration(configuration.id);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::PermissionDenied);
    EXPECT_FALSE(prepared);
    EXPECT_TRUE(launcher.launches().empty());
}

TEST(FrostEngineRunUi, PhysicalProjectResolutionCannotReplaceExecutableOrExtraArguments) {
    for (const bool replaceProgram : {true, false}) {
        io::ScriptedProcessLauncher launcher;
        auto manager = run::RunManager::create(launcher);
        const auto configuration = engineConfiguration();
        std::optional<run::RunResult> finished;
        RunControllerHooks hooks;
        hooks.prepareEngineConfiguration = [](const auto& value) -> Result<run::RunConfiguration> { return value; };
        hooks.engineLaunchAdmissionFor = [](const auto&) -> std::function<Result<void>()> {
            return []() -> Result<void> { return {}; };
        };
        hooks.engineLaunchPlanFor =
            [replaceProgram](const auto&) -> std::function<Result<run::LaunchPlan>(run::LaunchPlan)> {
            return [replaceProgram](run::LaunchPlan plan) -> Result<run::LaunchPlan> {
                plan.arguments[1] = path("physical project/Game.frostproj");
                if (replaceProgram) {
                    plan.program = path("unselected.exe");
                } else {
                    plan.arguments.push_back("--unselected-option");
                }
                return plan;
            };
        };
        hooks.engineProcessFinished = [&](const auto& value) { finished = value; };
        RunController controller(nullptr, std::move(hooks));
        controller.setRunManager(manager.get());
        controller.setStoredConfigurations({configuration});
        ASSERT_TRUE(controller.runConfiguration(configuration.id).has_value());
        ASSERT_TRUE(spinUntil([&] { return finished.has_value(); }));
        ASSERT_TRUE(finished->error.has_value());
        EXPECT_EQ(finished->error->code(), ErrorCode::InvalidArgument);
        EXPECT_TRUE(launcher.launches().empty());
        EXPECT_EQ(controller.configurations().all().front(), configuration);
        EXPECT_TRUE(manager->waitForIdle(30s));
    }
}

} // namespace
} // namespace frost::ui
