// SPDX-License-Identifier: Apache-2.0
// Source-only R9 protocol fixtures. Execution is intentionally deferred by the user.
#include "frost/debug/debug_session.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "debug_test_util.hpp"
#include <gtest/gtest.h>
#include <future>

namespace frost::debug {
namespace {
using test::waitUntil;
TEST(DebugDepthProtocol, OptionalCapabilitiesRemainFalseAndPartialUpdatesPreserveOthers) {
    auto caps = dap::parseCapabilities(rpc::Json::object().set("supportsReadMemoryRequest", true)
        .set("supportsStepInTargetsRequest", true).set("supportsDataBreakpoints", true));
    EXPECT_TRUE(caps.supportsReadMemoryRequest);
    EXPECT_TRUE(caps.supportsStepInTargetsRequest);
    EXPECT_FALSE(caps.supportsSingleThreadExecutionRequests);
    caps = dap::mergeCapabilities(caps, rpc::Json::object().set("capabilities",
        rpc::Json::object().set("supportsDataBreakpoints", false)));
    EXPECT_TRUE(caps.supportsReadMemoryRequest);
    EXPECT_TRUE(caps.supportsStepInTargetsRequest);
    EXPECT_FALSE(caps.supportsDataBreakpoints);
}
TEST(DebugDepthProtocol, ThreadFreezeStateIsUnknownUnlessExplicitlyReported) {
    EXPECT_FALSE(dap::parseThread(rpc::Json::object().set("id", 17)).frozen.has_value());
    EXPECT_EQ(dap::parseThread(rpc::Json::object().set("id", 17).set("frostFrozen", true)).frozen, true);
    EXPECT_EQ(dap::parseThread(rpc::Json::object().set("id", 17).set("frostFrozen", false)).frozen, false);
    EXPECT_FALSE(dap::parseThread(rpc::Json::object().set("id", 17).set("frostFrozen", "false")).frozen.has_value());
}
TEST(DebugDepthProtocol, MemoryReferenceRemainsOpaqueAndIsNeverInferredFromValue) {
    EXPECT_TRUE(dap::parseVariable(rpc::Json::object().set("value", "0x1234")).memoryReference.empty());
    EXPECT_EQ(dap::parseVariable(rpc::Json::object().set("value", "pretty pointer")
        .set("memoryReference", "adapter:heap/17")).memoryReference, "adapter:heap/17");
    EXPECT_EQ(dap::parseEvaluateResult(rpc::Json::object().set("result", "object")
        .set("memoryReference", "handle/3"), "p").memoryReference, "handle/3");
}
TEST(DebugDepthSession, MissingCapabilitiesRefuseWithoutSendingOptionalRequests) {
    FakeDebugAdapter adapter;
    scriptProgram(adapter, FakeProgram{});
    auto session = DebugSession::start(test::fakeFactory(adapter), rpc::Json::object(), {}, {});
    ASSERT_TRUE(waitUntil([&] { return session->state() != DebugState::Starting; }));
    bool answered = false;
    session->stepInTargets([&](Result<rpc::Json> result) {
        answered = true; ASSERT_FALSE(result); EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    });
    EXPECT_TRUE(answered);
    EXPECT_TRUE(adapter.argumentsOf("stepInTargets").empty());
    EXPECT_FALSE(session->stepIntoTarget(1));
    session->setThreadFrozen(1, true, [&](Result<rpc::Json> result) {
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    });
    EXPECT_TRUE(adapter.argumentsOf("frost.setThreadFrozen").empty());
    session->readMemory("opaque", 16, [&](Result<dap::ReadMemoryResult> result) {
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
    });
    EXPECT_TRUE(adapter.argumentsOf("readMemory").empty());
}
TEST(DebugDepthSession, SelectedCallUsesAdapterIdAndExpiredTargetIsRejected) {
    FakeDebugAdapter adapter;
    const auto path = test::workPath("calls.cpp");
    scriptProgram(adapter, FakeProgram{.sourcePath = path, .codeLines = {10}, .runsForever = true});
    adapter.on("initialize", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.body =
        rpc::Json::object().set("supportsConfigurationDoneRequest", true).set("supportsStepInTargetsRequest", true)}; });
    adapter.on("stepInTargets", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.body =
        rpc::Json::object().set("targets", rpc::Json::array({rpc::Json::object().set("id", 73).set("label", "inner(x)")}))}; });
    auto session = DebugSession::start(test::fakeFactory(adapter), rpc::Json::object(), {{path, {{1, 10}}}}, {});
    ASSERT_TRUE(waitUntil([&] { return session->selectedFrame().has_value(); }));
    std::promise<Result<rpc::Json>> promise;
    auto future = promise.get_future();
    session->stepInTargets([&](Result<rpc::Json> result) { promise.set_value(std::move(result)); });
    ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_TRUE(future.get());
    EXPECT_FALSE(session->stepIntoTarget(999));
    ASSERT_TRUE(session->selectThread(2));
    ASSERT_TRUE(waitUntil([&] { return session->selectedFrame().has_value(); }));
    EXPECT_FALSE(session->stepIntoTarget(73));
    EXPECT_TRUE(adapter.argumentsOf("stepIn").empty());
    ASSERT_TRUE(session->selectThread(1));
    std::promise<Result<rpc::Json>> refreshed;
    auto result = refreshed.get_future();
    session->stepInTargets([&](Result<rpc::Json> body) { refreshed.set_value(std::move(body)); });
    ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_TRUE(result.get());
    ASSERT_TRUE(session->stepIntoTarget(73));
    ASSERT_TRUE(adapter.waitForRequest("stepIn"));
    EXPECT_EQ(adapter.argumentsOf("stepIn").front().integerAt("targetId"), 73);
    EXPECT_EQ(adapter.argumentsOf("stepIn").front().integerAt("threadId"), 1);
}
TEST(DebugDepthSession, HardwareBreakpointResponseKeepsAdapterVerificationAndOpaqueDataId) {
    FakeDebugAdapter adapter;
    const auto path = test::workPath("watch.cpp");
    scriptProgram(adapter, FakeProgram{.sourcePath = path, .codeLines = {10}});
    adapter.on("initialize", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.body =
        rpc::Json::object().set("supportsConfigurationDoneRequest", true).set("supportsDataBreakpoints", true)}; });
    adapter.on("dataBreakpointInfo", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.body =
        rpc::Json::object().set("dataId", "opaque:heap/9").set("description", "obj.count")
            .set("accessTypes", rpc::Json::array({"write", "readWrite"})).set("canPersist", false)}; });
    adapter.on("setDataBreakpoints", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.body =
        rpc::Json::object().set("breakpoints", rpc::Json::array({rpc::Json::object()
            .set("verified", false).set("message", "No hardware slots available")}))}; });
    auto session = DebugSession::start(test::fakeFactory(adapter), rpc::Json::object(), {{path, {{1, 10}}}}, {});
    ASSERT_TRUE(waitUntil([&] { return session->selectedFrame().has_value(); }));
    std::promise<Result<rpc::Json>> info;
    auto infoResult = info.get_future();
    session->dataBreakpointInfo(100, "count", [&](Result<rpc::Json> result) { info.set_value(std::move(result)); });
    ASSERT_EQ(infoResult.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto body = infoResult.get(); ASSERT_TRUE(body);
    EXPECT_EQ(body->stringAt("dataId"), "opaque:heap/9");
    auto list = rpc::Json::array({rpc::Json::object().set("dataId", body->stringAt("dataId"))
        .set("accessType", "write")});
    std::promise<Result<rpc::Json>> configured;
    auto configuredResult = configured.get_future();
    session->setDataBreakpoints(std::move(list), [&](Result<rpc::Json> result) { configured.set_value(std::move(result)); });
    ASSERT_EQ(configuredResult.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto response = configuredResult.get(); ASSERT_TRUE(response);
    ASSERT_NE(response->find("breakpoints"), nullptr);
    const auto result = dap::parseBreakpoint(response->find("breakpoints")->items().front());
    EXPECT_FALSE(result.verified);
    EXPECT_EQ(result.message, "No hardware slots available");
    EXPECT_EQ(adapter.argumentsOf("dataBreakpointInfo").front().integerAt("variablesReference"), 100);
    EXPECT_EQ(adapter.argumentsOf("dataBreakpointInfo").front().stringAt("name"), "count");
}

TEST(DebugDepthSession, FreezeReceiptDoesNotInventPersistentThreadState) {
    FakeDebugAdapter adapter;
    const auto path = test::workPath("threads.cpp");
    scriptProgram(adapter, FakeProgram{.sourcePath = path, .codeLines = {10}});
    adapter.on("initialize", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.body =
        rpc::Json::object().set("supportsConfigurationDoneRequest", true).set("frostSupportsThreadFreeze", true)}; });
    adapter.on("frost.setThreadFrozen", [](const rpc::Json& arguments) { return FakeDebugAdapter::Reply{.body =
        rpc::Json::object().set("threadId", *arguments.integerAt("threadId"))
            .set("operationApplied", true).set("commandSucceeded", true)}; });
    auto session = DebugSession::start(test::fakeFactory(adapter), rpc::Json::object(), {{path, {{1, 10}}}}, {});
    ASSERT_TRUE(waitUntil([&] { return session->selectedFrame().has_value(); }));
    std::promise<Result<rpc::Json>> receipt;
    auto result = receipt.get_future();
    session->setThreadFrozen(2, true, [&](Result<rpc::Json> body) { receipt.set_value(std::move(body)); });
    ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    auto body = result.get(); ASSERT_TRUE(body);
    EXPECT_FALSE(body->boolAt("frozen").has_value());
    EXPECT_EQ(adapter.argumentsOf("frost.setThreadFrozen").front().integerAt("threadId"), 2);
    EXPECT_EQ(adapter.argumentsOf("frost.setThreadFrozen").front().boolAt("frozen"), true);
    for (const auto& thread : session->threads()) EXPECT_FALSE(thread.frozen.has_value());
    bool rejected = false;
    session->setThreadFrozen(999, false, [&](Result<rpc::Json> response) {
        rejected = true; ASSERT_FALSE(response); EXPECT_EQ(response.error().code(), ErrorCode::NotFound);
    });
    EXPECT_TRUE(rejected);
    EXPECT_EQ(adapter.argumentsOf("frost.setThreadFrozen").size(), 1U);
}

}
}
