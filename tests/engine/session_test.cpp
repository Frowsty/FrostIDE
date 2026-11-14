// SPDX-License-Identifier: Apache-2.0
// Contract-v2 socket tests for session authentication, policy, cancellation and reconnection.
#include "frost/engine/session.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

#include <sodium.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>

#include <unistd.h>
#endif

namespace frost::engine {
namespace {
using namespace std::chrono_literals;
using Json = rpc::Json;
#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
void closeSocket(Socket socket) { closesocket(socket); }
#else
using Socket = int;
constexpr Socket kInvalid = -1;
void closeSocket(Socket socket) { close(socket); }
#endif
std::string testMac(std::string_view secret, std::string_view text) {
    crypto_auth_hmacsha256_state state{};
    std::array<unsigned char, 32> bytes{};
    std::array<char, 65> encoded{};
    crypto_auth_hmacsha256_init(&state, reinterpret_cast<const unsigned char*>(secret.data()), secret.size());
    crypto_auth_hmacsha256_update(&state, reinterpret_cast<const unsigned char*>(text.data()), text.size());
    crypto_auth_hmacsha256_final(&state, bytes.data());
    sodium_bin2hex(encoded.data(), encoded.size(), bytes.data(), bytes.size());
    return encoded.data();
}
// An independent minimal network peer. It validates both role transcripts; it never supplies
// acceptance evidence for the real editor, which belongs to session_corpus_test.cpp.
class SessionPeer {
public:
    explicit SessionPeer(char generation = 'b', bool badProof = false, std::optional<Json> processId = Json(12345),
                         bool badInfoIdentity = false, std::optional<Json> scriptDebug = std::nullopt,
                         Json bootstrapOverrides = Json::object(), bool pendingPlayer = false,
                         std::optional<Json> crashSource = std::nullopt)
        : m_badProof(badProof), m_processId(std::move(processId)), m_badInfoIdentity(badInfoIdentity),
          m_scriptDebug(std::move(scriptDebug)), m_bootstrapOverrides(std::move(bootstrapOverrides)),
          m_pendingPlayer(pendingPlayer), m_crashSource(std::move(crashSource)) {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return;
        m_initialized = true;
#endif
        static_cast<void>(sodium_init());
        endpoint.projectPath =
#ifdef _WIN32
            "C:/session-project";
#else
            "/session-project";
#endif
        endpoint.instanceId = std::string(32, 'a');
        endpoint.generation = std::string(32, generation);
        endpoint.secret = "paired-test-token";
        m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_listener == kInvalid)
            return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(m_listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(m_listener, 1) != 0)
            return;
#ifdef _WIN32
        int size = sizeof(address);
#else
        socklen_t size = sizeof(address);
#endif
        if (getsockname(m_listener, reinterpret_cast<sockaddr*>(&address), &size) != 0)
            return;
        endpoint.port = ntohs(address.sin_port);
        m_thread = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    ~SessionPeer() {
        stop();
#ifdef _WIN32
        if (m_initialized)
            WSACleanup();
#endif
    }
    void stop() {
        m_thread.request_stop();
        {
            std::lock_guard lock(m_mutex);
            if (m_peer != kInvalid) {
#ifdef _WIN32
                shutdown(m_peer, SD_BOTH);
#else
                shutdown(m_peer, SHUT_RDWR);
#endif
            }
        }
        if (m_thread.joinable())
            m_thread.join();
        if (m_listener != kInvalid) {
            closeSocket(m_listener);
            m_listener = kInvalid;
        }
    }
    EngineEndpoint endpoint;
    std::atomic<unsigned> commands{0}, clientProofs{0}, bootstraps{0};
    std::atomic<bool> heartbeats{true};

private:
    bool ready(Socket socket, long milliseconds) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(socket, &set);
        timeval wait{0, milliseconds * 1000};
        return select(static_cast<int>(socket + 1), &set, nullptr, nullptr, &wait) > 0;
    }
    Json identity() const {
        return Json::object()
            .set("instanceId", endpoint.instanceId)
            .set("generation", endpoint.generation)
            .set("projectPath", endpoint.projectPath);
    }
    std::string transcript(std::string_view role, std::string_view client) const {
        return "FrostEngine.Session.v2\nrole=" + std::string(role) + "\ninstanceId=" + endpoint.instanceId +
               "\ngeneration=" + endpoint.generation + "\nprojectPath=" + endpoint.projectPath +
               "\nclientNonce=" + std::string(client) + "\nserverNonce=" + std::string(64, 'c') +
               "\ncapabilities=session.v2,auth.hmac-sha256,events.v1,heartbeat.v1\nheartbeatIntervalMs="
               "50\nheartbeatTimeoutMs=250\n";
    }
    bool sendJson(Socket peer, Json value) {
        const auto bytes = value.dump() + "\n";
        std::size_t offset = 0;
        while (offset < bytes.size()) {
#ifdef _WIN32
            const auto sent = send(peer, bytes.data() + offset, static_cast<int>(bytes.size() - offset), 0);
#else
            const auto sent = send(peer, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
#endif
            if (sent <= 0)
                return false;
            offset += static_cast<std::size_t>(sent);
        }
        return true;
    }
    void run(std::stop_token stop) {
        while (!stop.stop_requested() && !ready(m_listener, 20)) {
        }
        if (stop.stop_requested())
            return;
        const Socket peer = accept(m_listener, nullptr, nullptr);
        if (peer == kInvalid)
            return;
        {
            std::lock_guard lock(m_mutex);
            m_peer = peer;
        }
        bool authenticated = false, subscribed = false;
        std::uint64_t sequence = 0;
        std::string buffered, client;
        auto heartbeatAt = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            if (subscribed && heartbeats && std::chrono::steady_clock::now() >= heartbeatAt) {
                if (!sendJson(peer, Json::object()
                                        .set("jsonrpc", "2.0")
                                        .set("method", "engine.heartbeat")
                                        .set("params", identity().set("contractVersion", 2))))
                    break;
                heartbeatAt = std::chrono::steady_clock::now() + 50ms;
            }
            if (!ready(peer, 10))
                continue;
            std::array<char, 4096> bytes{};
            const auto read = recv(peer, bytes.data(), static_cast<int>(bytes.size()), 0);
            if (read <= 0)
                break;
            buffered.append(bytes.data(), static_cast<std::size_t>(read));
            for (;;) {
                const auto newline = buffered.find('\n');
                if (newline == std::string::npos)
                    break;
                auto request = rpc::parseJson(std::string_view(buffered).substr(0, newline));
                buffered.erase(0, newline + 1);
                if (!request)
                    break;
                const auto id = request->unsignedAt("id").value_or(0);
                const auto method = request->stringAt("method");
                const auto* params = request->find("params");
                Json result;
                if (method == "auth.challenge") {
                    client = params ? std::string(params->stringAt("clientNonce")) : "";
                    result =
                        identity()
                            .set("contractVersion", 2)
                            .set("algorithm", "hmac-sha256")
                            .set("clientNonce", client)
                            .set("serverNonce", std::string(64, 'c'))
                            .set("capabilities",
                                 Json::array({"session.v2", "auth.hmac-sha256", "events.v1", "heartbeat.v1"}))
                            .set("heartbeatIntervalMs", 50)
                            .set("heartbeatTimeoutMs", 250)
                            .set("serverProof", m_badProof ? std::string(64, '0')
                                                           : testMac(endpoint.secret, transcript("server", client)));
                } else if (method == "auth.prove") {
                    ++clientProofs;
                    authenticated =
                        params && params->stringAt("proof") == testMac(endpoint.secret, transcript("client", client));
                    result = identity().set("authenticated", authenticated).set("contractVersion", 2);
                } else {
                    const auto* session = request->find("session");
                    if (!authenticated || !session || *session != identity())
                        break;
                    if (method == "server.info") {
                        result = identity()
                                     .set("role", m_pendingPlayer ? "player" : "editor")
                                     .set("contractVersion", 2)
                                     .set("sessionProtocolVersion", 2)
                                     .set("authenticated", true);
                        if (m_processId)
                            result.set("processId", *m_processId);
                        if (m_scriptDebug)
                            result.set("scriptDebug", *m_scriptDebug);
                        if (m_crashSource)
                            result.set("crashArtifactSource", *m_crashSource);
                        if (m_badInfoIdentity)
                            result.set("generation", std::string(32, 'f'));
                    } else if (method == "script.debug.bootstrap") {
                        ++bootstraps;
                        result = identity()
                                     .set("protocolVersion", 1)
                                     .set("host", "127.0.0.1")
                                     .set("port", 45678)
                                     .set("processId", 12345)
                                     .set("debugSessionId", std::string(32, 'c'))
                                     .set("credential", std::string(64, 'd'))
                                     .set("expiresMs", 30000);
                        for (std::size_t i = 0; i < m_bootstrapOverrides.size(); ++i)
                            result.set(m_bootstrapOverrides.keys()[i], m_bootstrapOverrides.items()[i]);
                    } else if (method == "schema.export") {
                        result =
                            Json::object().set("commands", Json::array({Json::object().set("name", "echo"),
                                                                        Json::object().set("name", "play.pause"),
                                                                        Json::object().set("name", "play.resume")}));
                    } else if (method == "server.subscribe") {
                        result = identity()
                                     .set("sequence", sequence)
                                     .set("snapshots",
                                          Json::object().set("play.state",
                                                             Json::object()
                                                                 .set("state", m_pendingPlayer ? "Unavailable" : "Edit")
                                                                 .set("mode", m_pendingPlayer ? "Unavailable" : "Edit")
                                                                 .set("paused", false)
                                                                 .set("frames", 0)));
                        subscribed = true;
                    } else {
                        ++commands;
                        if (method == "test.hold" || method == "test.run")
                            continue;
                        if (method == "test.logs") {
                            auto data =
                                Json::object()
                                    .set("sequence", 77)
                                    .set("level", "warn")
                                    .set("logger", "Lua")
                                    .set("message", "actual log " + endpoint.secret)
                                    .set("timestamp", 123456789)
                                    .set("source", Json::object().set("path", "Scripts/Actual.lua").set("line", 17));
                            if (!sendJson(peer, Json::object()
                                                    .set("jsonrpc", "2.0")
                                                    .set("method", "engine.event")
                                                    .set("params", identity()
                                                                       .set("event", "log.entry")
                                                                       .set("sequence", ++sequence)
                                                                       .set("data", data))))
                                break;
                            data.set("path", "Scripts/Actual.lua").set("line", 17);
                            if (!sendJson(peer, Json::object()
                                                    .set("jsonrpc", "2.0")
                                                    .set("method", "engine.event")
                                                    .set("params", identity()
                                                                       .set("event", "diagnostic")
                                                                       .set("sequence", ++sequence)
                                                                       .set("data", std::move(data)))))
                                break;
                        }
                        if (method == "test.project-change" || method == "test.shutdown") {
                            (void)sendJson(peer, Json::object()
                                                     .set("jsonrpc", "2.0")
                                                     .set("method", "engine.event")
                                                     .set("params", identity()
                                                                        .set("event", method == "test.shutdown"
                                                                                          ? "server.shutdown"
                                                                                          : "project.changed")
                                                                        .set("sequence", ++sequence)
                                                                        .set("data", Json::object())));
#ifdef _WIN32
                            shutdown(peer, SD_BOTH);
#else
                            shutdown(peer, SHUT_RDWR);
#endif
                            continue;
                        }
                        if (method == "test.operation-state") {
                            (void)sendJson(peer,
                                           Json::object()
                                               .set("jsonrpc", "2.0")
                                               .set("method", "engine.event")
                                               .set("params", identity()
                                                                  .set("event", "play.state")
                                                                  .set("sequence", ++sequence)
                                                                  .set("data", params ? *params : Json::object())));
                        }
                        if (method == "test.error") {
                            if (!sendJson(peer,
                                          Json::object()
                                              .set("jsonrpc", "2.0")
                                              .set("id", id)
                                              .set("error", Json::object()
                                                                .set("code", -32003)
                                                                .set("message", "busy " + endpoint.secret)
                                                                .set("data", Json::object()
                                                                                 .set("reason", "actual engine detail")
                                                                                 .set("token", endpoint.secret)))))
                                break;
                            continue;
                        }
                        if (method == "test.stale" || method == "test.replay" || method == "test.badstate") {
                            auto envelope = identity()
                                                .set("event", "play.state")
                                                .set("sequence", ++sequence)
                                                .set("data", Json::object()
                                                                 .set("state", "Play")
                                                                 .set("mode", "Play")
                                                                 .set("paused", false)
                                                                 .set("frames", sequence));
                            if (method == "test.stale")
                                envelope.set("generation", std::string(32, 'e'));
                            if (method == "test.replay")
                                envelope.set("sequence", 0);
                            if (method == "test.badstate")
                                envelope.set("data", Json::object().set("state", "Play"));
                            if (!sendJson(peer, Json::object()
                                                    .set("jsonrpc", "2.0")
                                                    .set("method", "engine.event")
                                                    .set("params", std::move(envelope))))
                                break;
                            continue;
                        }
                        result = params ? *params : Json::object();
                        if (method == "play.pause" || method == "play.resume") {
                            const bool paused = method == "play.pause";
                            if (!sendJson(peer, Json::object()
                                                    .set("jsonrpc", "2.0")
                                                    .set("method", "engine.event")
                                                    .set("params",
                                                         identity()
                                                             .set("event", "play.state")
                                                             .set("sequence", ++sequence)
                                                             .set("data", Json::object()
                                                                              .set("state", paused ? "Paused" : "Play")
                                                                              .set("mode", "Play")
                                                                              .set("paused", paused)
                                                                              .set("frames", sequence)))))
                                break;
                        }
                    }
                }
                if (!sendJson(peer,
                              Json::object().set("jsonrpc", "2.0").set("id", id).set("result", std::move(result))))
                    break;
            }
        }
        {
            std::lock_guard lock(m_mutex);
            m_peer = kInvalid;
            closeSocket(peer);
        }
    }
    bool m_badProof = false;
    std::optional<Json> m_processId;
    bool m_badInfoIdentity = false;
    std::optional<Json> m_scriptDebug;
    Json m_bootstrapOverrides;
    bool m_pendingPlayer = false;
    std::optional<Json> m_crashSource;
    Socket m_listener = kInvalid, m_peer = kInvalid;
    std::mutex m_mutex;
    std::jthread m_thread;
#ifdef _WIN32
    bool m_initialized = false;
#endif
};
struct Observations {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned connections = 0, states = 0, disconnects = 0, logs = 0, diagnostics = 0;
    EngineIdentity identity, disconnectIdentity;
    EngineDisconnectCause disconnectCause = EngineDisconnectCause::Unexpected;
    EngineState state;
    EngineLogEntry log;
    EngineDiagnostic diagnostic;
    EngineSessionCallbacks callbacks() {
        EngineSessionCallbacks callbacks;
        callbacks.connected = [this](const EngineIdentity& value) {
            std::lock_guard lock(mutex);
            ++connections;
            identity = value;
            cv.notify_all();
        };
        callbacks.state = [this](const EngineState& value) {
            std::lock_guard lock(mutex);
            ++states;
            state = value;
            cv.notify_all();
        };
        callbacks.log = [this](const EngineLogEntry& value) {
            std::lock_guard lock(mutex);
            ++logs;
            log = value;
            cv.notify_all();
        };
        callbacks.diagnostic = [this](const EngineDiagnostic& value) {
            std::lock_guard lock(mutex);
            ++diagnostics;
            diagnostic = value;
            cv.notify_all();
        };
        callbacks.disconnected = [this](const EngineDisconnect& value) {
            std::lock_guard lock(mutex);
            ++disconnects;
            disconnectIdentity = value.identity;
            disconnectCause = value.cause;
            cv.notify_all();
        };
        return callbacks;
    }
    template<class Predicate>
    bool wait(Predicate predicate) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, 3s, predicate);
    }
};
EngineSessionOptions optionsFor(const SessionPeer& peer) {
    EngineSessionOptions options;
    options.endpoint = peer.endpoint;
    options.trustRevision = 42;
    options.trusted = [](std::string_view, std::uint64_t revision) { return revision == 42; };
    options.admitCommand = [](std::string_view, const Json&, const EngineIdentity&) -> Result<void> { return {}; };
    options.connectTimeout = 1s;
    return options;
}
Result<Json> invoke(EngineSession& session, std::string method, Json params = Json::object()) {
    auto promise = std::make_shared<std::promise<Result<Json>>>();
    auto future = promise->get_future();
    auto id = session.request(std::move(method), std::move(params),
                              [promise](Result<Json> reply) { promise->set_value(std::move(reply)); });
    if (!id)
        return std::unexpected(id.error());
    if (future.wait_for(3s) != std::future_status::ready)
        return fail(ErrorCode::Timeout, "test reply timeout");
    return future.get();
}
TEST(EngineSession, RejectsUntrustedAndMalformedSelectionSynchronously) {
    EngineSessionOptions options;
    EXPECT_EQ(EngineSession::create(options).error().code(), ErrorCode::InvalidArgument);
    SessionPeer peer;
    options = optionsFor(peer);
    options.trusted = [](std::string_view, std::uint64_t) { return false; };
    EXPECT_EQ(EngineSession::create(options).error().code(), ErrorCode::Untrusted);
}
TEST(EngineSession, RejectsInvalidServerProofWithoutSendingClientProofOrCommand) {
    SessionPeer peer('b', true);
    ASSERT_NE(peer.endpoint.port, 0);
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
    EXPECT_EQ(observed.connections, 0U);
    EXPECT_EQ(peer.clientProofs.load(), 0U);
    EXPECT_EQ(peer.commands.load(), 0U);
    (*session)->shutdown();
}
TEST(EngineSession, OmittedAuthenticatedProcessIdRemainsUnknown) {
    SessionPeer peer('b', false, std::nullopt);
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    EXPECT_EQ(observed.identity.processId, 0);
    EXPECT_EQ(observed.state.identity.processId, 0);
    EXPECT_TRUE((*session)->connected());
    (*session)->shutdown();
}
TEST(EngineSession, InvalidAuthenticatedProcessIdIsRefusedBeforeConnected) {
    for (const auto& invalid : {Json(0), Json(-1), Json("12345"), Json(1.5), Json(true), Json(),
                                Json(std::numeric_limits<std::uint64_t>::max())}) {
        SessionPeer peer('b', false, invalid);
        Observations observed;
        auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
        ASSERT_TRUE(session);
        ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
        EXPECT_EQ(observed.connections, 0U);
        EXPECT_EQ(observed.states, 0U);
        EXPECT_EQ(peer.commands.load(), 0U);
        (*session)->shutdown();
    }
}
TEST(EngineSession, ProcessIdFromMismatchedServerInfoIdentityIsNeverPublished) {
    SessionPeer peer('b', false, Json(12345), true);
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
    EXPECT_EQ(observed.connections, 0U);
    EXPECT_EQ(observed.states, 0U);
    EXPECT_EQ(peer.commands.load(), 0U);
    (*session)->shutdown();
}
TEST(EngineSession, PersistentTwentyPauseResumeCyclesAndSecretRedaction) {
    SessionPeer peer;
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    EXPECT_EQ(observed.identity.heartbeatTimeoutMs, 250U);
    EXPECT_EQ(observed.identity.processId, 12345);
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
        ASSERT_TRUE(invoke(**session, "play.pause"));
        ASSERT_TRUE(invoke(**session, "play.resume"));
    }
    ASSERT_TRUE(observed.wait([&] { return observed.states == 41; }));
    EXPECT_EQ(observed.state.state, "Play");
    EXPECT_EQ(observed.state.sequence, 40U);
    EXPECT_EQ(observed.state.identity.processId, 12345);
    EXPECT_EQ((*session)->generation(), 1U);
    auto echoed = invoke(**session, "echo",
                         Json::object().set("message", "prefix " + peer.endpoint.secret).set("secret", "hidden"));
    ASSERT_TRUE(echoed);
    EXPECT_EQ(echoed->stringAt("message"), "prefix [redacted]");
    EXPECT_EQ(echoed->stringAt("secret"), "[redacted]");
    EXPECT_EQ((*session)->sentCommandCount(), 41U);
    (*session)->shutdown();
}
TEST(EngineSession, PolicyRefusalNeverWritesAndQueuedCancellationCompletesOnce) {
    SessionPeer peer;
    Observations observed;
    auto options = optionsFor(peer);
    options.admitCommand = [](std::string_view, const Json&, const EngineIdentity&) -> Result<void> {
        return fail(ErrorCode::PermissionDenied, "blocked");
    };
    auto session = EngineSession::create(std::move(options), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    auto result = invoke(**session, "echo");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(peer.commands.load(), 0U);
    EXPECT_EQ((*session)->sentCommandCount(), 0U);
    std::stop_source stop;
    stop.request_stop();
    std::promise<Result<Json>> cancelled;
    auto future = cancelled.get_future();
    auto request = (*session)->request(
        "echo", Json::object(), [&](Result<Json> reply) { cancelled.set_value(std::move(reply)); }, stop.get_token());
    ASSERT_TRUE(request);
    ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(future.get().error().code(), ErrorCode::Cancelled);
    EXPECT_EQ(peer.commands.load(), 0U);
    (*session)->shutdown();
}
TEST(EngineSession, HeartbeatSurvivesHeldMainThreadRequestAndCancellation) {
    SessionPeer peer;
    Observations observed;
    auto options = optionsFor(peer);
    options.requestTimeout = 100ms;
    auto session = EngineSession::create(std::move(options), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    std::promise<Result<Json>> completed;
    auto future = completed.get_future();
    auto request = (*session)->request("test.run", Json::object(),
                                       [&](Result<Json> reply) { completed.set_value(std::move(reply)); });
    ASSERT_TRUE(request);
    EXPECT_EQ(future.wait_for(700ms), std::future_status::timeout);
    EXPECT_TRUE((*session)->connected());
    EXPECT_TRUE(invoke(**session, "echo"));
    (*session)->cancel(*request);
    ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(future.get().error().code(), ErrorCode::Cancelled);
    EXPECT_TRUE((*session)->connected());
    (*session)->shutdown();
}
TEST(EngineSession, ReconnectUsesFreshSelectedGenerationAndDoesNotReplayPendingMutation) {
    SessionPeer first('b'), second('d');
    Observations observed;
    auto options = optionsFor(first);
    options.rediscover = [&]() -> Result<EngineEndpoint> { return second.endpoint; };
    auto session = EngineSession::create(std::move(options), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    std::promise<Result<Json>> completed;
    auto future = completed.get_future();
    ASSERT_TRUE((*session)->request("test.hold", Json::object(),
                                    [&](Result<Json> reply) { completed.set_value(std::move(reply)); }));
    // A completed echo proves the preceding hold reached the same server before disconnect.
    ASSERT_TRUE(invoke(**session, "echo"));
    first.stop();
    ASSERT_TRUE(observed.wait([&] { return observed.connections == 2 && observed.states >= 2; }));
    ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    EXPECT_FALSE(future.get());
    EXPECT_EQ(observed.identity.generation, second.endpoint.generation);
    EXPECT_EQ((*session)->generation(), 2U);
    EXPECT_EQ(second.commands.load(), 0U);
    EXPECT_TRUE(invoke(**session, "echo"));
    (*session)->shutdown();
}
TEST(EngineSession, MissingHeartbeatDisconnects) {
    SessionPeer peer;
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    peer.heartbeats = false;
    ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
    EXPECT_FALSE((*session)->connected());
    (*session)->shutdown();
}
TEST(EngineSession, TypedLogsAndDiagnosticsRetainActualSourceAndSanitizeSecrets) {
    SessionPeer peer;
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    ASSERT_TRUE(invoke(**session, "test.logs"));
    ASSERT_TRUE(observed.wait([&] { return observed.logs == 1 && observed.diagnostics == 1; }));
    EXPECT_EQ(observed.log.logger, "Lua");
    EXPECT_EQ(observed.log.message, "actual log [redacted]");
    EXPECT_EQ(observed.log.file, "Scripts/Actual.lua");
    EXPECT_EQ(observed.log.line, 17U);
    EXPECT_EQ(observed.log.sequence, 77U);
    EXPECT_EQ(observed.log.eventSequence, 1U);
    EXPECT_EQ(observed.log.identity.processId, 12345);
    EXPECT_EQ(observed.log.timestamp, 123456789);
    EXPECT_EQ(observed.diagnostic.severity, "warning");
    EXPECT_EQ(observed.diagnostic.file, "Scripts/Actual.lua");
    EXPECT_EQ(observed.diagnostic.line, 17U);
    EXPECT_EQ(observed.diagnostic.column, 0U);
    EXPECT_EQ(observed.diagnostic.eventSequence, 2U);
    EXPECT_EQ(observed.diagnostic.identity.processId, 12345);
    (*session)->shutdown();
}
TEST(EngineSession, MissingPolicyDeniesAndReservedOrOversizedRequestsAreNotAdmitted) {
    SessionPeer peer;
    Observations observed;
    auto options = optionsFor(peer);
    options.admitCommand = {};
    auto session = EngineSession::create(std::move(options), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    auto denied = invoke(**session, "echo");
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code(), ErrorCode::PermissionDenied);
    for (const auto* method :
         {"auth", "auth.prove", "server.configure", "server.subscribe", "server.shutdown", "engine.event"})
        EXPECT_EQ((*session)->request(method, Json::object(), [](Result<Json>) {}).error().code(),
                  ErrorCode::InvalidArgument);
    EXPECT_EQ((*session)
                  ->request("echo", Json::object().set("bytes", std::string(1024 * 1024, 'x')), [](Result<Json>) {})
                  .error()
                  .code(),
              ErrorCode::InvalidArgument);
    EXPECT_EQ(peer.commands.load(), 0U);
    EXPECT_EQ((*session)->sentCommandCount(), 0U);
    (*session)->shutdown();
}
TEST(EngineSession, PendingBoundAndShutdownCompleteCallbacksOnWorkerExactlyOnce) {
    SessionPeer peer;
    Observations observed;
    std::thread::id callbackThread;
    auto callbacks = observed.callbacks();
    auto connected = callbacks.connected;
    callbacks.connected = [&](const EngineIdentity& value) {
        callbackThread = std::this_thread::get_id();
        connected(value);
    };
    auto session = EngineSession::create(optionsFor(peer), std::move(callbacks));
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    EXPECT_NE(callbackThread, std::this_thread::get_id());
    std::atomic<unsigned> completions{0};
    std::atomic<bool> wrongThread{false};
    for (unsigned i = 0; i < 128; ++i) {
        ASSERT_TRUE((*session)->request("test.hold", Json::object(), [&](Result<Json>) {
            if (std::this_thread::get_id() != callbackThread)
                wrongThread = true;
            ++completions;
        }));
    }
    EXPECT_EQ((*session)->request("test.hold", Json::object(), [](Result<Json>) {}).error().code(), ErrorCode::Busy);
    (*session)->shutdown();
    EXPECT_EQ(completions.load(), 128U);
    EXPECT_FALSE(wrongThread.load());
    (*session)->shutdown();
    EXPECT_EQ(completions.load(), 128U);
}
TEST(EngineSession, RefusalPreservesSanitizedActualEngineCodeMessageAndData) {
    SessionPeer peer;
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    auto result = invoke(**session, "test.error");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Busy);
    EXPECT_NE(result.error().message().find("-32003"), std::string::npos);
    EXPECT_NE(result.error().message().find("busy [redacted]"), std::string::npos);
    EXPECT_NE(result.error().message().find("actual engine detail"), std::string::npos);
    EXPECT_EQ(result.error().message().find(peer.endpoint.secret), std::string::npos);
    (*session)->shutdown();
}
TEST(EngineSession, StaleOrReplayedOrMalformedStateDisconnectsWithoutPublishingState) {
    for (const auto* command : {"test.stale", "test.replay", "test.badstate"}) {
        SessionPeer peer;
        Observations observed;
        auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
        ASSERT_TRUE(session);
        ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
        EXPECT_FALSE(invoke(**session, command));
        ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
        EXPECT_EQ(observed.states, 1U);
        EXPECT_EQ(observed.state.state, "Edit");
        (*session)->shutdown();
    }
}
TEST(EngineSession, TrustRevisionIsRecheckedAfterPolicyBeforeSending) {
    SessionPeer peer;
    Observations observed;
    std::atomic<bool> trusted{true};
    auto options = optionsFor(peer);
    options.trusted = [&](std::string_view project, std::uint64_t revision) {
        return trusted.load() && project == peer.endpoint.projectPath && revision == 42;
    };
    options.rediscover = [&]() -> Result<EngineEndpoint> {
        ADD_FAILURE() << "revoked session must not rediscover";
        return peer.endpoint;
    };
    options.admitCommand = [&](std::string_view, const Json&, const EngineIdentity& identity) -> Result<void> {
        EXPECT_EQ(identity.generation, peer.endpoint.generation);
        EXPECT_FALSE(identity.schema.isNull());
        trusted = false;
        return {};
    };
    auto session = EngineSession::create(std::move(options), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    auto result = invoke(**session, "echo");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Untrusted);
    ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
    EXPECT_EQ(peer.commands.load(), 0U);
    EXPECT_EQ((*session)->sentCommandCount(), 0U);
    (*session)->shutdown();
}
TEST(EngineSession, TimeoutReportsUncertaintyAndDoesNotDropHealthySession) {
    SessionPeer peer;
    Observations observed;
    auto options = optionsFor(peer);
    options.requestTimeout = 100ms;
    auto session = EngineSession::create(std::move(options), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    auto result = invoke(**session, "test.hold");
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout);
    EXPECT_NE(result.error().message().find("uncertain"), std::string::npos);
    EXPECT_TRUE((*session)->connected());
    EXPECT_TRUE(invoke(**session, "echo"));
    (*session)->shutdown();
}

Json scriptDebugCapability(bool available = true) {
    return Json::object().set("protocolVersion", 1).set("available", available);
}
Result<ScriptDebugBootstrap> privateBootstrap(
    EngineSession& session, const EngineIdentity& owner,
    std::function<Result<void>(const EngineIdentity&)> admission = [](const EngineIdentity&) -> Result<void> {
        return {};
    }) {
    auto promise = std::make_shared<std::promise<Result<ScriptDebugBootstrap>>>();
    auto future = promise->get_future();
    auto accepted =
        session.bootstrapScriptDebug(owner, std::move(admission), [promise](Result<ScriptDebugBootstrap> result) {
            promise->set_value(std::move(result));
        });
    if (!accepted)
        return std::unexpected(accepted.error());
    if (future.wait_for(3s) != std::future_status::ready)
        return fail(ErrorCode::Timeout, "private test bootstrap callback timed out");
    return future.get();
}
TEST(EngineSession, LuaBootstrapIsPrivateAndGenericCommandSendsNothing) {
    SessionPeer peer('b', false, Json(12345), false, scriptDebugCapability());
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    auto result = (*session)->request("script.debug.bootstrap", Json::object(), [](Result<Json>) {
        ADD_FAILURE() << "private bootstrap must never complete through a generic JSON callback";
    });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(peer.bootstraps.load(), 0U);
    EXPECT_EQ(peer.commands.load(), 0U);
    EXPECT_EQ((*session)->sentCommandCount(), 0U);
    (*session)->shutdown();
}
TEST(EngineSession, LuaBootstrapMissingOrUnavailableMetadataRefusesBeforeSend) {
    for (const auto& metadata :
         {std::optional<Json>{}, std::optional<Json>{scriptDebugCapability(false)},
          std::optional<Json>{Json::object().set("available", true).set("protocolVersion", 2)}}) {
        SessionPeer peer('b', false, Json(12345), false, metadata);
        Observations observed;
        auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
        ASSERT_TRUE(session);
        ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
        auto result = privateBootstrap(**session, observed.identity);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::Unsupported);
        EXPECT_EQ(peer.bootstraps.load(), 0U);
        (*session)->shutdown();
    }
}
TEST(EngineSession, LuaBootstrapMalformedCapabilityMetadataCannotAuthenticateAsDebugCapable) {
    for (const auto& metadata :
         {Json("invalid"), Json::object().set("available", "true").set("protocolVersion", 1),
          Json::object().set("available", true), Json::object().set("available", true).set("protocolVersion", -1)}) {
        SessionPeer peer('b', false, Json(12345), false, metadata);
        Observations observed;
        auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
        ASSERT_TRUE(session);
        ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
        EXPECT_EQ(observed.connections, 0U);
        EXPECT_EQ(peer.bootstraps.load(), 0U);
        EXPECT_FALSE((*session)->connected());
        (*session)->shutdown();
    }
}
TEST(EngineSession, LuaBootstrapBindsActualPidProjectAndConnectionBeforeSend) {
    SessionPeer peer('b', false, Json(12345), false, scriptDebugCapability());
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    for (unsigned change = 0; change < 5; ++change) {
        auto owner = observed.identity;
        if (change == 0)
            ++owner.processId;
        if (change == 1)
            ++owner.connectionGeneration;
        if (change == 2)
            owner.projectPath += "/other";
        if (change == 3)
            owner.instanceId = std::string(32, 'e');
        if (change == 4)
            owner.generation = std::string(32, 'e');
        auto result = privateBootstrap(**session, owner);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
    }
    EXPECT_EQ(peer.bootstraps.load(), 0U);
    (*session)->shutdown();
}
TEST(EngineSession, LuaTypedBootstrapHasConservativeLeaseAndRevokedConnectionToken) {
    SessionPeer peer('b', false, Json(12345), false, scriptDebugCapability());
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    auto revocation = (*session)->debugConnectionStopToken(observed.identity);
    ASSERT_TRUE(revocation);
    EXPECT_FALSE(revocation->stop_requested());
    const auto queuedAfter = std::chrono::steady_clock::now();
    auto result = privateBootstrap(**session, observed.identity);
    const auto completedBefore = std::chrono::steady_clock::now();
    ASSERT_TRUE(result) << result.error().describe();
    EXPECT_EQ(result->processId, 12345);
    EXPECT_EQ(result->projectPath, observed.identity.projectPath);
    EXPECT_EQ(result->instanceId, observed.identity.instanceId);
    EXPECT_EQ(result->generation, observed.identity.generation);
    EXPECT_EQ(result->connectionGeneration, observed.identity.connectionGeneration);
    EXPECT_EQ(result->debugSessionId, std::string(32, 'c'));
    EXPECT_EQ(result->port, 45678);
    EXPECT_GE(result->expiresAt, queuedAfter + 30s);
    EXPECT_LE(result->expiresAt, completedBefore + 30s);
    EXPECT_EQ(std::move(*result).takeCredential(), std::string(64, 'd'));
    EXPECT_EQ(peer.bootstraps.load(), 1U);
    EXPECT_EQ((*session)->sentCommandCount(), 0U);
    peer.heartbeats = false;
    ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
    EXPECT_TRUE(revocation->stop_requested());
    EXPECT_FALSE((*session)->debugConnectionStopToken(observed.identity));
    (*session)->shutdown();
}
TEST(EngineSession, LuaBootstrapRechecksAdmissionAndTrustBeforeSending) {
    SessionPeer peer('b', false, Json(12345), false, scriptDebugCapability());
    Observations observed;
    std::atomic<bool> trusted{true};
    auto options = optionsFor(peer);
    options.trusted = [&](std::string_view, std::uint64_t) { return trusted.load(); };
    auto session = EngineSession::create(std::move(options), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    auto result = privateBootstrap(**session, observed.identity, [&](const EngineIdentity&) -> Result<void> {
        trusted = false; // Cancellation/trust after a caller's successful policy must still deny.
        return {};
    });
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Untrusted);
    EXPECT_EQ(peer.bootstraps.load(), 0U);
    EXPECT_EQ((*session)->sentCommandCount(), 0U);
    (*session)->shutdown();
}
TEST(EngineSession, LuaBootstrapInvalidPrivateReplyCannotExposeCredentialInError) {
    for (const auto& overrides :
         {Json::object().set("processId", 999), Json::object().set("generation", std::string(32, 'f')),
          Json::object().set("host", "remote.invalid"), Json::object().set("expiresMs", 30001),
          Json::object().set("credential", "invalid-private-secret")}) {
        SessionPeer peer('b', false, Json(12345), false, scriptDebugCapability(), overrides);
        Observations observed;
        auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
        ASSERT_TRUE(session);
        ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
        auto result = privateBootstrap(**session, observed.identity);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::Corrupt);
        EXPECT_EQ(result.error().describe().find(std::string(64, 'd')), std::string::npos);
        EXPECT_EQ(result.error().describe().find("invalid-private-secret"), std::string::npos);
        EXPECT_EQ(peer.bootstraps.load(), 1U);
        (*session)->shutdown();
    }
}

TEST(EngineSession, PendingPlayerDebugPublishesUnavailableWithoutInventingAVm) {
    SessionPeer peer('b', false, Json(12345), false, scriptDebugCapability(), Json::object(), true);
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    EXPECT_EQ(observed.identity.role, "player");
    EXPECT_EQ(observed.state.state, "Unavailable");
    EXPECT_EQ(observed.state.mode, "Unavailable");
    EXPECT_FALSE(observed.state.playing);
    EXPECT_FALSE(observed.state.paused);
    EXPECT_TRUE(observed.state.playGeneration.empty());
    EXPECT_TRUE(privateBootstrap(**session, observed.identity));
    (*session)->shutdown();
}

Json crashSourceMetadata() {
#ifdef _WIN32
    constexpr std::string_view root = "C:\\engine-crashes", project = "C:/session-project";
#else
    constexpr std::string_view root = "/engine-crashes", project = "/session-project";
#endif
    return Json::object()
        .set("protocolVersion", 1)
        .set("sourceId", std::string(32, 'f'))
        .set("rootPath", root)
        .set("processId", 12345)
        .set("processStartUnixMs", 1794360000000LL)
        .set("processRole", "editor")
        .set("instanceId", std::string(32, 'a'))
        .set("generation", std::string(32, 'b'))
        .set("projectPath", project);
}
TEST(EngineSession, AuthenticatedCrashSourceSurvivesActualConnectionLossWithoutInventingCrashEvent) {
    SessionPeer peer('b', false, Json(12345), false, std::nullopt, Json::object(), false, crashSourceMetadata());
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    ASSERT_TRUE(observed.identity.crashArtifactSource);
    EXPECT_EQ(observed.identity.crashArtifactSource->processId, 12345);
    EXPECT_EQ(observed.identity.crashArtifactSource->projectPath, peer.endpoint.projectPath);
    EXPECT_EQ(observed.identity.crashArtifactSource->processStartUnixMs, 1794360000000LL);
    EXPECT_EQ(observed.state.identity.crashArtifactSource, observed.identity.crashArtifactSource);
    peer.heartbeats = false;
    ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
    EXPECT_EQ(observed.disconnectIdentity.crashArtifactSource, observed.identity.crashArtifactSource);
    EXPECT_EQ(observed.logs, 0U);
    EXPECT_EQ(observed.diagnostics, 0U);
    (*session)->shutdown();
}
TEST(EngineSession, MismatchedLiveCrashSourceCannotAuthenticateAsAnotherProcessOrProject) {
    for (const auto& mutation :
         {Json::object().set("processId", 999), Json::object().set("generation", std::string(32, 'e')),
          Json::object().set("projectPath", "/another/project"), Json::object().set("processStartUnixMs", 0),
          Json::object().set("sourceId", "invalid")}) {
        auto source = crashSourceMetadata();
        for (std::size_t i = 0; i < mutation.size(); ++i)
            source.set(mutation.keys()[i], mutation.items()[i]);
        SessionPeer peer('b', false, Json(12345), false, std::nullopt, Json::object(), false, source);
        Observations observed;
        auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
        ASSERT_TRUE(session);
        ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
        EXPECT_EQ(observed.connections, 0U);
        EXPECT_EQ(observed.states, 0U);
        EXPECT_FALSE((*session)->connected());
        (*session)->shutdown();
    }
}

// The peer below verifies the liveness policy using explicit typed native evidence. Actual OS
// debugger all-stop across 60+ seconds is a separate mandatory real-engine acceptance oracle.
constexpr NativeDebuggerBinding kNativeBinding{17, 3, 12345};
TEST(EngineSession, ExactNativeStopEvidenceHoldsHeartbeatOnlyAndContinuedUsesOriginalGrace) {
    SessionPeer peer;
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    auto lease = (*session)->bindNativeDebuggerPause(observed.identity, kNativeBinding);
    ASSERT_TRUE(lease);
    ASSERT_TRUE(lease->observe({kNativeBinding, 1, NativeAdapterEvent::Stopped}));
    peer.heartbeats = false;
    std::this_thread::sleep_for(700ms); // >2x the actual negotiated 250ms timeout; no fake heartbeat.
    EXPECT_TRUE((*session)->connected());
    EXPECT_TRUE(invoke(**session, "echo"));
    ASSERT_TRUE(lease->observe({kNativeBinding, 2, NativeAdapterEvent::Continued}));
    std::this_thread::sleep_for(75ms);
    EXPECT_TRUE((*session)->connected()); // Grace is separate from the old actual heartbeat timestamp.
    ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
    EXPECT_FALSE((*session)->connected());
    EXPECT_FALSE(lease->observe({kNativeBinding, 3, NativeAdapterEvent::Stopped}));
    (*session)->shutdown();
}
TEST(EngineSession, NativePauseCannotChangeRequestDeadlinesOrRescueTrustRevocation) {
    SessionPeer peer;
    Observations observed;
    std::atomic<bool> trusted{true};
    auto options = optionsFor(peer);
    options.requestTimeout = 100ms;
    options.trusted = [&](std::string_view, std::uint64_t) { return trusted.load(); };
    auto session = EngineSession::create(std::move(options), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    auto lease = (*session)->bindNativeDebuggerPause(observed.identity, kNativeBinding);
    ASSERT_TRUE(lease);
    ASSERT_TRUE(lease->observe({kNativeBinding, 1, NativeAdapterEvent::Stopped}));
    peer.heartbeats = false;
    auto held = invoke(**session, "test.hold");
    ASSERT_FALSE(held);
    EXPECT_EQ(held.error().code(), ErrorCode::Timeout);
    EXPECT_TRUE((*session)->connected());
    trusted = false;
    ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
    EXPECT_FALSE(lease->observe({kNativeBinding, 2, NativeAdapterEvent::Stopped}));
    (*session)->shutdown();
}
TEST(EngineSession, ForeignNativeEvidenceAndDuplicateBindingsCannotSuspendHeartbeatExpiry) {
    SessionPeer peer;
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    for (unsigned change = 0; change < 4; ++change) {
        auto wrong = observed.identity;
        if (change == 0)
            ++wrong.processId;
        if (change == 1)
            ++wrong.connectionGeneration;
        if (change == 2)
            wrong.projectPath += "/other";
        if (change == 3)
            wrong.generation = std::string(32, 'e');
        EXPECT_FALSE((*session)->bindNativeDebuggerPause(wrong, kNativeBinding));
    }
    auto lease = (*session)->bindNativeDebuggerPause(observed.identity, kNativeBinding);
    ASSERT_TRUE(lease);
    auto duplicate = (*session)->bindNativeDebuggerPause(observed.identity, kNativeBinding);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code(), ErrorCode::AlreadyExists);
    for (const auto binding : {NativeDebuggerBinding{18, 3, 12345}, NativeDebuggerBinding{17, 4, 12345},
                               NativeDebuggerBinding{17, 3, 999}}) {
        auto result = lease->observe({binding, 1, NativeAdapterEvent::Stopped});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
    }
    peer.heartbeats = false;
    ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
    (*session)->shutdown();
}
TEST(EngineSession, StaleNativeStoppedEvidenceAndRepeatedContinuedCannotExtendGrace) {
    SessionPeer peer;
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    auto lease = (*session)->bindNativeDebuggerPause(observed.identity, kNativeBinding);
    ASSERT_TRUE(lease);
    ASSERT_TRUE(lease->observe({kNativeBinding, 10, NativeAdapterEvent::Stopped}));
    peer.heartbeats = false;
    std::this_thread::sleep_for(400ms);
    ASSERT_TRUE((*session)->connected());
    ASSERT_TRUE(lease->observe({kNativeBinding, 11, NativeAdapterEvent::Continued}));
    ASSERT_TRUE(lease->observe({kNativeBinding, 10, NativeAdapterEvent::Stopped})); // Stale; ignored.
    for (std::uint64_t sequence = 12; sequence < 30 && (*session)->connected(); ++sequence) {
        std::this_thread::sleep_for(40ms);
        (void)lease->observe({kNativeBinding, sequence, NativeAdapterEvent::Continued});
    }
    EXPECT_FALSE((*session)->connected()); // Duplicate continued events cannot restart 250ms grace.
    (*session)->shutdown();
}
TEST(EngineSession, ActualHeartbeatClearsResumeGraceAndLaterSilenceExpiresNormally) {
    SessionPeer peer;
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    auto lease = (*session)->bindNativeDebuggerPause(observed.identity, kNativeBinding);
    ASSERT_TRUE(lease);
    ASSERT_TRUE(lease->observe({kNativeBinding, 1, NativeAdapterEvent::Stopped}));
    peer.heartbeats = false;
    std::this_thread::sleep_for(400ms);
    ASSERT_TRUE(lease->observe({kNativeBinding, 2, NativeAdapterEvent::Continued}));
    peer.heartbeats = true;
    std::this_thread::sleep_for(500ms);
    ASSERT_TRUE((*session)->connected());
    peer.heartbeats = false;
    ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
    (*session)->shutdown();
}
TEST(EngineSession, NativeEndOrLeaseRevocationNeverGrantsAContinuationGrace) {
    for (const bool ended : {true, false}) {
        SessionPeer peer;
        Observations observed;
        auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
        ASSERT_TRUE(session);
        ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
        auto lease = (*session)->bindNativeDebuggerPause(observed.identity, kNativeBinding);
        ASSERT_TRUE(lease);
        ASSERT_TRUE(lease->observe({kNativeBinding, 1, NativeAdapterEvent::Stopped}));
        peer.heartbeats = false;
        std::this_thread::sleep_for(400ms);
        if (ended)
            ASSERT_TRUE(lease->observe({kNativeBinding, 2, NativeAdapterEvent::Ended}));
        else
            lease->revoke();
        auto resume = lease->observe({kNativeBinding, 3, NativeAdapterEvent::Continued});
        ASSERT_FALSE(resume);
        EXPECT_EQ(resume.error().code(), ErrorCode::Cancelled);
        ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
        (*session)->shutdown();
    }
}
TEST(EngineSession, NativePauseNeverSuppressesPhysicalEofOrTransfersAcrossReconnect) {
    SessionPeer first('b'), second('d');
    Observations observed;
    auto options = optionsFor(first);
    options.rediscover = [&]() -> Result<EngineEndpoint> { return second.endpoint; };
    auto session = EngineSession::create(std::move(options), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    auto lease = (*session)->bindNativeDebuggerPause(observed.identity, kNativeBinding);
    ASSERT_TRUE(lease);
    ASSERT_TRUE(lease->observe({kNativeBinding, 1, NativeAdapterEvent::Stopped}));
    first.heartbeats = false;
    std::this_thread::sleep_for(400ms);
    ASSERT_TRUE((*session)->connected());
    first.stop(); // Actual OS socket EOF, not a heartbeat timer.
    ASSERT_TRUE(observed.wait([&] { return observed.connections == 2 && observed.states >= 2; }));
    EXPECT_FALSE(lease->observe({kNativeBinding, 2, NativeAdapterEvent::Stopped}));
    EXPECT_EQ(observed.identity.generation, second.endpoint.generation);
    second.heartbeats = false;
    ASSERT_TRUE(observed.wait([&] { return observed.disconnects >= 2; }));
    EXPECT_FALSE((*session)->connected()); // Old stopped evidence cannot protect the new connection.
    (*session)->shutdown();
}
TEST(EngineSession, ObservedCommandReplyCarriesActualSerializedEventCursor) {
    SessionPeer peer;
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    ASSERT_TRUE(invoke(**session, "test.logs")); // Actual log+diagnostic envelopes precede its reply.
    auto promise = std::make_shared<std::promise<Result<EngineCommandObservation>>>();
    auto result = promise->get_future();
    ASSERT_TRUE((*session)->requestObserved(
        "echo", Json::object().set("value", 42),
        [promise](Result<EngineCommandObservation> value) { promise->set_value(std::move(value)); }));
    ASSERT_EQ(result.wait_for(3s), std::future_status::ready);
    auto observedReply = result.get();
    ASSERT_TRUE(observedReply);
    EXPECT_EQ(observedReply->stateSequenceAtReply, 2U);
    EXPECT_EQ(observedReply->data.integerAt("value"), 42);
    (*session)->shutdown();
}

TEST(EngineSession, GracefulControlAnnouncementKeepsItsTypedCauseAcrossImmediateEof) {
    for (const auto command : {"test.project-change", "test.shutdown"}) {
        SessionPeer peer;
        Observations observed;
        auto options = optionsFor(peer);
        options.rediscover = [&]() -> Result<EngineEndpoint> {
            ADD_FAILURE() << "graceful control event must not auto-reconnect";
            return peer.endpoint;
        };
        auto session = EngineSession::create(std::move(options), observed.callbacks());
        ASSERT_TRUE(session);
        ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
        EXPECT_FALSE(invoke(**session, command));
        ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
        EXPECT_EQ(observed.disconnectCause, std::string_view(command) == "test.shutdown"
                                                ? EngineDisconnectCause::ServerShutdown
                                                : EngineDisconnectCause::ProjectChanged);
        EXPECT_FALSE((*session)->connected());
        (*session)->shutdown();
    }
}
TEST(EngineSession, RemotePlayOperationPhasesAreActualTypedStateAndUnknownPhaseDisconnects) {
    SessionPeer peer;
    Observations observed;
    auto session = EngineSession::create(optionsFor(peer), observed.callbacks());
    ASSERT_TRUE(session);
    ASSERT_TRUE(observed.wait([&] { return observed.states > 0; }));
    auto state = Json::object()
                     .set("state", "Edit")
                     .set("mode", "Edit")
                     .set("paused", false)
                     .set("frames", 0)
                     .set("operationId", std::string(32, 'd'))
                     .set("phase", "pending")
                     .set("playGeneration", "");
    ASSERT_TRUE(invoke(**session, "test.operation-state", state));
    ASSERT_TRUE(observed.wait([&] { return observed.states == 2; }));
    EXPECT_EQ(observed.state.phase, "pending");
    EXPECT_EQ(observed.state.operationId, std::string(32, 'd'));
    EXPECT_FALSE(observed.state.playing);
    EXPECT_TRUE(observed.state.playGeneration.empty());
    state.set("state", "Play").set("mode", "Play").set("phase", "playing").set("playGeneration", std::string(32, 'e'));
    ASSERT_TRUE(invoke(**session, "test.operation-state", state));
    ASSERT_TRUE(observed.wait([&] { return observed.states == 3; }));
    EXPECT_EQ(observed.state.playGeneration, std::string(32, 'e'));
    EXPECT_TRUE(observed.state.playing);
    state.set("state", "Edit").set("mode", "Edit").set("phase", "guessed").set("playGeneration", "");
    EXPECT_FALSE(invoke(**session, "test.operation-state", state));
    ASSERT_TRUE(observed.wait([&] { return observed.disconnects > 0; }));
    EXPECT_EQ(observed.states, 3U);
    EXPECT_EQ(observed.state.phase, "playing");
    (*session)->shutdown();
}
} // namespace
} // namespace frost::engine
