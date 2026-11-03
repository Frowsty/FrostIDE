// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost/debug/lldb_dap.hpp without a real lldb-dap: the pinned path and the search, the
// process spec (no lldbinit, the stripped symbol-server variables), the launch request's arguments,
// the adapter factory through the scripted launcher, and the trust gate that refuses an untrusted
// root before any adapter starts.

#include "frost/core/clock.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/debug/lldb_dap.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/trust_store.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace frost::debug {
namespace {

using test::kWindows;
using test::workPath;

const std::string kExe = kWindows ? "lldb-dap.exe" : "lldb-dap";

// Whether `spec` removes `name` from the inherited environment.
bool removes(const io::ProcessSpec& spec, std::string_view name) {
    return std::ranges::any_of(spec.environment,
                               [&](const auto& entry) { return entry.first == name && !entry.second; });
}

TEST(LldbDap, ThePinnedPathFollowsTheSdkLock) {
    EXPECT_FALSE(pinnedLldbVersion().empty());
    const std::string expected = workPath(std::format("sdk/lldb-{}/bin/{}", pinnedLldbVersion(), kExe));
    EXPECT_EQ(pinnedLldbDapPath(workPath("sdk")).value(), expected);
    const LldbDapSearch search = defaultLldbDapSearch("X");
    EXPECT_EQ(search.configured, "X");
    if constexpr (kWindows) {
        EXPECT_NE(std::ranges::find(search.sdkRoots, std::string("E:\\sdk")), search.sdkRoots.end());
    }
}

TEST(LldbDap, FindsTheConfiguredOrThePinnedCopyAndNamesWhatItTried) {
    ManualClock clock;
    io::MemFs fs(clock);
    const LldbDapSearch empty{.configured = {}, .sdkRoots = {workPath("sdk1"), workPath("sdk2")}};
    auto missing = findLldbDap(fs, empty);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    EXPECT_NE(missing.error().message().find(pinnedLldbDapPath(workPath("sdk2")).value()), std::string::npos)
        << missing.error().message();
    EXPECT_NE(missing.error().message().find("tools.lldbDap.path"), std::string::npos);

    const std::string pinned = pinnedLldbDapPath(workPath("sdk2")).value();
    ASSERT_TRUE(fs.createDirectories(std::string(io::parentPath(pinned))).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(pinned, "MZ").has_value());
    EXPECT_EQ(findLldbDap(fs, empty).value(), pinned) << "the first SDK root that has it";

    const std::string custom = workPath("tools/my-lldb-dap.exe");
    ASSERT_TRUE(fs.createDirectories(workPath("tools")).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(custom, "MZ").has_value());
    EXPECT_EQ(findLldbDap(fs, LldbDapSearch{.configured = custom, .sdkRoots = empty.sdkRoots}).value(), custom)
        << "the setting wins";
    EXPECT_EQ(findLldbDap(fs, LldbDapSearch{.configured = workPath("tools/none.exe"), .sdkRoots = empty.sdkRoots})
                  .error()
                  .code(),
              ErrorCode::NotFound)
        << "a configured path that is missing is not replaced by the SDK's copy";
    EXPECT_EQ(findLldbDap(fs, LldbDapSearch{.configured = "lldb-dap", .sdkRoots = {}}).error().code(),
              ErrorCode::InvalidArgument)
        << "PATH is never searched";
}

TEST(LldbDap, TheProcessSpecReadsNoLldbinitAndDropsSymbolServers) {
    const io::ProcessSpec spec = lldbDapProcessSpec(workPath("sdk/" + kExe));
    EXPECT_EQ(spec.executable, workPath("sdk/" + kExe));
    EXPECT_EQ(spec.arguments, (std::vector<std::string>{"--no-lldbinit"}));
    for (const std::string_view name : {"_NT_SYMBOL_PATH", "_NT_ALT_SYMBOL_PATH", "DEBUGINFOD_URLS"}) {
        EXPECT_TRUE(removes(spec, name)) << name;
    }
    if constexpr (kWindows) {
        EXPECT_TRUE(removes(spec, "NoDefaultCurrentDirectoryInExePath"));
    }
    EXPECT_EQ(spec.stdinMode, io::Stdio::Pipe);
    EXPECT_EQ(spec.stdoutMode, io::Stdio::Pipe);
    EXPECT_TRUE(spec.killOnClose) << "Stop ends lldb-dap and the debuggee with it";
}

TEST(LldbDap, LaunchArgumentsCarryTheResolvedRun) {
    DebugTarget target{.name = "App [Debug|x64]",
                       .program = workPath("App/x64/Debug/App.exe"),
                       .arguments = {"--flag", "two words"},
                       .workingDirectory = workPath("App"),
                       .environment = {{"FROST_RUN_TEST", "1"}},
                       .inheritEnvironment = false,
                       .trustRoots = {}};
    const rpc::Json launch = lldbDapLaunchArguments(target);
    EXPECT_EQ(launch.stringAt("name"), "App [Debug|x64]");
    EXPECT_EQ(launch.stringAt("type"), "lldb-dap");
    EXPECT_EQ(launch.stringAt("request"), "launch");
    EXPECT_EQ(launch.stringAt("program"), target.program);
    EXPECT_EQ(launch.stringAt("cwd"), workPath("App"));
    EXPECT_EQ(launch.boolAt("stopOnEntry"), false);
    ASSERT_NE(launch.find("args"), nullptr);
    EXPECT_EQ(launch.find("args")->dump(), R"(["--flag","two words"])");
    EXPECT_EQ(launch.find("env")->dump(), R"(["FROST_RUN_TEST=1"])");
    ASSERT_NE(launch.find("initCommands"), nullptr) << "the environment is not merged";
    EXPECT_EQ(launch.find("initCommands")->dump(), R"(["settings set target.inherit-env false"])");

    target.workingDirectory.clear();
    target.inheritEnvironment = true;
    target.name.clear();
    const rpc::Json defaults = lldbDapLaunchArguments(target);
    EXPECT_EQ(defaults.stringAt("cwd"), workPath("App/x64/Debug")) << "the program's folder";
    EXPECT_EQ(defaults.stringAt("name"), "App.exe");
    EXPECT_EQ(defaults.find("initCommands"), nullptr);
}

TEST(LldbDap, TheFactoryStartsLldbDapThroughTheLauncher) {
    io::ScriptedProcessLauncher launcher;
    const std::string exe = workPath("sdk/" + kExe);
    launcher.expect({.expectExecutable = exe});
    AdapterFactory factory = lldbDapAdapterFactory(launcher, exe);
    std::atomic<bool> closed = false;
    auto client = factory(DapHandlers{.onClosed = [&](const Error&) { closed = true; }});
    ASSERT_TRUE(client.has_value()) << client.error().describe();
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    EXPECT_EQ(launches[0].spec.arguments, (std::vector<std::string>{"--no-lldbinit"}));
    EXPECT_TRUE(removes(launches[0].spec, "_NT_SYMBOL_PATH"));
    EXPECT_TRUE(launches[0].spec.killOnClose);
    // The scripted process exits at once: the client sees the end of its output.
    EXPECT_TRUE(test::waitUntil([&] { return closed.load(); }));
    client->reset();

    launcher.expect({.expectExecutable = exe, .launchError = Error(ErrorCode::NotFound, "no such file")});
    auto failed = factory(DapHandlers{});
    ASSERT_FALSE(failed.has_value());
    EXPECT_NE(failed.error().describe().find("lldb-dap"), std::string::npos) << failed.error().describe();
}

class DebugTrust : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(fs.createDirectories(workPath("Trusted/x64/Debug")).has_value());
        ASSERT_TRUE(fs.createDirectories(workPath("Untrusted/x64/Debug")).has_value());
        ASSERT_TRUE(fs.createDirectories(workPath("data")).has_value());
        auto opened = project::TrustStore::open(fs, workPath("data/trusted-roots.txt"));
        ASSERT_TRUE(opened.has_value()) << opened.error().describe();
        trust = std::move(*opened);
        ASSERT_TRUE(trust->trust(workPath("Trusted")).has_value());
    }

    // A factory that counts its calls and connects to `adapter`.
    AdapterFactory countingFactory() {
        return [this](DapHandlers handlers) -> Result<std::unique_ptr<IDebugAdapterClient>> {
            ++factoryCalls;
            return makeDapClient(adapter.transport(), std::move(handlers));
        };
    }

    ManualClock clock;
    io::MemFs fs{clock};
    std::unique_ptr<project::TrustStore> trust;
    FakeDebugAdapter adapter;
    int factoryCalls = 0;
};

TEST_F(DebugTrust, SourceMapRequiresAbsolutePrefixAndTrustedExistingDebugRoot) {
    ASSERT_TRUE(fs.createDirectories(workPath("Original")).has_value());
    const std::vector<std::string> roots{workPath("Trusted")};
    auto mapping = makeSourceMap(fs, *trust, workPath("Original"), workPath("Trusted"), roots);
    ASSERT_TRUE(mapping) << mapping.error().describe();
    DebugTarget target{.program = workPath("Trusted/x64/Debug/App.exe"), .trustRoots = roots};
    target.sourceMap = *mapping;
    const auto launch = lldbDapLaunchArguments(target);
    ASSERT_NE(launch.find("sourceMap"), nullptr);
    EXPECT_EQ(
        launch.find("sourceMap")->dump(),
        rpc::Json::array({rpc::Json::array({rpc::Json(workPath("Original")), rpc::Json(workPath("Trusted"))})}).dump());
    EXPECT_FALSE(makeSourceMap(fs, *trust, "relative", workPath("Trusted"), roots));
    EXPECT_TRUE(makeSourceMap(fs, *trust, workPath("missing"), workPath("Trusted"), roots));
    EXPECT_FALSE(makeSourceMap(fs, *trust, workPath("Trusted"), workPath("Trusted"), roots));
    EXPECT_FALSE(makeSourceMap(fs, *trust, workPath("Original"), workPath("Untrusted"), roots));
    target.trustRoots = {workPath("Untrusted")};
    EXPECT_FALSE(startDebugSession(*trust, target, countingFactory(), {}, {}));
    EXPECT_EQ(factoryCalls, 0);
    target.trustRoots = roots;
    ASSERT_TRUE(trust->untrust(workPath("Trusted")));
    EXPECT_FALSE(startDebugSession(*trust, target, countingFactory(), {}, {}));
    EXPECT_EQ(factoryCalls, 0);
}

TEST_F(DebugTrust, AnUntrustedRootIsRefusedBeforeAnyAdapterStarts) {
    io::ScriptedProcessLauncher launcher;
    DebugTarget target{.program = workPath("Untrusted/x64/Debug/App.exe")};
    auto refused = startDebugSession(*trust, target, lldbDapAdapterFactory(launcher, workPath("sdk/" + kExe)), {}, {});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::Untrusted);
    EXPECT_NE(refused.error().message().find(workPath("Untrusted/x64/Debug")), std::string::npos);
    EXPECT_TRUE(launcher.launches().empty()) << "no lldb-dap was started";

    // A trust root that is not trusted refuses even when the program's folder is.
    target.program = workPath("Trusted/x64/Debug/App.exe");
    target.trustRoots = {workPath("Trusted"), workPath("Untrusted")};
    auto partly = startDebugSession(*trust, target, countingFactory(), {}, {});
    ASSERT_FALSE(partly.has_value());
    EXPECT_EQ(partly.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(factoryCalls, 0);

    target.program = "App.exe";
    EXPECT_EQ(startDebugSession(*trust, target, countingFactory(), {}, {}).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(factoryCalls, 0);
}

TEST_F(DebugTrust, AttachChecksTrustAndNeverLaunchesOrTerminatesTheHost) {
    DebugTarget target{.program = workPath("Untrusted/x64/Debug/App.exe"), .trustRoots = {workPath("Untrusted")}};
    auto rejected = attachDebugSession(*trust, target, countingFactory(), rpc::Json::object(), {}, {});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(factoryCalls, 0);

    target.program = workPath("Trusted/x64/Debug/App.exe");
    target.trustRoots = {workPath("Trusted")};
    scriptProgram(adapter, FakeProgram{.runsForever = true});
    adapter.on("attach",
               [](const rpc::Json&) { return FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json()}}}; });
    test::SessionRecorder recorder;
    auto attached = attachDebugSession(*trust, target, countingFactory(), rpc::Json::object().set("marker", 17), {},
                                       recorder.callbacks());
    ASSERT_TRUE(attached);
    ASSERT_TRUE(adapter.waitForRequest("attach"));
    EXPECT_EQ(adapter.argumentsOf("attach").front().integerAt("marker"), 17);
    EXPECT_TRUE(adapter.argumentsOf("launch").empty());
    (*attached)->stop();
    ASSERT_TRUE(test::waitUntil([&] { return recorder.hasEnded(); }));
    const auto disconnects = adapter.argumentsOf("disconnect");
    ASSERT_EQ(disconnects.size(), 1U);
    EXPECT_EQ(disconnects.front().boolAt("terminateDebuggee"), false);
}

TEST_F(DebugTrust, ATrustedRootStartsTheSession) {
    scriptProgram(adapter, FakeProgram{.sourcePath = workPath("Trusted/main.cpp"), .codeLines = {}});
    DebugTarget target{.program = workPath("Trusted/x64/Debug/App.exe"), .trustRoots = {workPath("Trusted")}};
    test::SessionRecorder recorder;
    auto started = startDebugSession(*trust, target, countingFactory(), {}, recorder.callbacks());
    ASSERT_TRUE(started.has_value()) << started.error().describe();
    EXPECT_EQ(factoryCalls, 1);
    ASSERT_TRUE(test::waitUntil([&] { return recorder.hasEnded(); })) << "the scripted program exits at once";
    EXPECT_EQ(adapter.argumentsOf("launch").front().stringAt("program"), target.program);
    EXPECT_EQ((*started)->outcome()->exitCode, 0);
}

} // namespace
} // namespace frost::debug
