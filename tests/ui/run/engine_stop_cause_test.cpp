// SPDX-License-Identifier: Apache-2.0
// FER-16 explicit Run Stop classification precedes actual cleanup; automatic cleanup stays untagged.
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/ui/run_controller.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <gtest/gtest.h>
namespace frost::ui {
namespace {
bool events(const std::function<bool()>& done) {
    const QDeadlineTimer deadline(5000);
    while (!done()) {
        if (deadline.hasExpired())
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}
TEST(EngineRunStopCause, UserStopNotifiesExactRunBeforeTerminationWhileAutomaticCleanupDoesNot) {
    io::ScriptedProcessLauncher launcher;
    launcher.expect({.neverExits = true});
    launcher.expect({.neverExits = true});
    auto manager = run::RunManager::create(launcher);
    run::RunConfiguration c;
    c.id = "engine.stop";
    c.name = "Editor";
    c.type = run::RunType::FrostEditor;
    const std::string root = io::kNativePathStyle == io::PathStyle::Windows ? "C:/stop-cause" : "/stop-cause";
    c.program = io::normalizePath(root + "/FrostEditor.exe").value_or("");
    c.engineProjectPath = io::normalizePath(root + "/Game.frostproj").value_or("");
    RunControllerHooks hooks;
    hooks.prepareEngineConfiguration = [](const auto& c) -> Result<run::RunConfiguration> { return c; };
    hooks.engineLaunchAdmissionFor = [](const auto&) -> std::function<Result<void>()> {
        return []() -> Result<void> { return {}; };
    };
    hooks.engineLaunchPlanFor = [](const auto&) -> std::function<Result<run::LaunchPlan>(run::LaunchPlan)> {
        return [](run::LaunchPlan p) -> Result<run::LaunchPlan> { return p; };
    };
    std::vector<run::RunId> explicitStops;
    hooks.engineStopRequested = [&](run::RunId id) {
        explicitStops.push_back(id);
        ASSERT_TRUE(manager->status(id));
        EXPECT_FALSE(launcher.launches().back().terminated);
    };
    RunController controller(nullptr, std::move(hooks));
    controller.setRunManager(manager.get());
    auto first = controller.runEngineConfiguration(c);
    ASSERT_TRUE(first);
    ASSERT_TRUE(events([&] { return !launcher.launches().empty(); }));
    controller.stop();
    ASSERT_TRUE(events([&] { return !controller.configurationRunning(c.id); }));
    ASSERT_EQ(explicitStops, (std::vector<run::RunId>{*first}));
    auto second = controller.runEngineConfiguration(c);
    ASSERT_TRUE(second);
    ASSERT_TRUE(events([&] { return launcher.launches().size() == 2; }));
    manager->stop(*second);
    ASSERT_TRUE(events([&] { return !controller.configurationRunning(c.id); }));
    EXPECT_EQ(explicitStops, (std::vector<run::RunId>{*first}));
    EXPECT_TRUE(manager->waitForIdle(std::chrono::seconds(5)));
    EXPECT_TRUE(launcher.launches().back().terminated);
}
} // namespace
} // namespace frost::ui
