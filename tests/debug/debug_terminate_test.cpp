// SPDX-License-Identifier: Apache-2.0
// Explicit PID-bound DAP termination, including refusal without an adapter/process kill fallback.
#include "frost/debug/debug_session.hpp"
#include "frost/debug/fake_debug_adapter.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>

namespace frost::debug {
namespace {
using test::fakeFactory;
using test::waitUntil;
using Reply = FakeDebugAdapter::Reply;

class DebugTerminateTest : public ::testing::Test {
protected:
    void start(bool supportsTerminate = true, DebugTargetMode mode = DebugTargetMode::Launch) {
        scriptProgram(adapter, FakeProgram{.runsForever = true});
        adapter.on("initialize", [supportsTerminate](const rpc::Json&) {
            return Reply{.body = rpc::Json::object()
                                     .set("supportsConfigurationDoneRequest", true)
                                     .set("supportsTerminateRequest", supportsTerminate)};
        });
        adapter.on("attach", [](const rpc::Json&) { return Reply{.events = {{"initialized", rpc::Json()}}}; });
        session = DebugSession::start(fakeFactory(adapter), rpc::Json::object(), {}, {},
                                      DebugSessionOptions{.targetMode = mode});
        ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Running; }));
        adapter.sendEvent("process", rpc::Json::object().set("systemProcessId", 4321));
        ASSERT_TRUE(waitUntil([&] { return session->processId() == 4321; }));
        adapter.sendEvent("stopped", rpc::Json::object().set("reason", "pause").set("threadId", 1));
        ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Stopped; }));
    }
    FakeDebugAdapter adapter;
    std::unique_ptr<DebugSession> session;
};

TEST_F(DebugTerminateTest, ExactPidIsRequiredAndAnAckDoesNotInventTermination) {
    start();
    auto refused = session->terminateDebuggee(4322);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::PermissionDenied);
    EXPECT_TRUE(adapter.argumentsOf("terminate").empty());
    EXPECT_TRUE(adapter.argumentsOf("disconnect").empty());
    std::promise<bool> response;
    auto received = response.get_future();
    ASSERT_TRUE(session->terminateDebuggee(4321, [&response](auto result) { response.set_value(result.has_value()); }));
    ASSERT_EQ(received.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(received.get());
    EXPECT_EQ(session->state(), DebugState::Stopped);
    EXPECT_FALSE(session->outcome());
    ASSERT_EQ(adapter.argumentsOf("terminate").size(), 1U);
    EXPECT_TRUE(adapter.argumentsOf("disconnect").empty());
    EXPECT_FALSE(session->canTerminateDebuggee());
    adapter.sendEvent("terminated", rpc::Json::object());
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Ended; }));
    ASSERT_TRUE(session->outcome());
    EXPECT_TRUE(session->outcome()->stopped);
}

TEST_F(DebugTerminateTest, RefusedTerminateLeavesTheActualSessionAliveAndRetryable) {
    start();
    adapter.on("terminate", [](const rpc::Json&) { return Reply{.success = false, .message = "terminate refused"}; });
    std::promise<bool> response;
    auto received = response.get_future();
    ASSERT_TRUE(session->terminateDebuggee(4321, [&response](auto result) { response.set_value(result.has_value()); }));
    ASSERT_EQ(received.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_FALSE(received.get());
    EXPECT_EQ(session->state(), DebugState::Stopped);
    EXPECT_TRUE(session->canTerminateDebuggee());
    EXPECT_TRUE(adapter.argumentsOf("disconnect").empty());
    EXPECT_FALSE(session->outcome());
}

TEST_F(DebugTerminateTest, LaunchMayUseStandardDapDisconnectTerminationWhenTerminateIsUnavailable) {
    start(false);
    ASSERT_TRUE(session->canTerminateDebuggee());
    ASSERT_TRUE(session->terminateDebuggee(4321));
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Ended; }));
    EXPECT_TRUE(adapter.argumentsOf("terminate").empty());
    ASSERT_EQ(adapter.argumentsOf("disconnect").size(), 1U);
    EXPECT_EQ(adapter.argumentsOf("disconnect").front().boolAt("terminateDebuggee"), true);
}

TEST_F(DebugTerminateTest, FailedDisconnectFallbackDoesNotCloseOrKillTheAdapter) {
    start(false);
    adapter.on("disconnect", [](const rpc::Json&) { return Reply{.success = false, .message = "disconnect refused"}; });
    std::promise<bool> response;
    auto received = response.get_future();
    ASSERT_TRUE(session->terminateDebuggee(4321, [&response](auto result) { response.set_value(result.has_value()); }));
    ASSERT_EQ(received.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_FALSE(received.get());
    EXPECT_EQ(session->state(), DebugState::Stopped);
    EXPECT_TRUE(session->canTerminateDebuggee());
    EXPECT_FALSE(session->outcome());
}

TEST_F(DebugTerminateTest, OrdinaryStopDuringAnUnansweredTerminateStillReportsStopped) {
    start();
    adapter.on("terminate", [](const rpc::Json&) { return Reply{.respond = false}; });
    ASSERT_TRUE(session->terminateDebuggee(4321));
    ASSERT_TRUE(adapter.waitForRequest("terminate"));
    session->stop();
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Ended; }));
    ASSERT_TRUE(session->outcome());
    EXPECT_TRUE(session->outcome()->stopped);
}

TEST_F(DebugTerminateTest, AttachWithoutTerminateCapabilityRefusesAndOrdinaryStopStillDetaches) {
    start(false, DebugTargetMode::Attach);
    EXPECT_FALSE(session->canTerminateDebuggee());
    auto refused = session->terminateDebuggee(4321);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::Unsupported);
    EXPECT_TRUE(adapter.argumentsOf("terminate").empty());
    EXPECT_TRUE(adapter.argumentsOf("disconnect").empty());
    session->stop();
    ASSERT_TRUE(waitUntil([&] { return session->state() == DebugState::Ended; }));
    ASSERT_EQ(adapter.argumentsOf("disconnect").size(), 1U);
    EXPECT_EQ(adapter.argumentsOf("disconnect").front().boolAt("terminateDebuggee"), false);
}
} // namespace
} // namespace frost::debug
