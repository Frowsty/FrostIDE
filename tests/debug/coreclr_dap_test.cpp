// SPDX-License-Identifier: Apache-2.0
// Tests the explicit CoreCLR host and argument handling without starting an adapter.

#include "frost/core/clock.hpp"
#include "frost/debug/coreclr_dap.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/project/trust_store.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>
#include <atomic>

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

TEST(CoreClrDap, LaunchRejectsUntrustedWorkingDirectoryBeforeAdapterCreation) {
    ManualClock clock;
    io::MemFs fs(clock);
    ASSERT_TRUE(fs.createDirectories(test::workPath("data")));
    ASSERT_TRUE(fs.createDirectories(test::workPath("project")));
    auto trust = project::TrustStore::open(fs, test::workPath("data/trust"));
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(test::workPath("project")));
    CoreClrTarget target{.dotnet = test::workPath("sdk/dotnet"),
                         .assembly = test::workPath("project/App.dll"),
                         .workingDirectory = test::workPath("other")};
    int calls = 0;
    AdapterFactory factory = [&calls](DapHandlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
        ++calls;
        return fail(ErrorCode::Unknown, "unexpected adapter start");
    };
    auto session = startCoreClrSession(**trust, target, {test::workPath("project")}, std::move(factory), {}, {});
    ASSERT_FALSE(session);
    EXPECT_EQ(session.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(calls, 0);
}

TEST(CoreClrDap, LaunchRechecksTrustAtTheActualDapWriter) {
    ManualClock clock;
    io::MemFs fs(clock);
    const auto root = test::workPath("project");
    ASSERT_TRUE(fs.createDirectories(root));
    ASSERT_TRUE(fs.createDirectories(test::workPath("data")));
    auto trust = project::TrustStore::open(fs, test::workPath("data/trust"));
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(root));
    CoreClrTarget target{.dotnet = test::workPath("sdk/dotnet"),
                         .assembly = test::workPath("project/App.dll"), .workingDirectory = root};
    FakeDebugAdapter adapter;
    adapter.on("initialize", [&](const rpc::Json&) {
        EXPECT_TRUE((*trust)->untrust(root));
        return FakeDebugAdapter::Reply{.body = rpc::Json::object()};
    });
    test::SessionRecorder recorder;
    auto session = startCoreClrSession(**trust, target, {root}, test::fakeFactory(adapter), {}, recorder.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(test::waitUntil([&] { return recorder.hasEnded(); }));
    EXPECT_TRUE(adapter.argumentsOf("launch").empty());
    ASSERT_TRUE((*session)->outcome());
    ASSERT_TRUE((*session)->outcome()->error);
    EXPECT_EQ((*session)->outcome()->error->code(), ErrorCode::Untrusted);
}

TEST(CoreClrDap, AttachUsesCoreClrIdentityAndDisconnectsWithoutTerminatingTheHost) {
    ManualClock clock;
    io::MemFs fs(clock);
    const auto root = test::workPath("project");
    ASSERT_TRUE(fs.createDirectories(root));
    ASSERT_TRUE(fs.createDirectories(test::workPath("data")));
    auto trust = project::TrustStore::open(fs, test::workPath("data/trust"));
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(root));
    DebugTarget target;
    target.program = test::workPath("sdk/dotnet");
    target.workingDirectory = root;
    target.trustRoots = {root};
    FakeDebugAdapter adapter;
    adapter.on("attach", [](const rpc::Json&) {
        return FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json::object()}}};
    });
    test::SessionRecorder recorder;
    auto session = attachCoreClrSession(**trust, target, 123, test::fakeFactory(adapter), [] { return Result<void>{}; },
                                        {}, recorder.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(adapter.waitForRequest("attach"));
    EXPECT_EQ(adapter.argumentsOf("initialize").front().stringAt("adapterID"), "coreclr");
    EXPECT_EQ(adapter.argumentsOf("attach").front().integerAt("processId"), 123);
    EXPECT_TRUE(adapter.argumentsOf("launch").empty());
    (*session)->stop();
    ASSERT_TRUE(adapter.waitForRequest("disconnect"));
    EXPECT_EQ(adapter.argumentsOf("disconnect").front().boolAt("terminateDebuggee"), false);
    EXPECT_TRUE(adapter.argumentsOf("terminate").empty());
}

TEST(CoreClrDap, AttachRechecksCapturedProcessAfterInitialize) {
    ManualClock clock;
    io::MemFs fs(clock);
    const auto root = test::workPath("project");
    ASSERT_TRUE(fs.createDirectories(root));
    ASSERT_TRUE(fs.createDirectories(test::workPath("data")));
    auto trust = project::TrustStore::open(fs, test::workPath("data/trust"));
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trust(root));
    DebugTarget target;
    target.program = test::workPath("sdk/dotnet");
    target.trustRoots = {root};
    std::atomic_bool live{true};
    FakeDebugAdapter adapter;
    adapter.on("initialize", [&](const rpc::Json&) {
        live.store(false);
        return FakeDebugAdapter::Reply{};
    });
    auto admission = [&]() -> Result<void> {
        if (!live.load())
            return fail(ErrorCode::NotFound, "selected process exited");
        return {};
    };
    test::SessionRecorder recorder;
    auto session = attachCoreClrSession(**trust, target, 123, test::fakeFactory(adapter), admission, {},
                                        recorder.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(test::waitUntil([&] { return recorder.hasEnded(); }));
    EXPECT_TRUE(adapter.argumentsOf("attach").empty());
    ASSERT_TRUE((*session)->outcome()->error);
    EXPECT_EQ((*session)->outcome()->error->code(), ErrorCode::NotFound);
}

TEST(CoreClrDap, RequestOnlyGrantRemainsUntilWriterAdmissionThenEndsBeforeAttach) {
    ManualClock clock;
    io::MemFs fs(clock);
    const auto root = test::workPath("one-off");
    ASSERT_TRUE(fs.createDirectories(root));
    ASSERT_TRUE(fs.createDirectories(test::workPath("data")));
    auto trust = project::TrustStore::open(fs, test::workPath("data/trust"));
    ASSERT_TRUE(trust);
    ASSERT_TRUE((*trust)->trustForSession(root));
    DebugTarget target;
    target.program = test::workPath("one-off/dotnet");
    target.workingDirectory = root;
    target.trustRoots = {root};
    FakeDebugAdapter adapter;
    std::atomic_int releases{0};
    adapter.on("initialize", [&](const rpc::Json&) {
        EXPECT_TRUE((*trust)->isTrusted(root));
        return FakeDebugAdapter::Reply{};
    });
    adapter.on("attach", [&](const rpc::Json&) {
        EXPECT_FALSE((*trust)->isTrusted(root));
        return FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json::object()}}};
    });
    auto release = [&] {
        (*trust)->endSessionTrust(root);
        ++releases;
    };
    auto session = attachCoreClrSession(**trust, target, 123, test::fakeFactory(adapter), [] { return Result<void>{}; },
                                        {}, {}, release);
    ASSERT_TRUE(session);
    ASSERT_TRUE(adapter.waitForRequest("attach"));
    EXPECT_EQ(releases.load(), 1);
    EXPECT_FALSE((*trust)->isTrusted(root));
    (*session)->stop();
}
TEST(CoreClrDap, ProcessAdmissionRejectsMalformedSelectionWithoutInspectingAnOsProcess) {
    EXPECT_FALSE(coreClrProcessAdmission(0, test::workPath("sdk/dotnet")));
    EXPECT_FALSE(coreClrProcessAdmission(123, "dotnet"));
    EXPECT_FALSE(coreClrProcessAdmission(123, std::string("/bad\0path", 9)));
}
} // namespace
} // namespace frost::debug
