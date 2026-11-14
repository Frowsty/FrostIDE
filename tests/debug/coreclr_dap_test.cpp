// SPDX-License-Identifier: Apache-2.0
// Tests the explicit CoreCLR host and argument handling without starting an adapter.

#include "frost/core/clock.hpp"
#include "frost/debug/coreclr_dap.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/project/trust_store.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

namespace frost::debug {
namespace {

TEST(CoreClrDap, LaunchUsesAbsoluteHostAndPreservesArgumentsAndEnvironment) {
    const CoreClrTarget target{.dotnet = test::workPath("sdk/dotnet"),
                               .assembly = test::workPath("project/bin/App.dll"),
                               .workingDirectory = test::workPath("project"),
                               .arguments = {"two words", ""},
                               .environment = {{"TEST_VAR", "a=b"}}};
    const auto launch = coreClrLaunchArguments(target);
    ASSERT_TRUE(launch.has_value());
    EXPECT_EQ(launch->stringAt("program"), target.dotnet);
    EXPECT_EQ(launch->stringAt("cwd"), target.workingDirectory);
    ASSERT_NE(launch->find("args"), nullptr);
    ASSERT_EQ(launch->find("args")->size(), 3U);
    EXPECT_EQ(*launch->find("args")->items()[0].asString(), target.assembly);
    EXPECT_EQ(*launch->find("args")->items()[1].asString(), "two words");
    EXPECT_EQ(*launch->find("args")->items()[2].asString(), "");
    ASSERT_NE(launch->find("env"), nullptr);
    EXPECT_EQ(launch->find("env")->stringAt("TEST_VAR"), "a=b");
}

TEST(CoreClrDap, RejectsRelativePathsAndUnrepresentableEnvironment) {
    CoreClrTarget target{.dotnet = test::workPath("sdk/dotnet"),
                         .assembly = test::workPath("project/App.dll"),
                         .workingDirectory = test::workPath("project")};
    target.dotnet = "dotnet";
    EXPECT_FALSE(coreClrLaunchArguments(target));
    target.dotnet = test::workPath("sdk/dotnet");
    target.environment.emplace_back("BAD=NAME", "value");
    EXPECT_FALSE(coreClrLaunchArguments(target));
    target.environment.clear();
    target.arguments.emplace_back("a\0b", 3);
    EXPECT_FALSE(coreClrLaunchArguments(target));
}

TEST(CoreClrDap, UntrustedProjectCannotStartTheAdapter) {
    ManualClock clock;
    io::MemFs fs(clock);
    ASSERT_TRUE(fs.createDirectories(test::workPath("data")));
    ASSERT_TRUE(fs.createDirectories(test::workPath("project")));
    auto trust = project::TrustStore::open(fs, test::workPath("data/trust.txt"));
    ASSERT_TRUE(trust);
    CoreClrTarget target{.dotnet = test::workPath("sdk/dotnet"),
                         .assembly = test::workPath("project/App.dll"),
                         .workingDirectory = test::workPath("project")};
    int calls = 0;
    AdapterFactory factory = [&calls](const DapHandlers&) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        ++calls;
        return fail(ErrorCode::Unknown, "unexpected adapter start");
    };
    auto denied = startCoreClrSession(**trust, target, {test::workPath("project")}, std::move(factory), {}, {});
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(calls, 0);
    ASSERT_TRUE(fs.createDirectories(test::workPath("other")));
    ASSERT_TRUE((*trust)->trust(test::workPath("other")));
    AdapterFactory secondFactory = [&calls](const DapHandlers&) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        ++calls;
        return fail(ErrorCode::Unknown, "unexpected adapter start");
    };
    auto wrongRoot = startCoreClrSession(**trust, target, {test::workPath("other")}, std::move(secondFactory), {}, {});
    ASSERT_FALSE(wrongRoot);
    EXPECT_EQ(wrongRoot.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(calls, 0);
}

} // namespace
} // namespace frost::debug
