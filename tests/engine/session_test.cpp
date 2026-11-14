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
                         bool badInfoIdentity = false)
        : m_badProof(badProof), m_processId(std::move(processId)), m_badInfoIdentity(badInfoIdentity) {
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
    std::atomic<unsigned> commands{0}, clientProofs{0};
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
                                     .set("role", "editor")
                                     .set("contractVersion", 2)
                                     .set("sessionProtocolVersion", 2)
                                     .set("authenticated", true);
                        if (m_processId)
                            result.set("processId", *m_processId);
                        if (m_badInfoIdentity)
                            result.set("generation", std::string(32, 'f'));
                    } else if (method == "schema.export") {
                        result =
                            Json::object().set("commands", Json::array({Json::object().set("name", "echo"),
                                                                        Json::object().set("name", "play.pause"),
                                                                        Json::object().set("name", "play.resume")}));
                    } else if (method == "server.subscribe") {
                        result = identity()
                                     .set("sequence", sequence)
                                     .set("snapshots", Json::object().set("play.state", Json::object()
                                                                                            .set("state", "Edit")
                                                                                            .set("mode", "Edit")
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
    EngineIdentity identity;
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
        callbacks.disconnected = [this](const EngineDisconnect&) {
            std::lock_guard lock(mutex);
            ++disconnects;
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
} // namespace
} // namespace frost::engine
