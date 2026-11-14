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
}
}
