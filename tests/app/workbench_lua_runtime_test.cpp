// SPDX-License-Identifier: Apache-2.0
// Source-authored composition unit tests. Scripted children prove routing/argv, not Lua acceptance.
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/run_controller.hpp"

#include "workbench_rig.hpp"
#include <gtest/gtest.h>
namespace frost::app::test {
namespace {
TEST(WorkbenchLuaRuntime, RunUsesExplicitInterpreterAndDirtyScriptSavePolicy) {
    WorkDir dir("lua-explicit-runtime");
    dir.write("tools/lua", "unit fixture, never executed");
    dir.write("workspace/main.lua", "print('old')\n");
    io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    ASSERT_TRUE(rig.settings->set(settings::Scope::Application, settings::key::buildSaveAllBeforeBuild, true));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("workspace")));
    run::RunConfiguration config;
    config.id = "lua.script";
    config.name = "Lua";
    config.type = run::RunType::LuaScript;
    config.program = dir.join("tools/lua");
    config.target = dir.join("workspace/main.lua");
    config.interpreterArguments = {"-E"};
    config.arguments = {"two words", ""};
    rig.workbench->runController()->setStoredConfigurations({config});
    ASSERT_TRUE(rig.workbench->runController()->selectConfiguration(config.id));
    ASSERT_TRUE(rig.workbench->openPath(config.target));
    ASSERT_TRUE(rig.view()->editor().document().replace({0, 0}, "-- saved before Lua\n", {}));
    launcher.expect({.expectExecutable = config.program});
    auto started = rig.workbench->runController()->run();
    ASSERT_TRUE(started) << started.error().describe();
    ASSERT_TRUE(rig.workbench->runManager()->waitForIdle(std::chrono::seconds(30)));
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    EXPECT_EQ(launches.front().spec.executable, config.program);
    EXPECT_EQ(launches.front().spec.arguments, (std::vector<std::string>{"-E", config.target, "two words", ""}));
    EXPECT_EQ(launches.front().spec.stdinMode, io::Stdio::Pipe);
    EXPECT_TRUE(launches.front().spec.killOnClose);
    EXPECT_TRUE(dir.read("workspace/main.lua").starts_with("-- saved before Lua\n"));
}
TEST(WorkbenchLuaRuntime, MissingAdapterAndReplDebugNeverStartNativeOrEngineFallback) {
    WorkDir dir("lua-debug-unavailable");
    dir.write("tools/lua", "unit fixture, never executed");
    dir.write("workspace/main.lua", "return 42\n");
    io::ScriptedProcessLauncher launcher;
    RigOptions options;
    options.launcher = &launcher;
    options.disableProjectEvaluation = true;
    WorkbenchRig rig(dir, options);
    ASSERT_TRUE(rig.trust->trust(dir.join("workspace")));
    ASSERT_TRUE(rig.workbench->openPath(dir.join("workspace")));
    run::RunConfiguration config;
    config.id = "lua.script";
    config.name = "Lua";
    config.type = run::RunType::LuaScript;
    config.program = dir.join("tools/lua");
    config.target = dir.join("workspace/main.lua");
    rig.workbench->runController()->setStoredConfigurations({config});
    ASSERT_TRUE(rig.workbench->runController()->selectConfiguration(config.id));
    auto missing = rig.workbench->debugController()->debug();
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    EXPECT_TRUE(launcher.launches().empty());
    config.type = run::RunType::LuaRepl;
    config.target.clear();
    rig.workbench->runController()->setStoredConfigurations({config});
    ASSERT_TRUE(rig.workbench->runController()->selectConfiguration(config.id));
    auto unsupported = rig.workbench->debugController()->debug();
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().code(), ErrorCode::Unsupported);
    EXPECT_TRUE(launcher.launches().empty());
}
} // namespace
} // namespace frost::app::test
