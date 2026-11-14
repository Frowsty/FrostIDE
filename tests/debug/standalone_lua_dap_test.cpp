// SPDX-License-Identifier: Apache-2.0
// Real protocol/argv construction, not simulated conformance acceptance.
#include "frost/debug/standalone_lua_dap.hpp"
#include "frost/io/scripted_process_launcher.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>
namespace frost::debug {
namespace {
TEST(StandaloneLuaDap, ExplicitLuaExeAndArg0NeverFallBackToAdapterBundledInterpreter) {
    StandaloneLuaTarget target;
    target.interpreter = test::workPath("sdk/selected-lua");
    target.script = test::workPath("project/main.lua");
    target.workingDirectory = test::workPath("project");
    target.interpreterArguments = {"-E", "-e", "assert(_VERSION == 'Lua 5.4')"};
    target.arguments = {"two words", ""};
    target.environment = {{"USER_VALUE", "a=b"}};
    auto launch = standaloneLuaLaunchArguments(target);
    ASSERT_TRUE(launch);
    EXPECT_EQ(launch->stringAt("luaexe"), target.interpreter);
    EXPECT_EQ(launch->stringAt("luaVersion"), "lua54");
    EXPECT_EQ(launch->stringAt("program"), target.script);
    EXPECT_EQ(launch->stringAt("cwd"), target.workingDirectory);
    ASSERT_NE(launch->find("arg0"), nullptr);
    ASSERT_EQ(launch->find("arg0")->size(), 3U);
    EXPECT_EQ(*launch->find("arg0")->items()[2].asString(), "assert(_VERSION == 'Lua 5.4')");
    ASSERT_NE(launch->find("arg"), nullptr);
    ASSERT_EQ(launch->find("arg")->size(), 2U);
    EXPECT_EQ(*launch->find("arg")->items()[0].asString(), "two words");
    EXPECT_EQ(launch->stringAt("console"), "internalConsole");
    EXPECT_EQ(launch->boolAt("useWSL"), false);
}
TEST(StandaloneLuaDap, RejectsMissingRuntimeAndRelativeScriptBeforeStartingAdapter) {
    StandaloneLuaTarget target;
    target.script = test::workPath("project/main.lua");
    target.workingDirectory = test::workPath("project");
    EXPECT_FALSE(standaloneLuaLaunchArguments(target));
    target.interpreter = test::workPath("sdk/lua");
    target.script = "relative.lua";
    EXPECT_FALSE(standaloneLuaLaunchArguments(target));
    io::ScriptedProcessLauncher launcher;
    auto factory = standaloneLuaAdapterFactory(launcher, "lua-debug");
    EXPECT_FALSE(factory({}));
}

TEST(StandaloneLuaDap, InstalledAdapterUsesNoArgumentsAndOwnedStdioWithNoInterpreterSubstitution) {
    io::ScriptedProcessLauncher launcher;
    const auto adapter = test::workPath("installed-lua-debug/bin/lua-debug");
    launcher.expect({.expectExecutable = adapter, .launchError = Error(ErrorCode::IoError, "authored start refusal")});
    auto started = standaloneLuaAdapterFactory(launcher, adapter)({});
    EXPECT_FALSE(started);
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    EXPECT_EQ(launches.front().spec.executable, adapter);
    EXPECT_TRUE(launches.front().spec.arguments.empty());
    EXPECT_EQ(launches.front().spec.stdinMode, io::Stdio::Pipe);
    EXPECT_EQ(launches.front().spec.stdoutMode, io::Stdio::Pipe);
    EXPECT_EQ(launches.front().spec.stderrMode, io::Stdio::Pipe);
    EXPECT_TRUE(launches.front().spec.killOnClose);
}

} // namespace
} // namespace frost::debug
