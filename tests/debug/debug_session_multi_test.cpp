// SPDX-License-Identifier: Apache-2.0
// Session configuration barrier and passive dump invariants at the actual DAP request boundary.
#include "frost/debug/debug_session.hpp"
#include "frost/debug/fake_debug_adapter.hpp"
#include "frost/rpc/framing.hpp"

#include "debug_test_util.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <future>
namespace frost::debug {
namespace {
TEST(DebugCancelledAttach, LocalPreparationCancellationAllowsEitherAcknowledgementToRemainPending) {
    for (const bool holdAttach : {false, true}) {
        SCOPED_TRACE(holdAttach ? "attach response pending" : "configurationDone response pending");
        FakeDebugAdapter adapter;
        scriptProgram(adapter, FakeProgram{.runsForever = true});
        adapter.on("attach", [holdAttach](const rpc::Json&) {
            return FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json::object()}}, .respond = !holdAttach};
        });
        if (!holdAttach)
            adapter.on("configurationDone", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.respond = false}; });
        std::atomic<bool> configured{false};
        DebugSessionCallbacks callbacks;
        callbacks.onConfigured = [&] { configured.store(true); };
        DebugSessionOptions options;
        options.adapterId = "frost-lua";
        options.targetMode = DebugTargetMode::Attach;
        auto session = DebugSession::start(test::fakeFactory(adapter), rpc::Json::object(), {}, callbacks, options);
        ASSERT_TRUE(adapter.waitForRequest("configurationDone"));
        if (holdAttach)
            ASSERT_TRUE(test::waitUntil([&] { return configured.load(); }));
        ASSERT_TRUE(session->cancelAttachedPreparation());
        ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Ended; }));
        EXPECT_TRUE(adapter.argumentsOf("disconnect").empty());
        ASSERT_TRUE(session->outcome());
        EXPECT_TRUE(session->outcome()->stopped);
        EXPECT_FALSE(session->outcome()->error);
        EXPECT_FALSE(session->outcome()->disconnectConfirmed);
    }
}
TEST(DebugCancelledAttach, WireCompleteAttachRequiresOrdinaryDetachAndLaunchModeCannotUseLocalCancel) {
    FakeDebugAdapter adapter;
    scriptProgram(adapter, FakeProgram{.runsForever = true});
    adapter.on("attach", [](const rpc::Json&) {
        return FakeDebugAdapter::Reply{.events = {{"initialized", rpc::Json::object()}}};
    });
    std::atomic<bool> configured{false};
    DebugSessionCallbacks callbacks;
    callbacks.onConfigured = [&] { configured.store(true); };
    DebugSessionOptions options;
    options.targetMode = DebugTargetMode::Attach;
    auto session = DebugSession::start(test::fakeFactory(adapter), rpc::Json::object(), {}, callbacks, options);
    ASSERT_TRUE(test::waitUntil([&] { return configured.load(); }));
    auto refused = session->cancelAttachedPreparation();
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::Busy);
    EXPECT_TRUE(adapter.argumentsOf("disconnect").empty());
    session->stop();
    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Ended; }));
    ASSERT_FALSE(adapter.argumentsOf("disconnect").empty());
    EXPECT_EQ(adapter.argumentsOf("disconnect").front().boolAt("terminateDebuggee"), false);
    EXPECT_TRUE(session->outcome()->disconnectConfirmed);
    FakeDebugAdapter native;
    scriptProgram(native, FakeProgram{.runsForever = true});
    auto launched = DebugSession::start(test::fakeFactory(native), rpc::Json::object(), {}, {}, DebugSessionOptions{});
    EXPECT_EQ(launched->cancelAttachedPreparation().error().code(), ErrorCode::Unsupported);
}

TEST(DebugPrivateLaunch, StopDuringHeldFinalAdmissionCannotWriteTheQueuedLaunchAfterRelease) {
    FakeDebugAdapter adapter;
    scriptProgram(adapter, FakeProgram{.runsForever = true});
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto released = release.get_future().share();
    DebugSessionOptions options;
    options.privateSession = true;
    options.launchAdmission = [&]() -> Result<void> {
        entered.set_value();
        if (released.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
            return fail(ErrorCode::Timeout, "held admission fixture timed out");
        return {};
    };
    auto session = DebugSession::start(test::fakeFactory(adapter), rpc::Json::object(), {}, {}, options);
    struct ReleaseOnExit {
        std::promise<void>& promise;
        bool done = false;
        void now() {
            if (!done) {
                done = true;
                promise.set_value();
            }
        }
        ~ReleaseOnExit() { now(); }
    } guard{release}; // Released before session/transport join even on a failed assertion.
    ASSERT_EQ(enteredFuture.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    session->stop();
    guard.now();
    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Ended; }));
    EXPECT_TRUE(adapter.argumentsOf("launch").empty());
    ASSERT_FALSE(adapter.argumentsOf("disconnect").empty());
    ASSERT_TRUE(session->outcome());
    EXPECT_TRUE(session->outcome()->stopped);
    EXPECT_FALSE(session->outcome()->error);
    EXPECT_TRUE(session->outcome()->disconnectConfirmed); // Only the peer's actual disconnect ACK.
}

TEST(DebugPrivateLaunch, RevocationAfterInitializeQueuedSendsNoLaunchAndDoesNotExposePayloadInError) {
    FakeDebugAdapter adapter;
    scriptProgram(adapter, FakeProgram{.runsForever = true});
    adapter.on("initialize", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.respond = false}; });
    std::atomic<bool> admitted{true};
    DebugSessionOptions options;
    options.privateSession = true;
    options.launchAdmission = [&]() -> Result<void> {
        return admitted.load() ? Result<void>{}
                               : Result<void>(fail(ErrorCode::Untrusted, "revoked PRIVATE_BOOTSTRAP_SENTINEL"));
    };
    auto arguments = rpc::Json::object()
                         .set("program", test::workPath("Engine.exe"))
                         .set("env", rpc::Json::array().push("FROST_PRIVATE=PRIVATE_BOOTSTRAP_SENTINEL"));
    auto session = DebugSession::start(test::fakeFactory(adapter), std::move(arguments), {}, {}, options);
    ASSERT_TRUE(adapter.waitForRequest("initialize"));
    admitted.store(false);
    for (const auto& request : adapter.requests()) {
        if (request.stringAt("command") == "initialize")
            adapter.sendRaw(rpc::encodeFrame(rpc::Json::object()
                                                 .set("seq", 1000)
                                                 .set("type", "response")
                                                 .set("command", "initialize")
                                                 .set("request_seq", request.integerAt("seq").value())
                                                 .set("success", true)
                                                 .set("body", rpc::Json::object())
                                                 .dump()));
    }
    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Ended; }));
    EXPECT_TRUE(adapter.argumentsOf("launch").empty());
    const auto outcome = session->outcome();
    ASSERT_TRUE(outcome);
    ASSERT_TRUE(outcome->error);
    EXPECT_EQ(outcome->error->code(), ErrorCode::Untrusted);
    EXPECT_EQ(outcome->error->message().find("PRIVATE_BOOTSTRAP_SENTINEL"), std::string::npos);
    EXPECT_FALSE(session->restart());
}

TEST(DebugCancelledAttach, LateStartupFailureAfterExplicitStopDoesNotFabricateFatalOrDetachConfirmation) {
    FakeDebugAdapter adapter;
    scriptProgram(adapter, FakeProgram{.runsForever = true});
    adapter.on("attach", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.respond = false}; });
    adapter.on("disconnect", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.respond = false}; });
    DebugSessionOptions options;
    options.adapterId = "frost-lua";
    options.targetMode = DebugTargetMode::Attach;
    auto session = DebugSession::start(test::fakeFactory(adapter), rpc::Json::object(), {}, {}, options);
    ASSERT_TRUE(adapter.waitForRequest("attach"));
    session->stop();
    ASSERT_TRUE(adapter.waitForRequest("disconnect"));
    for (const auto& request : adapter.requests()) {
        if (request.stringAt("command") == "attach") {
            adapter.sendRaw(rpc::encodeFrame(rpc::Json::object()
                                                 .set("seq", 1000)
                                                 .set("type", "response")
                                                 .set("command", "attach")
                                                 .set("request_seq", request.integerAt("seq").value())
                                                 .set("success", false)
                                                 .set("message", "connection lease revoked during native pause")
                                                 .dump()));
        }
    }
    adapter.crash(0); // Real EOF at the framing boundary, without any disconnect response.
    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Ended; }));
    const auto outcome = session->outcome();
    ASSERT_TRUE(outcome);
    EXPECT_TRUE(outcome->stopped);
    EXPECT_FALSE(outcome->error);
    EXPECT_FALSE(outcome->disconnectConfirmed);
    EXPECT_EQ(adapter.argumentsOf("disconnect").front().boolAt("terminateDebuggee"), false);
}

TEST(DebugScriptPause, PreservesActualHookIdentityAndNeverDerivesAnOsThreadFromLuaThreadId) {
    FakeDebugAdapter adapter;
    scriptProgram(adapter, FakeProgram{.runsForever = true});
    DebugSessionOptions options;
    options.adapterId = "frost-lua";
    auto session = DebugSession::start(test::fakeFactory(adapter), rpc::Json::object(), {}, {}, options);
    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Running; }));
    adapter.sendEvent("stopped", rpc::Json::object()
                                     .set("reason", "breakpoint")
                                     .set("threadId", 7)
                                     .set("vmGeneration", std::uint64_t{11})
                                     .set("osThreadId", std::uint64_t{9132})
                                     .set("stopGeneration", std::uint64_t{22}));
    ASSERT_TRUE(test::waitUntil([&] { return session->stopInfo() && session->stopInfo()->scriptPause; }));
    const auto pause = *session->stopInfo()->scriptPause;
    EXPECT_EQ(pause.vmGeneration, 11U);
    EXPECT_EQ(pause.osThreadId, 9132U);
    EXPECT_EQ(pause.stopGeneration, 22U);
    EXPECT_EQ(session->stopInfo()->threadId, 7);
    adapter.sendEvent("continued", rpc::Json::object());
    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Running; }));
    EXPECT_FALSE(session->stopInfo());
    adapter.sendEvent("stopped", rpc::Json::object()
                                     .set("reason", "pause")
                                     .set("threadId", 7)
                                     .set("vmGeneration", std::uint64_t{11})
                                     .set("stopGeneration", std::uint64_t{23}));
    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Stopped; }));
    ASSERT_TRUE(session->stopInfo());
    EXPECT_FALSE(session->stopInfo()->scriptPause);
    EXPECT_FALSE(dap::parseStoppedEvent(rpc::Json::object()
                                            .set("vmGeneration", -1)
                                            .set("osThreadId", std::uint64_t{9132})
                                            .set("stopGeneration", std::uint64_t{24}))
                     .scriptPause);
}

TEST(DebugConfigurationBarrier, AnEarlyStoppedEventDoesNotClaimConfigurationDoneWasAcknowledged) {
    FakeDebugAdapter adapter;
    scriptProgram(adapter, FakeProgram{.runsForever = true});
    adapter.on("configurationDone", [](const rpc::Json&) { return FakeDebugAdapter::Reply{.respond = false}; });
    std::atomic<bool> configured{false};
    DebugSessionCallbacks callbacks;
    callbacks.onConfigured = [&] { configured.store(true); };
    auto session = DebugSession::start(test::fakeFactory(adapter), rpc::Json::object(), {}, std::move(callbacks),
                                       DebugSessionOptions{});
    ASSERT_TRUE(adapter.waitForRequest("configurationDone"));
    adapter.sendEvent("stopped", rpc::Json::object().set("reason", "entry").set("threadId", 1));
    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Stopped; }));
    EXPECT_FALSE(configured.load());
    for (const auto& request : adapter.requests()) {
        if (request.stringAt("command") == "configurationDone") {
            adapter.sendRaw(rpc::encodeFrame(rpc::Json::object()
                                                 .set("seq", 1000)
                                                 .set("type", "response")
                                                 .set("command", "configurationDone")
                                                 .set("request_seq", request.integerAt("seq").value())
                                                 .set("success", true)
                                                 .set("body", rpc::Json::object())
                                                 .dump()));
            break;
        }
    }
    ASSERT_TRUE(test::waitUntil([&] { return configured.load(); }));
    EXPECT_EQ(session->state(), DebugState::Stopped);
}

TEST(DebugReadOnlyDump, ExecutionMutationAndTerminationSendNoRequestsAndStopDetaches) {
    FakeDebugAdapter adapter;
    scriptProgram(adapter, FakeProgram{.runsForever = true});
    DebugSessionOptions options;
    options.readOnly = true;
    auto session = DebugSession::start(test::fakeFactory(adapter), rpc::Json::object(), {}, {}, options);
    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Running; }));
    adapter.sendEvent("stopped", rpc::Json::object().set("reason", "entry").set("threadId", 1));
    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Stopped; }));
    EXPECT_FALSE(session->continueRun());
    EXPECT_FALSE(session->next());
    EXPECT_FALSE(session->pause());
    EXPECT_FALSE(session->runToLine(test::workPath("source.cpp"), 7));
    EXPECT_FALSE(session->restart());
    EXPECT_FALSE(session->terminateDebuggee(1));
    std::promise<bool> variable;
    auto result = variable.get_future();
    session->setVariable(1, "value", "42", [&](auto answer) { variable.set_value(answer.has_value()); });
    ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_FALSE(result.get());
    for (const auto command : {"continue", "next", "pause", "restart", "terminate", "setVariable", "goto"})
        EXPECT_TRUE(adapter.argumentsOf(command).empty()) << command;
    session->stop();
    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Ended; }));
    ASSERT_FALSE(adapter.argumentsOf("disconnect").empty());
    EXPECT_EQ(adapter.argumentsOf("disconnect").front().boolAt("terminateDebuggee"), false);
}

TEST(DebugReadOnlyDump, FailedDumpLaunchNeverRequestsDebuggeeTermination) {
    FakeDebugAdapter adapter;
    scriptProgram(adapter, FakeProgram{.runsForever = true});
    adapter.on("launch",
               [](const rpc::Json&) { return FakeDebugAdapter::Reply{.success = false, .message = "bad dump"}; });
    DebugSessionOptions options;
    options.readOnly = true;
    auto session = DebugSession::start(test::fakeFactory(adapter), rpc::Json::object(), {}, {}, options);
    ASSERT_TRUE(test::waitUntil([&] { return session->state() == DebugState::Ended; }));
    ASSERT_FALSE(adapter.argumentsOf("disconnect").empty());
    EXPECT_EQ(adapter.argumentsOf("disconnect").front().boolAt("terminateDebuggee"), false);
    ASSERT_TRUE(session->outcome());
    EXPECT_TRUE(session->outcome()->error);
}
} // namespace
} // namespace frost::debug
