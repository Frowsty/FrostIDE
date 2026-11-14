// SPDX-License-Identifier: Apache-2.0
// RUN-11 Debug source/lifecycle seam tests with owned scripted processes. These do not claim real
// engine/DAP authentication acceptance; the pinned app journey must prove configurationDone/VM entry.
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/ui/run_controller.hpp"

#include "engine_run_debug.hpp"
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <gtest/gtest.h>

#include <algorithm>

namespace frost::app {
namespace {
using namespace std::chrono_literals;
bool spin(const std::function<bool()>& done) {
    const QDeadlineTimer deadline(5000);
    while (!done()) {
        if (deadline.hasExpired())
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}
std::string path(std::string_view file) {
    return io::normalizePath((io::kNativePathStyle == io::PathStyle::Windows ? "C:/run11-debug/" : "/run11-debug/") +
                             std::string(file))
        .value_or("");
}
run::RunConfiguration configuration(run::RunType type = run::RunType::FrostPlayer) {
    run::RunConfiguration c;
    c.id = "owned.Debug";
    c.name = "Owned engine Debug";
    c.type = type;
    c.program = path(type == run::RunType::FrostPlayer ? "FrostRuntime.exe" : "FrostEditor.exe");
    c.engineProjectPath = path("Game.frostproj");
    return c;
}
ui::RunControllerHooks runHooks() {
    ui::RunControllerHooks h;
    h.prepareEngineConfiguration = [](const auto& c) -> Result<run::RunConfiguration> { return c; };
    h.engineLaunchAdmissionFor = [](const auto&) -> std::function<Result<void>()> {
        return []() -> Result<void> { return {}; };
    };
    h.engineLaunchPlanFor = [](const auto&) -> std::function<Result<run::LaunchPlan>(run::LaunchPlan)> {
        return [](run::LaunchPlan p) -> Result<run::LaunchPlan> { return p; };
    };
    return h;
}
EngineLuaDebugOwner owner(std::string role) {
    EngineLuaDebugOwner o;
    o.identity.role = std::move(role);
    o.current = [] { return true; };
    return o;
}
TEST(EngineRunDebug, TransientPlayerWaitAndEditorRemovalPreserveSavedConfigurationAndArguments) {
    auto c = configuration();
    c.arguments = {"--headless"};
    c.environment = {{"frost_script_debug_wait", "old"}, {"OTHER", "kept"}};
    const auto saved = c;
    auto transient = engineDebugHostConfiguration(c);
    ASSERT_TRUE(transient);
    EXPECT_EQ(c, saved);
    EXPECT_EQ(transient->arguments, c.arguments);
    ASSERT_EQ(transient->environment.size(), 2u);
    EXPECT_EQ(transient->environment.back(), (run::EnvironmentEntry{"FROST_SCRIPT_DEBUG_WAIT", "1"}));
    c.type = run::RunType::FrostEditor;
    auto editor = engineDebugHostConfiguration(c);
    ASSERT_TRUE(editor);
    EXPECT_FALSE(editor->environment.back().value);
    c.type = run::RunType::Native;
    EXPECT_EQ(engineDebugHostConfiguration(c).error().code(), ErrorCode::InvalidArgument);
}
TEST(EngineRunDebug, WaitsExactOwnedRunBeforePlayerAttachAndCleansRealLaunchOnFailure) {
    io::ScriptedProcessLauncher launcher;
    launcher.expect({.neverExits = true});
    auto manager = run::RunManager::create(launcher);
    ui::RunController runs(nullptr, runHooks());
    runs.setRunManager(manager.get());
    const auto c = configuration();
    runs.setStoredConfigurations({c});
    bool ready = false, completed = false;
    int captures = 0, lua = 0;
    run::RunId captured = 0;
    std::optional<Error> error;
    EngineRunDebugHooks hooks;
    hooks.capture = [&](run::RunId id) -> Result<EngineLuaDebugOwner> {
        ++captures;
        captured = id;
        if (!ready)
            return fail(ErrorCode::Busy, "pending authentication input");
        return owner("player");
    };
    hooks.prepareLua = [&](bool play, std::stop_token, MultiSessionDebugCoordinator::Prepared done,
                           bool entry) -> Result<void> {
        ++lua;
        EXPECT_FALSE(play);
        EXPECT_TRUE(entry);
        done(fail(ErrorCode::Unsupported, "validator bootstrap refusal"));
        return {};
    };
    EngineRunDebugCoordinator debug(runs, *manager, std::move(hooks));
    ASSERT_TRUE(debug.prepare(c, {}, [&](Result<PreparedDebugLaunch> result) {
        completed = true;
        if (!result)
            error = result.error();
    }));
    ASSERT_TRUE(spin([&] { return captures > 0; }));
    EXPECT_EQ(lua, 0);
    ASSERT_TRUE(runs.configurationRun(c.id));
    EXPECT_EQ(captured, *runs.configurationRun(c.id));
    ready = true;
    ASSERT_TRUE(spin([&] { return completed && manager->activeRuns().empty(); }));
    ASSERT_TRUE(error);
    EXPECT_EQ(error->code(), ErrorCode::Unsupported);
    EXPECT_EQ(lua, 1);
    ASSERT_EQ(launcher.launches().size(), 1u);
    EXPECT_TRUE(launcher.launches().front().terminated);
    EXPECT_EQ(runs.configurations().stored().front(), c);
    const auto records = launcher.launches();
    const auto& env = records.front().spec.environment;
    EXPECT_NE(
        std::ranges::find(env, std::pair<std::string, std::optional<std::string>>{"FROST_SCRIPT_DEBUG_WAIT", "1"}),
        env.end());
}
TEST(EngineRunDebug, CancelStopsOnlyCapturedOwnedRunAndNeverAnUnrelatedRun) {
    io::ScriptedProcessLauncher launcher;
    launcher.expect({.neverExits = true});
    launcher.expect({.neverExits = true});
    auto manager = run::RunManager::create(launcher);
    auto unrelated = configuration();
    unrelated.id = "unrelated";
    unrelated.type = run::RunType::Native;
    auto other = manager->start(unrelated);
    ASSERT_TRUE(other);
    ui::RunController runs(nullptr, runHooks());
    runs.setRunManager(manager.get());
    EngineRunDebugHooks hooks;
    hooks.capture = [](run::RunId) -> Result<EngineLuaDebugOwner> { return fail(ErrorCode::Busy, "pending"); };
    hooks.prepareLua = [](bool, std::stop_token, auto, bool) -> Result<void> {
        ADD_FAILURE() << "DAP must wait for owned authentication";
        return fail(ErrorCode::Internal, "unexpected");
    };
    EngineRunDebugCoordinator debug(runs, *manager, std::move(hooks));
    std::stop_source stop;
    bool completed = false;
    ASSERT_TRUE(debug.prepare(configuration(), stop.get_token(), [&](Result<PreparedDebugLaunch> result) {
        completed = true;
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    }));
    ASSERT_TRUE(spin([&] { return launcher.launches().size() == 2; }));
    stop.request_stop();
    ASSERT_TRUE(spin([&] { return completed && manager->activeRuns().size() == 1; }));
    ASSERT_TRUE(manager->status(*other));
    EXPECT_FALSE(launcher.launches().front().terminated);
    EXPECT_TRUE(launcher.launches().back().terminated);
    manager->stop(*other);
    EXPECT_TRUE(manager->waitForIdle(5s));
}
TEST(EngineRunDebug, AbandoningPreparedEditorStopsItsUnconsumedHost) {
    io::ScriptedProcessLauncher launcher;
    launcher.expect({.neverExits = true});
    auto manager = run::RunManager::create(launcher);
    ui::RunController runs(nullptr, runHooks());
    runs.setRunManager(manager.get());
    EngineRunDebugHooks hooks;
    hooks.capture = [](run::RunId) -> Result<EngineLuaDebugOwner> { return owner("editor"); };
    hooks.prepareLua = [](bool play, std::stop_token, MultiSessionDebugCoordinator::Prepared done,
                          bool) -> Result<void> {
        EXPECT_TRUE(play);
        done(PreparedDebugLaunch{
            [](auto&, auto) -> Result<ui::DebugSessionId> { return fail(ErrorCode::Internal, "must not adopt"); }});
        return {};
    };
    EngineRunDebugCoordinator debug(runs, *manager, std::move(hooks));
    std::optional<PreparedDebugLaunch> prepared;
    ASSERT_TRUE(debug.prepare(configuration(run::RunType::FrostEditor), {}, [&](Result<PreparedDebugLaunch> result) {
        ASSERT_TRUE(result);
        prepared = std::move(*result);
    }));
    ASSERT_TRUE(spin([&] { return prepared.has_value(); }));
    ASSERT_EQ(manager->activeRuns().size(), 1u);
    prepared.reset();
    ASSERT_TRUE(spin([&] { return manager->activeRuns().empty(); }));
    EXPECT_TRUE(launcher.launches().front().terminated);
}
TEST(EngineRunDebug, WrongAuthenticatedRoleAndStartupTimeoutCannotAdoptAnotherHost) {
    for (bool wrongRole : {true, false}) {
        io::ScriptedProcessLauncher launcher;
        launcher.expect({.neverExits = true});
        auto manager = run::RunManager::create(launcher);
        ui::RunController runs(nullptr, runHooks());
        runs.setRunManager(manager.get());
        EngineRunDebugHooks hooks;
        hooks.capture = [=](run::RunId) -> Result<EngineLuaDebugOwner> {
            if (wrongRole)
                return owner("editor");
            return fail(ErrorCode::Busy, "pending");
        };
        hooks.prepareLua = [](bool, std::stop_token, auto, bool) -> Result<void> {
            ADD_FAILURE() << "Unmatched or unauthenticated hosts cannot prepare DAP";
            return fail(ErrorCode::Internal, "unexpected");
        };
        EngineRunDebugOptions options;
        options.startupTimeout = 20ms;
        EngineRunDebugCoordinator debug(runs, *manager, std::move(hooks), options);
        std::optional<Error> error;
        ASSERT_TRUE(debug.prepare(configuration(), {}, [&](Result<PreparedDebugLaunch> result) {
            ASSERT_FALSE(result);
            error = result.error();
        }));
        ASSERT_TRUE(spin([&] { return error.has_value() && manager->activeRuns().empty(); }));
        EXPECT_EQ(error->code(), wrongRole ? ErrorCode::PermissionDenied : ErrorCode::Timeout);
        EXPECT_TRUE(launcher.launches().front().terminated);
    }
}
TEST(EngineRunDebug, IndependentDebugHostRetainsConfigurationTrustDespiteOrdinaryRunVeto) {
    io::ScriptedProcessLauncher launcher;
    launcher.expect({.neverExits = true});
    auto manager = run::RunManager::create(launcher);
    auto hooks = runHooks();
    bool trusted = false;
    hooks.mayRun = [] { return false; };
    hooks.mayRunConfiguration = [&](const auto&) -> Result<void> {
        if (!trusted)
            return fail(ErrorCode::Untrusted, "validator trust refusal");
        return {};
    };
    ui::RunController runs(nullptr, std::move(hooks));
    runs.setRunManager(manager.get());
    auto c = configuration();
    EXPECT_EQ(runs.runEngineConfiguration(c).error().code(), ErrorCode::Untrusted);
    EXPECT_TRUE(launcher.launches().empty());
    trusted = true;
    auto launched = runs.runEngineConfiguration(c);
    ASSERT_TRUE(launched);
    ASSERT_TRUE(spin([&] { return launcher.launches().size() == 1; }));
    manager->stop(*launched);
    EXPECT_TRUE(manager->waitForIdle(5s));
}
TEST(EngineRunDebug, TransientRunSeamRefusesImplicitRerunAndNonEngineConfigurations) {
    io::ScriptedProcessLauncher launcher;
    launcher.expect({.neverExits = true});
    auto manager = run::RunManager::create(launcher);
    ui::RunController runs(nullptr, runHooks());
    runs.setRunManager(manager.get());
    auto c = configuration();
    auto started = runs.runEngineConfiguration(c);
    ASSERT_TRUE(started);
    EXPECT_EQ(runs.runEngineConfiguration(c).error().code(), ErrorCode::Busy);
    c.type = run::RunType::Native;
    EXPECT_EQ(runs.runEngineConfiguration(c).error().code(), ErrorCode::InvalidArgument);
    ASSERT_TRUE(spin([&] { return launcher.launches().size() == 1; }));
    manager->stop(*started);
    EXPECT_TRUE(manager->waitForIdle(5s));
    EXPECT_EQ(launcher.launches().size(), 1u);
}
} // namespace
} // namespace frost::app
