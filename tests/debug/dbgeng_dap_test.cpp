// SPDX-License-Identifier: Apache-2.0
// DbgEng client admission/serialization: local paths, symbol-server refusal and exact argv/env.
#include "frost/debug/dbgeng_dap.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

namespace frost::debug {
namespace {

TEST(DbgEngDap, SerializesExplicitLiveAndReadOnlyDumpTargetsWithoutChangingLldbDefaults) {
    DbgEngTarget target;
    target.native.program = test::workPath("engine/FrostEditor.exe");
    target.native.arguments = {"--project", "project with spaces"};
    target.native.environment = {{"FROST_VALUE", "42"}};
    target.native.inheritEnvironment = false;
    target.natvisFiles = {test::workPath("engine/Frost.natvis")};
    target.localSymbolPaths = {test::workPath("symbols")};
    auto live = dbgEngDapLaunchArguments(target);
    ASSERT_TRUE(live.has_value()) << live.error().describe();
    EXPECT_EQ(live->stringAt("type"), kDbgEngAdapterId);
    EXPECT_EQ(*live->find("args")->items()[1].asString(), "project with spaces");
    EXPECT_EQ(live->find("env")->stringAt("FROST_VALUE"), "42");
    EXPECT_EQ(live->boolAt("inheritEnvironment"), false);
    EXPECT_EQ(live->find("dumpFile"), nullptr);
    EXPECT_EQ(lldbDapLaunchArguments(target.native).stringAt("type"), "lldb-dap");

    target.dumpFile = test::workPath("crashes/crash.dmp");
    auto dump = dbgEngDapLaunchArguments(target);
    ASSERT_TRUE(dump.has_value()) << dump.error().describe();
    EXPECT_EQ(dump->stringAt("dumpFile"), target.dumpFile);
    target.native.program.clear();
    auto dumpOnly = dbgEngDapLaunchArguments(target);
    ASSERT_TRUE(dumpOnly.has_value());
    EXPECT_TRUE(dumpOnly->stringAt("program").empty());
    EXPECT_EQ(dumpOnly->stringAt("dumpFile"), target.dumpFile);
    target.dumpFile.clear();
    EXPECT_FALSE(dbgEngDapLaunchArguments(target).has_value());
}

TEST(DbgEngDap, RejectsRemoteSymbolsRelativeArtifactsAndDebuggerCommandPaths) {
    DbgEngTarget target;
    target.native.program = test::workPath("FrostEditor.exe");
    for (const std::string path :
         {"srv*https://example.invalid/symbols", "relative", "\\\\host\\symbols", "//host/symbols", "\\/host/symbols",
          "/\\host/symbols", "C:\\local; srv*https://example.invalid"}) {
        target.localSymbolPaths = {path};
        EXPECT_FALSE(dbgEngDapLaunchArguments(target).has_value()) << path;
    }
    target.localSymbolPaths.clear();
    target.natvisFiles = {test::workPath("ok.natvis\n.shell command")};
    EXPECT_FALSE(dbgEngDapLaunchArguments(target).has_value());
    target.natvisFiles.clear();
    target.dumpFile = "crash.dmp";
    EXPECT_FALSE(dbgEngDapLaunchArguments(target).has_value());
    for (const std::string path : {"\\/host/artifact", "/\\host/artifact"}) {
        target.dumpFile = path;
        EXPECT_FALSE(dbgEngDapLaunchArguments(target).has_value()) << path;
        target.dumpFile.clear();
        target.natvisFiles = {path};
        EXPECT_FALSE(dbgEngDapLaunchArguments(target).has_value()) << path;
        target.natvisFiles.clear();
        target.native.program = path;
        EXPECT_FALSE(dbgEngDapLaunchArguments(target).has_value()) << path;
        target.native.program = test::workPath("FrostEditor.exe");
    }
}

TEST(DbgEngDap, StartsOnlyTheSeparateAdapterAndStripsInheritedSymbolEndpoints) {
    auto process = dbgEngDapProcessSpec(test::workPath("frost-dbgeng-dap.exe"));
    EXPECT_EQ(process.executable, test::workPath("frost-dbgeng-dap.exe"));
    EXPECT_EQ(process.stdinMode, io::Stdio::Pipe);
    EXPECT_EQ(process.stdoutMode, io::Stdio::Pipe);
    EXPECT_EQ(process.stderrMode, io::Stdio::Pipe);
    ASSERT_EQ(process.environment.size(), kStrippedAdapterEnvironment.size());
    for (std::size_t i = 0; i < process.environment.size(); ++i) {
        EXPECT_EQ(process.environment[i].first, kStrippedAdapterEnvironment[i]);
        EXPECT_FALSE(process.environment[i].second.has_value());
    }
}

} // namespace
} // namespace frost::debug
