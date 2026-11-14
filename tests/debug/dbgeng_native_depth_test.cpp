// SPDX-License-Identifier: Apache-2.0
// Authored real System32 DbgEng depth oracles. No mock engine values; execution is deferred.
#include "../../src/dbgeng/protocol.hpp"
#include "dbgeng_native_peer.hpp"
#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <algorithm>
namespace frost::debug {
namespace {
using namespace std::chrono_literals;
using native_test::Peer;
using Json = rpc::Json;
class NativeDepth : public testing::Test {
protected:
    frost::test::ScratchDir scratch{"dbgeng-depth"};
    std::string program;
    void SetUp() override {
        auto built = native_test::buildNativeFixture(FROST_DBGENG_DEPTH_SOURCE, scratch);
        ASSERT_TRUE(built) << built.error().describe();
        program = *built;
    }
    void launch(Peer& peer,
                Json points = Json::array().push(Json::object().set("name", "FrostNativeDepthCheckpoint"))) {
        auto initialized = peer.start(FROST_DBGENG_ADAPTER_PATH);
        ASSERT_TRUE(initialized);
        ASSERT_EQ(initialized->boolAt("supportsDataBreakpoints"), true);
        DbgEngTarget target;
        target.native.program = program;
        auto args = dbgEngDapLaunchArguments(target);
        ASSERT_TRUE(args);
        auto launched = peer.request("launch", *args);
        ASSERT_TRUE(launched) << peer.text();
        auto bound = peer.request("setFunctionBreakpoints", Json::object().set("breakpoints", std::move(points)));
        ASSERT_TRUE(bound) << peer.text();
        for (const auto& point : bound->find("breakpoints")->items())
            ASSERT_EQ(point.boolAt("verified"), true) << point.dump();
        ASSERT_TRUE(peer.request("configurationDone"));
        ASSERT_TRUE(test::waitUntil([&] { return peer.count("stopped") > 0; }, 60s)) << peer.text();
    }
    Json top(Peer& peer) {
        auto trace =
            peer.request("stackTrace", Json::object().set("threadId", *peer.last("stopped")->integerAt("threadId")));
        EXPECT_TRUE(trace);
        if (!trace)
            return {};
        return trace->find("stackFrames")->items().front();
    }
};
TEST_F(NativeDepth, NativeMemoryRegisterDisassemblyAndModuleEvidence) {
    Peer peer;
    launch(peer);
    ASSERT_FALSE(HasFatalFailure());
    auto frame = top(peer);
    auto value = peer.request("evaluate",
                              Json::object().set("expression", "nativeValue").set("frameId", *frame.integerAt("id")));
    ASSERT_TRUE(value) << peer.text();
    ASSERT_FALSE(value->stringAt("memoryReference").empty());
    const std::string location(value->stringAt("memoryReference"));
    auto memory = peer.request("readMemory", Json::object().set("memoryReference", location).set("count", 4));
    ASSERT_TRUE(memory);
    EXPECT_EQ(memory->stringAt("data"), "BwAAAA==");
    auto written = peer.request("writeMemory", Json::object().set("memoryReference", location).set("data", "CQAAAA=="));
    ASSERT_TRUE(written);
    EXPECT_EQ(written->integerAt("bytesWritten"), 4);
    auto registers = peer.request("frost.registers");
    ASSERT_TRUE(registers);
    EXPECT_GT(registers->find("registers")->size(), 0U);
    auto assembly = peer.request("disassemble",
                                 Json::object()
                                     .set("memoryReference", std::string(frame.stringAt("instructionPointerReference")))
                                     .set("instructionCount", 4));
    ASSERT_TRUE(assembly);
    EXPECT_EQ(assembly->find("instructions")->size(), 4U);
    auto modules = peer.request("modules");
    ASSERT_TRUE(modules);
    EXPECT_TRUE(std::ranges::any_of(modules->find("modules")->items(), [](const auto& item) {
        return item.stringAt("path").find("fixture.exe") != std::string_view::npos;
    }));
    auto thread = *peer.last("stopped")->integerAt("threadId");
    auto frozen = peer.request("frost.setThreadFrozen", Json::object().set("threadId", thread).set("frozen", true));
    ASSERT_TRUE(frozen);
    EXPECT_EQ(frozen->boolAt("commandSucceeded"), true);
    EXPECT_EQ(frozen->find("frozen"), nullptr);
    ASSERT_TRUE(peer.request("frost.setThreadFrozen", Json::object().set("threadId", thread).set("frozen", false)));
    ASSERT_TRUE(peer.request("setFunctionBreakpoints", Json::object().set("breakpoints", Json::array())));
    ASSERT_TRUE(peer.request("continue", Json::object().set("threadId", thread)));
    ASSERT_TRUE(test::waitUntil([&] { return peer.count("exited") > 0; }, 60s));
    EXPECT_EQ(peer.last("exited")->integerAt("exitCode"), 0);
    EXPECT_NE(peer.text().find("iteration=0 value=9"), std::string::npos) << peer.text();
    EXPECT_GT(peer.count("continued"), 0U);
}
TEST_F(NativeDepth, NativeConditionAndHitCountUseActualLoopHits) {
    Peer peer;
    launch(peer, Json::array().push(Json::object()
                                        .set("name", "FrostNativeDepthCheckpoint")
                                        .set("condition", "nativeValue == 9")
                                        .set("hitCondition", ">=3")));
    ASSERT_FALSE(HasFatalFailure());
    auto frame = top(peer);
    auto value = peer.request("evaluate",
                              Json::object().set("expression", "nativeValue").set("frameId", *frame.integerAt("id")));
    ASSERT_TRUE(value);
    EXPECT_NE(value->stringAt("result").find('9'), std::string_view::npos);
    EXPECT_EQ(peer.count("stopped"), 1U);
    EXPECT_NE(peer.text().find("iteration=1 value=8"), std::string::npos);
}
TEST_F(NativeDepth, HardwareWatchpointUsesAResolvedNativeGlobal) {
    Peer peer;
    launch(peer);
    ASSERT_FALSE(HasFatalFailure());
    auto data = peer.request("dataBreakpointInfo", Json::object().set("name", "nativeValue"));
    ASSERT_TRUE(data);
    ASSERT_FALSE(data->stringAt("dataId").empty());
    auto installed = peer.request(
        "setDataBreakpoints",
        Json::object().set(
            "breakpoints",
            Json::array().push(Json::object().set("dataId", data->stringAt("dataId")).set("accessType", "write"))));
    ASSERT_TRUE(installed);
    ASSERT_EQ(installed->find("breakpoints")->items().front().boolAt("verified"), true);
    ASSERT_TRUE(peer.request("setFunctionBreakpoints", Json::object().set("breakpoints", Json::array())));
    auto before = peer.count("stopped");
    ASSERT_TRUE(peer.request("continue", Json::object().set("threadId", *peer.last("stopped")->integerAt("threadId"))));
    ASSERT_TRUE(test::waitUntil([&] { return peer.count("stopped") > before; }, 60s));
    EXPECT_EQ(peer.last("stopped")->stringAt("reason"), "data breakpoint");
    EXPECT_FALSE(peer.request("dataBreakpointInfo",
                              Json::object().set("variablesReference", 999999).set("name", "nativeValue")));
}
TEST_F(NativeDepth, NativeWritesToARealArgumentReachTheProgram) {
    Peer peer;
    auto initialized = peer.start(FROST_DBGENG_ADAPTER_PATH);
    ASSERT_TRUE(initialized);
    DbgEngTarget target;
    target.native.program = program;
    auto arguments = dbgEngDapLaunchArguments(target);
    ASSERT_TRUE(arguments);
    ASSERT_TRUE(peer.request("launch", *arguments));
    auto source = io::realFileSystem().readFile(FROST_DBGENG_DEPTH_SOURCE);
    ASSERT_TRUE(source);
    const auto marker = source->find("DBGENG_DEPTH_WRITE_MARKER");
    ASSERT_NE(marker, std::string::npos);
    const auto line = 1 + std::count(source->begin(), source->begin() + static_cast<std::ptrdiff_t>(marker), '\n');
    auto points =
        peer.request("setBreakpoints", Json::object()
                                           .set("source", Json::object().set("path", FROST_DBGENG_DEPTH_SOURCE))
                                           .set("breakpoints", Json::array().push(Json::object().set("line", line))));
    ASSERT_TRUE(points);
    ASSERT_EQ(points->find("breakpoints")->items().front().boolAt("verified"), true);
    ASSERT_TRUE(peer.request("configurationDone"));
    ASSERT_TRUE(test::waitUntil([&] { return peer.count("stopped") > 0; }, 60s));
    auto frame = top(peer);
    auto scopes = peer.request("scopes", Json::object().set("frameId", *frame.integerAt("id")));
    ASSERT_TRUE(scopes);
    auto args = std::ranges::find_if(scopes->find("scopes")->items(), [](const auto& scope) {
        return scope.stringAt("presentationHint") == "arguments";
    });
    ASSERT_NE(args, scopes->find("scopes")->items().end());
    auto set = peer.request("setVariable", Json::object()
                                               .set("variablesReference", *args->integerAt("variablesReference"))
                                               .set("name", "iteration")
                                               .set("value", "99"));
    ASSERT_TRUE(set) << peer.text();
    ASSERT_TRUE(peer.request("setBreakpoints", Json::object()
                                                   .set("source", Json::object().set("path", FROST_DBGENG_DEPTH_SOURCE))
                                                   .set("breakpoints", Json::array())));
    ASSERT_TRUE(peer.request("continue", Json::object().set("threadId", *peer.last("stopped")->integerAt("threadId"))));
    ASSERT_TRUE(test::waitUntil([&] { return peer.count("exited") > 0; }, 60s));
    EXPECT_NE(peer.text().find("iteration=99 value=7"), std::string::npos) << peer.text();
}
} // namespace

TEST_F(NativeDepth, LogpointsUseRealValuesAndNeverInventAStoppedEvent) {
    Peer peer;
    ASSERT_TRUE(peer.start(FROST_DBGENG_ADAPTER_PATH));
    DbgEngTarget target;
    target.native.program = program;
    auto args = dbgEngDapLaunchArguments(target);
    ASSERT_TRUE(args);
    ASSERT_TRUE(peer.request("launch", *args));
    auto points = peer.request(
        "setFunctionBreakpoints",
        Json::object().set(
            "breakpoints",
            Json::array().push(
                Json::object().set("name", "FrostNativeDepthCheckpoint").set("logMessage", "hit {nativeValue}"))));
    ASSERT_TRUE(points);
    ASSERT_EQ(points->find("breakpoints")->items().front().boolAt("verified"), true);
    ASSERT_TRUE(peer.request("configurationDone"));
    ASSERT_TRUE(test::waitUntil([&] { return peer.count("exited") > 0; }, 60s)) << peer.text();
    EXPECT_EQ(peer.count("stopped"), 0U);
    for (auto expected : {"hit 7", "hit 8", "hit 9", "hit 10"})
        EXPECT_NE(peer.text().find(expected), std::string::npos) << peer.text();
}
TEST_F(NativeDepth, AttachDisconnectDetachesAndExplicitTerminateEndsOnlyTheAttachedTarget) {
    io::ProcessSpec spec;
    spec.executable = program;
    spec.arguments = {"--attach", scratch.join("ready")};
    auto child = io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(child);
    ASSERT_TRUE(
        test::waitUntil([&] { return io::realFileSystem().exists(scratch.join("ready")).value_or(false); }, 10s));
    const auto pid = (*child)->pid();
    ASSERT_GT(pid, 0U);
    {
        Peer peer;
        ASSERT_TRUE(peer.start(FROST_DBGENG_ADAPTER_PATH));
        DbgEngTarget target;
        target.processId = static_cast<std::uint32_t>(pid);
        target.native.stopOnEntry = true;
        auto args = dbgEngDapLaunchArguments(target);
        ASSERT_TRUE(args);
        ASSERT_TRUE(peer.request("attach", *args)) << peer.text();
        ASSERT_TRUE(peer.request("configurationDone"));
        ASSERT_TRUE(test::waitUntil([&] { return peer.count("stopped") > 0; }, 60s));
        ASSERT_TRUE(peer.request("disconnect", Json::object().set("terminateDebuggee", false)));
    }
    auto alive = (*child)->wait(100ms);
    ASSERT_FALSE(alive);
    EXPECT_EQ(alive.error().code(), ErrorCode::Timeout);
    Peer second;
    ASSERT_TRUE(second.start(FROST_DBGENG_ADAPTER_PATH));
    DbgEngTarget target;
    target.processId = static_cast<std::uint32_t>(pid);
    target.native.stopOnEntry = true;
    auto args = dbgEngDapLaunchArguments(target);
    ASSERT_TRUE(args);
    ASSERT_TRUE(second.request("attach", *args));
    ASSERT_TRUE(second.request("terminate"));
    EXPECT_TRUE((*child)->wait(10s));
}
} // namespace frost::debug
