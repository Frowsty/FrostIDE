// SPDX-License-Identifier: Apache-2.0
#include "frost/debug/gdb_dap.hpp"
#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"
#include "debug_test_util.hpp"
#include <gtest/gtest.h>
#include <algorithm>

namespace frost::debug {
namespace {
TEST(GdbDap, VersionAndEntrySemanticsFailClosed) {
    EXPECT_TRUE(validateGdbDapVersion("GNU gdb (GDB) 16.3\n"));
    EXPECT_TRUE(validateGdbDapVersion("GNU gdb (Ubuntu 17.1-1) 17.1\n"));
    EXPECT_FALSE(validateGdbDapVersion("GNU gdb (GDB) 15.2\n"));
    EXPECT_FALSE(validateGdbDapVersion("lldb version 23.1.4"));
    EXPECT_FALSE(validateGdbDapVersion("GNU gdb (GDB) unknown"));
    DebugTarget target{.program = test::workPath("program"), .stopOnEntry = true};
    EXPECT_FALSE(gdbDapLaunchArguments(target, {}));
}
TEST(GdbDap, DiscoveryNeverSearchesPathOrSubstitutesMissingConfiguredTools) {
    ManualClock clock;
    io::MemFs fs(clock);
    EXPECT_EQ(findGdbDap(fs, "gdb").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(findGdbDap(fs, test::workPath("missing")).error().code(), ErrorCode::NotFound);
    ASSERT_TRUE(fs.createDirectories(test::workPath("tools")));
    const auto path = test::workPath("tools/gdb");
    ASSERT_TRUE(fs.writeFileAtomic(path, "fixture"));
    EXPECT_EQ(findGdbDap(fs, path).value(), path);
}
TEST(GdbDap, StartupDisablesInitAutoLoadAndNetworkWithoutTouchingDapStdio) {
    const auto spec = gdbDapProcessSpec(test::workPath("gdb"));
    for (const auto* argument : {"--nx", "--interpreter=dap", "--init-eval-command=set auto-load off",
                                "--init-eval-command=set debuginfod enabled off"})
        EXPECT_NE(std::ranges::find(spec.arguments, argument), spec.arguments.end());
    EXPECT_EQ(spec.stdinMode, io::Stdio::Pipe);
    EXPECT_EQ(spec.stdoutMode, io::Stdio::Pipe);
    EXPECT_EQ(spec.stderrMode, io::Stdio::Pipe);
    EXPECT_TRUE(spec.killOnClose);
}
TEST(GdbDap, EnvironmentReplacesTheInferiorBlockAndKeepsArgumentsLiteral) {
    DebugTarget target{.program = test::workPath("program"), .arguments = {"a b", "$(touch nope)"},
                       .environment = {{"KEEP", "override"}, {"ADDED", "value"}}};
    auto arguments = gdbDapLaunchArguments(target, {{"KEEP", "original"}, {"OTHER", "inherited"}});
    ASSERT_TRUE(arguments);
    EXPECT_EQ(arguments->find("env")->stringAt("KEEP"), "override");
    EXPECT_EQ(arguments->find("env")->stringAt("OTHER"), "inherited");
    EXPECT_EQ(arguments->find("args")->dump(), "[\"a b\",\"$(touch nope)\"]");
    EXPECT_EQ(arguments->find("initCommands"), nullptr);
    target.inheritEnvironment = false;
    arguments = gdbDapLaunchArguments(target, {{"OTHER", "must disappear"}});
    ASSERT_TRUE(arguments);
    EXPECT_EQ(arguments->find("env")->find("OTHER"), nullptr);
}
TEST(GdbDap, UntrustedLaunchAndInvalidAttachCannotStartAnAdapter) {
    ManualClock clock;
    io::MemFs fs(clock);
    ASSERT_TRUE(fs.createDirectories(test::workPath("data")));
    auto trust = project::TrustStore::open(fs, test::workPath("data/trust"));
    ASSERT_TRUE(trust);
    int calls = 0;
    AdapterFactory factory = [&](DapHandlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        ++calls;
        return fail(ErrorCode::NotFound, "must not start");
    };
    DebugTarget target{.program = test::workPath("untrusted/program")};
    auto launch = startGdbDebugSession(**trust, target, factory, {}, {}, {});
    ASSERT_FALSE(launch);
    EXPECT_EQ(launch.error().code(), ErrorCode::Untrusted);
    EXPECT_FALSE(attachGdbDebugSession(**trust, target, 0, factory, {}, {}));
    EXPECT_EQ(calls, 0);
}
} // namespace
} // namespace frost::debug

