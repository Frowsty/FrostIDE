// SPDX-License-Identifier: Apache-2.0
// RUN-11 model/store/CLI and final child admission regression tests. Scripted launches test routing,
// not engine acceptance; the real run/session journey belongs to the application integration gate.

#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/rpc/json.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_manager.hpp"

#include "run/run_collector.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace frost::run {
namespace {
using namespace std::chrono_literals;

std::string path(std::string_view leaf) {
    const std::string root = io::kNativePathStyle == io::PathStyle::Windows ? "C:/run11" : "/run11";
    return io::normalizePath(root + "/" + std::string(leaf)).value_or("");
}

RunConfiguration engineConfiguration(RunType type) {
    RunConfiguration configuration;
    configuration.id = std::string(toString(type));
    configuration.name = "Engine selected Debug build";
    configuration.type = type;
    configuration.program = path(type == RunType::FrostEditor ? "Debug/FrostEditor.exe" : "Debug/FrostRuntime.exe");
    configuration.engineProjectPath = path("game with spaces/Game.frostproj");
    configuration.arguments = {"--offscreen"};
    return configuration;
}

TEST(FrostEngineRun, StoreVersionThreeKeepsBothTypesAndAllLaunchSettings) {
    auto editor = engineConfiguration(RunType::FrostEditor);
    auto player = engineConfiguration(RunType::FrostPlayer);
    player.arguments = {"--headless", "--frames", "2"};
    player.workingDirectory = path("game with spaces");
    player.environment = {{"FROST_TEST", "value"}};
    player.beforeLaunchTasks = {{.program = path("tools/prepare.exe"), .arguments = {"two words"}}};
    const std::vector<RunConfiguration> values{editor, player};
    const std::string text = serializeRunConfigurations(values);
    const auto document = rpc::parseJson(text);
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(document->integerAt("version"), 3);
    auto parsed = parseRunConfigurations(text);
    ASSERT_TRUE(parsed.has_value()) << parsed.error().describe();
    EXPECT_EQ(*parsed, values);
    EXPECT_EQ(parseRunType("frost-editor"), RunType::FrostEditor);
    EXPECT_EQ(parseRunType("frost-player"), RunType::FrostPlayer);

    RunConfiguration native;
    native.id = "ordinary";
    native.name = "Ordinary";
    native.program = path("ordinary.exe");
    auto ordinary = rpc::parseJson(serializeRunConfigurations(std::vector{native}));
    ASSERT_TRUE(ordinary.has_value());
    EXPECT_EQ(ordinary->integerAt("version"), 1);
    native.beforeLaunchTasks = {{.program = path("prepare.exe")}};
    ordinary = rpc::parseJson(serializeRunConfigurations(std::vector{native}));
    ASSERT_TRUE(ordinary.has_value());
    EXPECT_EQ(ordinary->integerAt("version"), 2);
}

TEST(FrostEngineRun, RequiresExplicitExecutableAndAbsoluteProjectAndCannotOverrideProject) {
    auto configuration = engineConfiguration(RunType::FrostEditor);
    ASSERT_TRUE(validateRunConfiguration(configuration).has_value());
    configuration.engineProjectPath = "relative/Game.frostproj";
    EXPECT_FALSE(validateRunConfiguration(configuration).has_value());
    configuration.engineProjectPath = path("Game.frostproj");
    configuration.program.clear();
    EXPECT_FALSE(validateRunConfiguration(configuration).has_value());
    configuration.program = path("FrostEditor.exe");
    configuration.arguments = {"--project", path("other")};
    EXPECT_FALSE(validateRunConfiguration(configuration).has_value());
    configuration.arguments = {"--project=" + path("other")};
    EXPECT_FALSE(validateRunConfiguration(configuration).has_value());
    configuration.arguments.clear();
    configuration.beforeLaunch = BeforeLaunch::Build;
    EXPECT_FALSE(validateRunConfiguration(configuration).has_value());
    configuration.beforeLaunch = BeforeLaunch::None;
    configuration.type = RunType::Native;
    EXPECT_FALSE(validateRunConfiguration(configuration).has_value());
}

TEST(FrostEngineRun, BothHostsReceiveRealProjectSwitchWithoutShellQuoting) {
    for (const auto type : {RunType::FrostEditor, RunType::FrostPlayer}) {
        auto configuration = engineConfiguration(type);
        auto plan = launchPlanFor(configuration);
        ASSERT_TRUE(plan.has_value()) << plan.error().describe();
        EXPECT_EQ(plan->program, configuration.program);
        EXPECT_EQ(plan->arguments,
                  (std::vector<std::string>{"--project", configuration.engineProjectPath, "--offscreen"}));
        EXPECT_EQ(plan->workingDirectory, std::string(io::parentPath(configuration.program)));
    }
}

TEST(FrostEngineRun, FinalAdmissionRefusesQueuedChildWithoutStartingAnyProcess) {
    io::ScriptedProcessLauncher launcher;
    test::RunCollector collector;
    auto handlers = collector.handlers();
    handlers.launchAdmission = []() -> Result<void> { return fail(ErrorCode::PermissionDenied, "trust revoked"); };
    auto manager = RunManager::create(launcher);
    auto configuration = engineConfiguration(RunType::FrostEditor);
    ASSERT_TRUE(manager->start(configuration, std::move(handlers)).has_value());
    const auto result = collector.waitFinished(30s).result;
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->error.has_value());
    EXPECT_EQ(result->error->code(), ErrorCode::PermissionDenied);
    EXPECT_FALSE(result->ran());
    EXPECT_TRUE(launcher.launches().empty());
}

TEST(FrostEngineRun, FinalAdmissionRunsAgainBetweenTaskAndEngineProcess) {
    io::ScriptedProcessLauncher launcher;
    auto configuration = engineConfiguration(RunType::FrostPlayer);
    configuration.beforeLaunchTasks = {{.program = path("prepare.exe")}};
    launcher.expect({.expectExecutable = configuration.beforeLaunchTasks.front().program});
    std::atomic<int> admissions = 0;
    test::RunCollector collector;
    auto handlers = collector.handlers();
    handlers.launchAdmission = [&]() -> Result<void> {
        if (++admissions > 1) {
            return fail(ErrorCode::PermissionDenied, "trust revoked after task");
        }
        return {};
    };
    auto manager = RunManager::create(launcher);
    ASSERT_TRUE(manager->start(configuration, std::move(handlers)).has_value());
    const auto result = collector.waitFinished(30s).result;
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->error.has_value());
    EXPECT_EQ(result->error->code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(admissions.load(), 2);
    ASSERT_EQ(launcher.launches().size(), 1U);
    EXPECT_EQ(launcher.launches().front().spec.executable, configuration.beforeLaunchTasks.front().program);
}

TEST(FrostEngineRun, WorkerResolutionPinsRootProjectAndReportsActualPlanWithoutChangingTasks) {
    io::ScriptedProcessLauncher launcher;
    auto configuration = engineConfiguration(RunType::FrostEditor);
    configuration.beforeLaunchTasks = {{.program = path("prepare.exe"), .arguments = {"unchanged task"}}};
    launcher.expect({.expectExecutable = configuration.beforeLaunchTasks.front().program});
    launcher.expect({.expectExecutable = configuration.program});
    test::RunCollector collector;
    auto handlers = collector.handlers();
    std::atomic<int> resolutions = 0;
    const auto caller = std::this_thread::get_id();
    handlers.transformLaunchPlan = [&](LaunchPlan plan) -> Result<LaunchPlan> {
        ++resolutions;
        EXPECT_NE(std::this_thread::get_id(), caller);
        plan.arguments[1] = path("physical/Game.frostproj");
        return plan;
    };
    auto manager = RunManager::create(launcher);
    ASSERT_TRUE(manager->start(configuration, std::move(handlers)).has_value());
    const auto result = collector.waitFinished(30s).result;
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->plan.has_value());
    EXPECT_EQ(resolutions.load(), 1);
    EXPECT_EQ(result->plan->arguments[1], path("physical/Game.frostproj"));
    ASSERT_EQ(launcher.launches().size(), 2U);
    EXPECT_EQ(launcher.launches()[0].spec.arguments, configuration.beforeLaunchTasks.front().arguments);
    EXPECT_EQ(launcher.launches()[1].spec.arguments[1], path("physical/Game.frostproj"));
    EXPECT_TRUE(manager->waitForIdle(30s));
}

TEST(FrostEngineRun, TrustRevocationDuringWorkerResolutionPreventsRootLaunch) {
    io::ScriptedProcessLauncher launcher;
    test::RunCollector collector;
    auto handlers = collector.handlers();
    std::atomic<bool> trusted = true;
    std::atomic<int> admissions = 0;
    handlers.launchAdmission = [&]() -> Result<void> {
        ++admissions;
        if (!trusted.load()) {
            return fail(ErrorCode::PermissionDenied, "trust revoked during project resolution");
        }
        return {};
    };
    handlers.transformLaunchPlan = [&](LaunchPlan plan) -> Result<LaunchPlan> {
        plan.arguments[1] = path("physical/Game.frostproj");
        trusted = false;
        return plan;
    };
    auto manager = RunManager::create(launcher);
    ASSERT_TRUE(manager->start(engineConfiguration(RunType::FrostEditor), std::move(handlers)).has_value());
    const auto result = collector.waitFinished(30s).result;
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->error.has_value());
    EXPECT_EQ(result->error->code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(admissions.load(), 2);
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_TRUE(manager->waitForIdle(30s));
}

TEST(FrostEngineRun, StopDuringWorkerAdmissionPreventsChildAfterAdmissionReturns) {
    io::ScriptedProcessLauncher launcher;
    test::RunCollector collector;
    auto handlers = collector.handlers();
    std::promise<void> entered;
    auto admissionEntered = entered.get_future();
    std::promise<void> release;
    auto continueAdmission = release.get_future();
    handlers.launchAdmission = [&]() -> Result<void> {
        entered.set_value();
        if (continueAdmission.wait_for(30s) != std::future_status::ready) {
            return fail(ErrorCode::Timeout, "test did not release worker admission");
        }
        return {};
    };
    auto manager = RunManager::create(launcher);
    const auto started = manager->start(engineConfiguration(RunType::FrostEditor), std::move(handlers));
    ASSERT_TRUE(started.has_value());
    const auto enteredStatus = admissionEntered.wait_for(30s);
    manager->stop(*started);
    release.set_value();
    ASSERT_EQ(enteredStatus, std::future_status::ready);
    const auto result = collector.waitFinished(30s).result;
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->stopped);
    EXPECT_TRUE(launcher.launches().empty());
    EXPECT_TRUE(manager->waitForIdle(30s));
}

} // namespace
} // namespace frost::run
