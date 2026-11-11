// SPDX-License-Identifier: Apache-2.0
// Filesystem-only discovery regressions: exact instance selection, authoritative records and tools.
#include "../../src/app/src/engine_locator.hpp"
#include "workbench_rig.hpp"

#include <algorithm>

namespace {
using frost::app::test::WorkDir;
using frost::lang::Json;

Json record(const std::string& project, const std::string& instance, int port, const std::string& generation) {
    return Json::object()
        .set("host", "127.0.0.1")
        .set("port", port)
        .set("projectPath", project)
        .set("instanceId", instance)
        .set("generation", generation)
        .set("tokenRequired", true)
        .set("token", "fixture-secret");
}

TEST(EngineLocator, InstancesRemainSeparateAndAuthoritativeRecordsWin) {
    WorkDir dir("engine-instance-list");
    dir.write("project/Game.frostproj", "{}");
    dir.write("other/Game.frostproj", "{}");
    const std::string first(32, 'a'), second(32, 'b');
    dir.write("Automation/Instances/first.json", record(dir.join("project"), first, 41001, "current").dump());
    dir.write("Automation/Instances/second.json", record(dir.join("project"), second, 41002, "second").dump());
    dir.write("Automation/Projects/project/first.json", record(dir.join("project"), first, 40000, "stale").dump());
    dir.write("Automation/Instances/wrong-project.json",
              record(dir.join("other"), std::string(32, 'c'), 41003, "wrong").dump());
    auto remote = record(dir.join("project"), std::string(32, 'd'), 41004, "remote");
    remote.set("host", "192.0.2.1");
    dir.write("Automation/Instances/remote.json", remote.dump());
    auto found =
        frost::app::listEngineInstances(frost::io::realFileSystem(), dir.join("Automation"), dir.join("project"));
    ASSERT_TRUE(found.has_value()) << found.error().describe();
    ASSERT_EQ(found->size(), 2u);
    EXPECT_EQ((*found)[0].instanceId, first);
    EXPECT_EQ((*found)[0].port, 41001);
    EXPECT_EQ((*found)[0].record.stringAt("generation"), "current");
    EXPECT_EQ((*found)[1].instanceId, second);
    EXPECT_EQ((*found)[1].port, 41002);
}

TEST(EngineLocator, MissingAndCancelledDiscoveryNeverReportsInventedInstances) {
    WorkDir dir("engine-instance-empty");
    dir.write("Game.frostproj", "{}");
    auto found = frost::app::listEngineInstances(frost::io::realFileSystem(), dir.join("missing"), dir.path());
    ASSERT_TRUE(found.has_value());
    EXPECT_TRUE(found->empty());
    std::stop_source cancelled;
    cancelled.request_stop();
    auto stopped = frost::app::listEngineInstances(frost::io::realFileSystem(), dir.join("missing"), dir.path(),
                                                   cancelled.get_token());
    ASSERT_FALSE(stopped.has_value());
    EXPECT_EQ(stopped.error().code(), frost::ErrorCode::Cancelled);
}

TEST(EngineLocator, BuiltEditorUsesActualGuiAndConsoleExecutables) {
    WorkDir dir("engine-installation-list");
    dir.write("project/Game.frostproj", "{}");
#ifdef _WIN32
    const std::string gui = dir.join("build/windows-debug/bin/FrostEditor.exe");
    const std::string cli = dir.join("build/windows-debug/bin/FrostEditor.com");
    dir.write("build/windows-debug/bin/FrostEditor.exe", "not executed by filesystem discovery");
    dir.write("build/windows-debug/bin/FrostEditor.com", "not executed by filesystem discovery");
#else
    const std::string gui = dir.join("build/linux-debug/bin/FrostEditor");
    const std::string cli = gui;
    dir.write("build/linux-debug/bin/FrostEditor", "not executed by filesystem discovery");
#endif
    auto found = frost::app::locateEngineInstallations(frost::io::realFileSystem(), dir.join("project"));
    ASSERT_TRUE(found.has_value()) << found.error().describe();
    const auto selected = std::ranges::find(*found, gui, &frost::app::EngineInstallation::program);
    ASSERT_NE(selected, found->end());
    EXPECT_EQ(selected->cliProgram, cli);
    auto unavailable = frost::app::locateEngineInstallations(frost::io::realFileSystem(), dir.join("project"),
                                                             dir.join("missing-editor"));
    ASSERT_FALSE(unavailable.has_value());
    EXPECT_EQ(unavailable.error().code(), frost::ErrorCode::NotFound);
}
} // namespace
