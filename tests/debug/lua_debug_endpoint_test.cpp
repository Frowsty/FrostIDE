// SPDX-License-Identifier: Apache-2.0
// Contract-v1 standard DAP over a real loopback socket; actual engine VM oracles live in corpus tests.
#include "frost/debug/lua_debug_adapter.hpp"
#include "frost/debug/lua_debug_endpoint.hpp"

#include "debug_test_util.hpp"
#include "lua_debug_peer.hpp"
#include <gtest/gtest.h>

#include <future>

namespace {
using namespace frost;
using namespace frost::debug;
using namespace frost::debug::test;
using namespace std::chrono_literals;
auto deadline() { return std::chrono::steady_clock::now() + 3s; }
using ReplyPromise = std::promise<Result<rpc::Json>>;

TEST(LuaDebugEndpoint, FrozenRoleSeparatedHmacVectorsMatchEngineContract) {
    // Public immutable vector from Frost-Tests/Data/Automation/ScriptDebugProtocol-v1.json.
    ASSERT_GE(sodium_init(), 0);
    const std::string client(64, 'e');
    EXPECT_EQ(luaProof(kLuaCredential, luaTranscript("server", client)),
              "eb4aa1f08dee5ce7457e4449b9742c88d6bf81eca40327e2aba937e0575fb239");
    EXPECT_EQ(luaProof(kLuaCredential, luaTranscript("client", client)),
              "689f8c8f67cf58c1891b7ba29b6cd261cb0c81c5df26dcae86b1002e11a9b4c9");
}

TEST(LuaDebugEndpoint, AuthenticatesAndLeavesActualNextDapFrameUnread) {
    LuaDapPeer peer([](LuaDapPeer& peer) {
        if (!peer.authenticate())
            return;
        peer.event("output", rpc::Json::object().set("category", "console").set("output", "peer-authenticated-output"));
        (void)peer.receive();
    });
    ASSERT_NE(peer.port, 0);
    auto connection = connectLuaDebugEndpoint(luaEndpoint(peer.port), admitLua(), deadline());
    ASSERT_TRUE(connection) << connection.error().describe();
    std::promise<std::string> output;
    auto observed = output.get_future();
    DapHandlers handlers;
    handlers.onEvent = [&output](std::string_view event, const rpc::Json& body) {
        if (event == "output")
            output.set_value(std::string(body.stringAt("output")));
    };
    auto client = luaDebugAdapterFactory(std::move(*connection))(std::move(handlers));
    ASSERT_TRUE(client);
    ASSERT_EQ(observed.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(observed.get(), "peer-authenticated-output");
    (*client)->close();
}

TEST(LuaDebugEndpoint, StandardInitializeUsesSequenceThreeAndOnlyActualCapabilities) {
    std::atomic<bool> correct{false};
    LuaDapPeer peer([&](LuaDapPeer& peer) {
        if (!peer.authenticate())
            return;
        auto initialize = peer.receive();
        if (!initialize)
            return;
        correct = initialize->integerAt("seq") == 3 && initialize->stringAt("command") == "initialize";
        peer.respond(*initialize, rpc::Json::object()
                                      .set("supportsConditionalBreakpoints", false)
                                      .set("supportsStepBack", false)
                                      .set("peerMarker", "actual-capabilities"));
        (void)peer.receive();
    });
    auto connection = connectLuaDebugEndpoint(luaEndpoint(peer.port), admitLua(), deadline());
    ASSERT_TRUE(connection);
    auto client = luaDebugAdapterFactory(std::move(*connection))({});
    ASSERT_TRUE(client);
    auto promise = std::make_shared<ReplyPromise>();
    auto reply = promise->get_future();
    (*client)->request("initialize", rpc::Json::object(),
                       [promise](auto result) { promise->set_value(std::move(result)); });
    ASSERT_EQ(reply.wait_for(3s), std::future_status::ready);
    auto result = reply.get();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->stringAt("peerMarker"), "actual-capabilities");
    EXPECT_EQ(result->boolAt("supportsConditionalBreakpoints"), false);
    EXPECT_EQ(result->boolAt("supportsStepBack"), false);
    EXPECT_TRUE(correct.load());
    (*client)->close();
}

TEST(LuaDebugEndpoint, MapsReorderedStandardRepliesToTheirDapRequests) {
    LuaDapPeer peer([](LuaDapPeer& peer) {
        if (!peer.authenticate())
            return;
        auto first = peer.receive(), second = peer.receive();
        if (!first || !second)
            return;
        peer.respond(*second, rpc::Json::object().set("value", 22));
        peer.respond(*first, rpc::Json::object().set("value", 11));
        (void)peer.receive();
    });
    auto connection = connectLuaDebugEndpoint(luaEndpoint(peer.port), admitLua(), deadline());
    ASSERT_TRUE(connection);
    auto client = luaDebugAdapterFactory(std::move(*connection))({});
    ASSERT_TRUE(client);
    auto first = std::make_shared<ReplyPromise>(), second = std::make_shared<ReplyPromise>();
    auto firstReply = first->get_future(), secondReply = second->get_future();
    (*client)->request("threads", rpc::Json::object(), [first](auto result) { first->set_value(std::move(result)); });
    (*client)->request("stackTrace", rpc::Json::object(),
                       [second](auto result) { second->set_value(std::move(result)); });
    ASSERT_EQ(firstReply.wait_for(3s), std::future_status::ready);
    ASSERT_EQ(secondReply.wait_for(3s), std::future_status::ready);
    auto a = firstReply.get(), b = secondReply.get();
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(a->integerAt("value"), 11);
    EXPECT_EQ(b->integerAt("value"), 22);
    (*client)->close();
}

TEST(LuaDebugEndpoint, MismatchedChallengeIdentityOrProofSendsNoClientProof) {
    const std::vector<std::pair<std::string, rpc::Json>> cases{{"projectPath", "/different/project"},
                                                               {"instanceId", std::string(32, '1')},
                                                               {"generation", std::string(32, '2')},
                                                               {"processId", 999},
                                                               {"debugSessionId", std::string(32, '3')},
                                                               {"serverProof", std::string(64, '0')},
                                                               {"serverNonce", "short"},
                                                               {"algorithm", "none"},
                                                               {"protocolVersion", 2},
                                                               {"clientNonce", std::string(64, '4')}};
    for (const auto& [field, value] : cases) {
        SCOPED_TRACE(field);
        LuaDapPeer peer(
            [&](LuaDapPeer& peer) { peer.authenticate([&](rpc::Json& challenge) { challenge.set(field, value); }); });
        auto result = connectLuaDebugEndpoint(luaEndpoint(peer.port), admitLua(), deadline());
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
        ASSERT_TRUE(waitUntil([&] { return peer.ended.load(); }, 3s));
        EXPECT_EQ(peer.proofAttempts.load(), 0);
        EXPECT_EQ(peer.received.load(), 1);
    }
}

TEST(LuaDebugEndpoint, AuthenticationAcknowledgmentCannotChangeTheProcessIdentity) {
    LuaDapPeer peer([](LuaDapPeer& peer) {
        peer.authenticate({}, [](rpc::Json& ack) { ack.set("processId", 999); });
        (void)peer.receive();
    });
    auto result = connectLuaDebugEndpoint(luaEndpoint(peer.port), admitLua(), deadline());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
    ASSERT_TRUE(waitUntil([&] { return peer.ended.load(); }, 3s));
    EXPECT_EQ(peer.proofAttempts.load(), 1);
    EXPECT_EQ(peer.received.load(), 2); // No initialize or any other standard command.
}

TEST(LuaDebugEndpoint, MissingAdmissionExpiredLeaseAndPreCancelledSetupSendNothing) {
    LuaDapPeer peer([](LuaDapPeer&) {});
    auto noAdmission = connectLuaDebugEndpoint(luaEndpoint(peer.port), {}, deadline());
    ASSERT_FALSE(noAdmission);
    EXPECT_EQ(noAdmission.error().code(), ErrorCode::InvalidArgument);
    auto expired = luaEndpoint(peer.port);
    expired.expiresAt = std::chrono::steady_clock::now() - 1ms;
    auto timeout = connectLuaDebugEndpoint(std::move(expired), admitLua(), deadline());
    ASSERT_FALSE(timeout);
    EXPECT_EQ(timeout.error().code(), ErrorCode::Timeout);
    std::stop_source stop;
    stop.request_stop();
    auto cancelled = connectLuaDebugEndpoint(luaEndpoint(peer.port), admitLua(), deadline(), stop.get_token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code(), ErrorCode::Cancelled);
    EXPECT_EQ(peer.accepted.load(), 0);
}

TEST(LuaDebugEndpoint, CancellationInterruptsAuthenticationWithoutSendingAProof) {
    std::atomic<bool> challenged{false};
    LuaDapPeer peer([&](LuaDapPeer& peer) {
        if (peer.receive())
            challenged = true;
        (void)peer.receive(); // No reply: cancellation must wake the client's blocked auth read.
    });
    std::stop_source stop;
    auto result = std::async(std::launch::async, [&] {
        return connectLuaDebugEndpoint(luaEndpoint(peer.port), admitLua(), deadline(), stop.get_token());
    });
    ASSERT_TRUE(waitUntil([&] { return challenged.load(); }, 1s));
    stop.request_stop();
    ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(result.get());
    EXPECT_EQ(peer.proofAttempts.load(), 0);
}

TEST(LuaDebugEndpoint, AbsoluteAuthenticationDeadlineIncludesPartialHeaderDribble) {
    LuaDapPeer peer([](LuaDapPeer& peer) {
        if (!peer.receive())
            return;
        peer.sendBytes("Content-Length: ");
        (void)peer.receive();
    });
    const auto started = std::chrono::steady_clock::now();
    auto result = connectLuaDebugEndpoint(luaEndpoint(peer.port), admitLua(), started + 150ms);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    EXPECT_EQ(peer.proofAttempts.load(), 0);
}

TEST(LuaDebugEndpoint, RevocationClosesBlockedAuthenticatedTransport) {
    LuaDapPeer peer([](LuaDapPeer& peer) {
        if (peer.authenticate())
            (void)peer.receive();
    });
    std::stop_source stop;
    auto connection = connectLuaDebugEndpoint(luaEndpoint(peer.port), admitLua(), deadline(), stop.get_token());
    ASSERT_TRUE(connection);
    auto transport = std::move(*connection).takeTransport();
    auto blocked = std::async(std::launch::async, [&] {
        std::array<char, 256> bytes{};
        return transport->read(bytes);
    });
    stop.request_stop();
    ASSERT_EQ(blocked.wait_for(1s), std::future_status::ready);
    auto closed = blocked.get();
    EXPECT_TRUE(!closed || *closed == 0);
}

TEST(LuaDebugEndpoint, FreshAdmissionRefusalSendsZeroTargetFrames) {
    std::atomic<bool> admitted{true};
    LuaDapPeer peer([](LuaDapPeer& peer) {
        if (peer.authenticate())
            (void)peer.receive();
    });
    auto connection = connectLuaDebugEndpoint(
        luaEndpoint(peer.port),
        [&](const LuaDebugIdentity&, std::string_view) -> Result<void> {
            if (!admitted)
                return fail(ErrorCode::Untrusted, "revoked");
            return {};
        },
        deadline());
    ASSERT_TRUE(connection);
    auto transport = std::move(*connection).takeTransport();
    admitted = false;
    const auto request = rpc::Json::object()
                             .set("seq", 3)
                             .set("type", "request")
                             .set("command", "evaluate")
                             .set("arguments", rpc::Json::object().set("expression", "danger()"));
    auto result = transport->write(rpc::encodeFrame(request.dump()));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Untrusted);
    ASSERT_TRUE(waitUntil([&] { return peer.ended.load(); }, 3s));
    EXPECT_EQ(peer.received.load(), 2);
}

TEST(LuaDebugEndpoint, DiscardingUnusedAuthenticatedFactoryClosesThePeer) {
    LuaDapPeer peer([](LuaDapPeer& peer) {
        if (peer.authenticate())
            (void)peer.receive();
    });
    auto connection = connectLuaDebugEndpoint(luaEndpoint(peer.port), admitLua(), deadline());
    ASSERT_TRUE(connection);
    {
        auto unused = luaDebugAdapterFactory(std::move(*connection));
    }
    ASSERT_TRUE(waitUntil([&] { return peer.ended.load(); }, 3s));
    EXPECT_EQ(peer.received.load(), 2);
}

TEST(LuaDebugEndpoint, MalformedOrOversizedStandardFrameDisconnectsRetainedClient) {
    for (const auto& malformed : {"Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}", "Content-Length: 1048577\r\n\r\n",
                                  "Content-Length: 2\r\n\r\n[]"}) {
        SCOPED_TRACE(malformed);
        LuaDapPeer peer([&](LuaDapPeer& peer) {
            if (!peer.authenticate())
                return;
            peer.sendBytes(malformed);
            (void)peer.receive();
        });
        auto connection = connectLuaDebugEndpoint(luaEndpoint(peer.port), admitLua(), deadline());
        ASSERT_TRUE(connection);
        std::atomic<bool> closed{false};
        DapHandlers handlers;
        handlers.onClosed = [&](const auto&) { closed = true; };
        auto client = luaDebugAdapterFactory(std::move(*connection))(std::move(handlers));
        ASSERT_TRUE(client);
        ASSERT_TRUE(waitUntil([&] { return closed.load(); }, 3s));
        EXPECT_EQ(peer.received.load(), 2);
        (*client)->close();
    }
}

TEST(LuaDebugEndpoint, DisconnectObserverRequiresActualMatchingAcknowledgment) {
    for (const bool accept : {true, false}) {
        SCOPED_TRACE(accept);
        LuaDapPeer peer([&](LuaDapPeer& peer) {
            if (!peer.authenticate())
                return;
            auto request = peer.receive();
            if (request && request->stringAt("command") == "disconnect")
                peer.respond(*request, rpc::Json::object(), accept);
            (void)peer.receive();
        });
        auto observed = std::make_shared<std::promise<Result<void>>>();
        auto detached = observed->get_future();
        LuaDebugTransportCallbacks callbacks;
        callbacks.onDetached = [observed](auto result) { observed->set_value(std::move(result)); };
        auto connection =
            connectLuaDebugEndpoint(luaEndpoint(peer.port), admitLua(), deadline(), {}, std::move(callbacks));
        ASSERT_TRUE(connection);
        auto client = luaDebugAdapterFactory(std::move(*connection))({});
        ASSERT_TRUE(client);
        (*client)->request("disconnect", rpc::Json::object().set("terminateDebuggee", false), [](auto) {});
        ASSERT_EQ(detached.wait_for(3s), std::future_status::ready);
        EXPECT_EQ(detached.get().has_value(), accept);
        (*client)->close();
    }
}

TEST(LuaDebugEndpoint, CredentialNeverReachesDapOutputOrProtocolErrors) {
    LuaDapPeer peer([](LuaDapPeer& peer) {
        if (!peer.authenticate())
            return;
        peer.event("output", rpc::Json::object()
                                 .set("output", std::string(kLuaCredential))
                                 .set(std::string(kLuaCredential), "secret-key-name"));
        (void)peer.receive();
    });
    auto connection = connectLuaDebugEndpoint(luaEndpoint(peer.port), admitLua(), deadline());
    ASSERT_TRUE(connection);
    std::promise<std::string> output;
    auto observed = output.get_future();
    DapHandlers handlers;
    handlers.onEvent = [&](std::string_view, const rpc::Json& body) { output.set_value(body.dump()); };
    auto client = luaDebugAdapterFactory(std::move(*connection))(std::move(handlers));
    ASSERT_TRUE(client);
    ASSERT_EQ(observed.wait_for(3s), std::future_status::ready);
    const auto text = observed.get();
    EXPECT_EQ(text.find(kLuaCredential), std::string::npos);
    EXPECT_NE(text.find("[redacted]"), std::string::npos);
    (*client)->close();
}

TEST(LuaDebugEndpoint, NativeBusyAdmissionRefusesLocallyWithoutClosingAuthenticatedPeer) {
    std::atomic<bool> nativePaused{false};
    LuaDapPeer peer([](LuaDapPeer& peer) {
        if (!peer.authenticate())
            return;
        const auto request = peer.receive();
        if (request)
            peer.respond(*request, rpc::Json::object().set("marker", "after-native-continued"));
        (void)peer.receive();
    });
    auto connection = connectLuaDebugEndpoint(
        luaEndpoint(peer.port),
        [&](const LuaDebugIdentity&, std::string_view command) -> Result<void> {
            if (nativePaused && command != "<receive>")
                return fail(ErrorCode::Busy, "native adapter is actually stopped");
            return {};
        },
        deadline());
    ASSERT_TRUE(connection);
    auto client = luaDebugAdapterFactory(std::move(*connection))({});
    ASSERT_TRUE(client);
    nativePaused = true;
    auto refused = std::make_shared<ReplyPromise>();
    auto refusedReply = refused->get_future();
    (*client)->request("evaluate", rpc::Json::object(),
                       [refused](auto reply) { refused->set_value(std::move(reply)); });
    ASSERT_EQ(refusedReply.wait_for(3s), std::future_status::ready);
    auto refusal = refusedReply.get();
    ASSERT_FALSE(refusal);
    EXPECT_EQ(refusal.error().code(), ErrorCode::Busy);
    EXPECT_FALSE((*client)->closed());
    EXPECT_EQ((*client)->sentMessageCount(), 0U);
    EXPECT_EQ(peer.received.load(), 2); // Both auth messages only; no evaluate bytes.
    nativePaused = false;
    auto admitted = std::make_shared<ReplyPromise>();
    auto admittedReply = admitted->get_future();
    (*client)->request("threads", rpc::Json::object(),
                       [admitted](auto reply) { admitted->set_value(std::move(reply)); });
    ASSERT_EQ(admittedReply.wait_for(3s), std::future_status::ready);
    auto result = admittedReply.get();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->stringAt("marker"), "after-native-continued");
    (*client)->close();
}

TEST(LuaDebugEndpoint, AuthenticatedFactoryCannotBeAdoptedTwice) {
    LuaDapPeer peer([](LuaDapPeer& peer) {
        if (peer.authenticate())
            (void)peer.receive();
    });
    auto connection = connectLuaDebugEndpoint(luaEndpoint(peer.port), admitLua(), deadline());
    ASSERT_TRUE(connection);
    auto factory = luaDebugAdapterFactory(std::move(*connection));
    const auto copy = factory;
    auto first = factory({});
    ASSERT_TRUE(first);
    auto second = copy({});
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code(), ErrorCode::Busy);
    EXPECT_EQ(peer.received.load(), 2);
    (*first)->close();
}

TEST(LuaDebugEndpoint, RetiredExperimentalEntryPointsRefuseWithoutConnecting) {
    LuaDapPeer peer([](LuaDapPeer&) {});
    auto endpoint = connectLuaDebugEndpoint(peer.port, "legacy-token", deadline());
    auto play = connectLuaDebugPlayEndpoint(peer.port, "legacy-token", deadline());
    auto factory = luaDebugAdapterFactory(peer.port, "legacy-token", 100ms, true);
    auto client = factory({});
    ASSERT_FALSE(endpoint);
    ASSERT_FALSE(play);
    ASSERT_FALSE(client);
    EXPECT_EQ(endpoint.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(play.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(client.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(peer.accepted.load(), 0);
}
} // namespace
