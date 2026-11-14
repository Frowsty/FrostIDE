// SPDX-License-Identifier: Apache-2.0
// Workbench typed mixed-debug routing/lifetime regressions with process and DAP doubles.
// These exercise public actions and cancellation. Real LLDB/engine authentication and same-pause
// behavior belong to the mandatory mixed corpus journey.
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"

#include "debug/debug_test_util.hpp"
#include "workbench_rig.hpp"
#include <QPushButton>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
namespace frost::app {
namespace {
using namespace std::chrono_literals;
class MixedPreparationLauncher final : public io::IProcessLauncher {
public:
    io::ScriptedProcessLauncher processes, cli;
    Result<std::unique_ptr<io::IProcess>> launch(const io::ProcessSpec& spec) override {
        if (!spec.arguments.empty() &&
            (spec.arguments.front() == "project.info" || spec.arguments.front() == "assets.list" ||
             spec.arguments.front() == "schema.export")) {
            cli.expect({.launchError = Error(ErrorCode::Unsupported, "protocol fixture has no project CLI")});
            return cli.launch(spec);
        }
        return processes.launch(spec);
    }
    Result<std::unique_ptr<io::IAsyncProcess>> launchAsync(const io::ProcessSpec& spec,
                                                           const io::AsyncContext& context) override {
        return processes.launchAsync(spec, context);
    }
};
class WorkbenchEngineMixedTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory = std::make_unique<test::WorkDir>("workbench-mixed-routing");
        directory->write("first/Game.frostproj", "{}");
        directory->write("second/Other.frostproj", "{}");
#ifdef _WIN32
        const std::string binary = "bin/FrostEditor.exe";
        task = directory->join("prepare/task.exe");
#else
        const std::string binary = "bin/FrostEditor";
        task = directory->join("prepare/task");
#endif
        directory->write(binary, "process lifecycle fixture");
        program = directory->join(binary);
        root = directory->join("first");
        test::RigOptions options;
        options.disableProjectEvaluation = true;
        options.launcher = &launcher;
        options.workbench.frostEditorPath = program;
        options.workbench.engineConfigDirectory = directory->join("private-engine-config");
        options.workbench.engineAutomationDirectory = directory->join("automation");
        // The private native launcher must refuse this supplied factory if preparation finishes.
        // Keeping the task pending tests the actual composed route before any native tool starts.
        options.workbench.debugAdapterFactory =
            [this](debug::DapHandlers) -> Result<std::unique_ptr<debug::IDebugAdapterClient>> {
            ++customFactoryCalls;
            return fail(ErrorCode::Internal, "unexpected custom native factory invocation");
        };
        rig = std::make_unique<test::WorkbenchRig>(*directory, std::move(options));
        ASSERT_TRUE(rig->trust->trust(root));
        ASSERT_TRUE(rig->workbench->openPath(root));
        ASSERT_TRUE(test::waitFor(
            [&] {
                return std::ranges::any_of(rig->workbench->runController()->configurations().all(),
                                           [](const auto& c) { return c.type == run::RunType::FrostEditor; });
            },
            5s));
        auto edited = rig->workbench->runController()->configurations().all();
        auto member = std::ranges::find(edited, run::RunType::FrostEditor, &run::RunConfiguration::type);
        ASSERT_NE(member, edited.end());
        member->engineDebugMode = run::EngineDebugMode::NativeAndLua;
        member->beforeLaunchTasks = {{task, {"held preparation"}, root}};
        selected = *member;
        ASSERT_TRUE(rig->workbench->runController()->replaceConfigurations(edited));
        ASSERT_TRUE(rig->workbench->runController()->selectConfiguration(selected.id));
        saved = rig->workbench->runController()->configurations().stored();
    }
    void TearDown() override { rig.reset(); }
    bool startHeld(bool primaryController = false) {
        launcher.processes.expect({.expectExecutable = task, .neverExits = true});
        if (primaryController) {
            auto accepted = rig->workbench->debugController()->debug();
            EXPECT_TRUE(accepted) << (accepted ? "" : accepted.error().message());
            if (!accepted)
                return false;
        } else {
            const auto invoked = rig->window->invoke("run.debug");
            EXPECT_EQ(invoked, input::InvokeResult::Ran);
            if (invoked != input::InvokeResult::Ran)
                return false;
        }
        return test::waitFor(
            [&] {
                const auto records = launcher.processes.launches();
                return !records.empty() && records.front().launched;
            },
            5s);
    }
    bool cancelled() const {
        const auto records = launcher.processes.launches();
        const auto state = rig->workbench->engineState();
        return !records.empty() && records.front().terminated && state.boolAt("launchReserved") != true &&
               !rig->workbench->debugSessions()->hasPendingWork();
    }
    void checkNoHostLaunch() {
        const auto records = launcher.processes.launches();
        ASSERT_EQ(records.size(), 1U);
        EXPECT_EQ(records.front().spec.executable, task);
        EXPECT_EQ(records.front().spec.arguments, std::vector<std::string>{"held preparation"});
        EXPECT_TRUE(records.front().terminated);
        EXPECT_EQ(customFactoryCalls.load(), 0);
    }
    void checkMixedPending() {
        const auto state = rig->workbench->engineState();
        EXPECT_EQ(state.boolAt("launchReserved"), true);
        EXPECT_EQ(state.unsignedAt("ownedRunId"), 0U);
        EXPECT_EQ(state.unsignedAt("ownedPid"), 0U);
        EXPECT_TRUE(rig->workbench->anyDebugSessionActive());
        EXPECT_TRUE(rig->workbench->debugSessions()->hasPendingWork());
        EXPECT_FALSE(rig->workbench->debugController()->debugging());
        EXPECT_EQ(rig->workbench->runController()->configurations().stored(), saved);
    }
    std::unique_ptr<test::WorkDir> directory;
    MixedPreparationLauncher launcher;
    std::unique_ptr<test::WorkbenchRig> rig;
    std::atomic<unsigned> customFactoryCalls{0};
    std::string root, program, task;
    run::RunConfiguration selected;
    std::vector<run::RunConfiguration> saved;
};

TEST_F(WorkbenchEngineMixedTest, TypedDebugActionUsesPhasedPreparationAndStopAllCancelsIt) {
    ASSERT_TRUE(startHeld());
    checkMixedPending();
    rig->window->toolWindows().show("debug");
    auto* pane = rig->window->toolWindows().content("debug");
    ASSERT_NE(pane, nullptr);
    auto* stop = pane->findChild<QPushButton*>(QStringLiteral("debugStopAll"));
    ASSERT_NE(stop, nullptr);
    ASSERT_TRUE(stop->isEnabled());
    stop->click();
    ASSERT_TRUE(test::waitFor([&] { return cancelled(); }, 5s));
    EXPECT_FALSE(rig->workbench->anyDebugSessionActive());
    EXPECT_EQ(rig->workbench->runController()->configurations().stored(), saved);
    checkNoHostLaunch();
}
TEST_F(WorkbenchEngineMixedTest, PrimaryDebugEntryReachesTypedMixedPreparation) {
    ASSERT_TRUE(startHeld(true));
    checkMixedPending();
    rig->workbench->debugSessions()->stopAll();
    ASSERT_TRUE(test::waitFor([&] { return cancelled(); }, 5s));
    checkNoHostLaunch();
}
TEST_F(WorkbenchEngineMixedTest, ProjectSwitchCancelsOldPreparationBeforeOpeningTheNextProject) {
    ASSERT_TRUE(startHeld());
    checkMixedPending();
    const auto next = directory->join("second");
    ASSERT_TRUE(rig->trust->trust(next));
    const auto switched = rig->workbench->openPath(next);
    ASSERT_TRUE(test::waitFor([&] { return cancelled(); }, 5s));
    // The existing project guard may require the caller to retry after asynchronous task cleanup.
    if (!switched)
        ASSERT_TRUE(rig->workbench->openPath(next));
    EXPECT_EQ(rig->workbench->projectRoot(), next);
    EXPECT_FALSE(rig->workbench->anyDebugSessionActive());
    EXPECT_EQ(rig->workbench->engineState().boolAt("connected"), false);
    checkNoHostLaunch();
}
TEST_F(WorkbenchEngineMixedTest, ShutdownCancelsMixedTaskAndDetachesAnOrdinaryExternalSession) {
    debug::FakeDebugAdapter external;
    debug::scriptProgram(external, debug::FakeProgram{.runsForever = true});
    external.on("attach", [](const rpc::Json&) {
        return debug::FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json::object()}}};
    });
    auto attached = rig->workbench->debugSessions()->attachTarget(
        debug::DebugTarget{
            .name = "External host", .program = directory->join("first/External.exe"), .trustRoots = {root}},
        debug::test::fakeFactory(external), rpc::Json::object().set("type", "lldb-dap").set("pid", 4321));
    ASSERT_TRUE(attached);
    ASSERT_TRUE(test::waitFor(
        [&] {
            auto* child = rig->workbench->debugSessions()->controller(*attached);
            return child && child->configured();
        },
        5s));
    ASSERT_TRUE(startHeld());
    rig->workbench->shutdown();
    ASSERT_TRUE(test::waitFor([&] { return cancelled() && !external.argumentsOf("disconnect").empty(); }, 5s));
    EXPECT_EQ(external.argumentsOf("disconnect").front().boolAt("terminateDebuggee"), false);
    EXPECT_TRUE(external.argumentsOf("terminate").empty());
    EXPECT_FALSE(rig->workbench->startEngineLuaDebug());
    checkNoHostLaunch();
}
TEST_F(WorkbenchEngineMixedTest, WindowDetachCancelsPendingPreparationBeforeProvidersAreDestroyed) {
    ASSERT_TRUE(startHeld());
    rig->window.reset();
    ASSERT_TRUE(test::waitFor(
        [&] {
            const auto records = launcher.processes.launches();
            return !records.empty() && records.front().terminated;
        },
        5s));
    EXPECT_EQ(rig->workbench->engineState().boolAt("available"), false);
    checkNoHostLaunch();
}
} // namespace
} // namespace frost::app
