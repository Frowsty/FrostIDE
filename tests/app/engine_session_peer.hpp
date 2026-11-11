// SPDX-License-Identifier: Apache-2.0
// Independent local contract-v2 peer for app integration tests; never real-editor acceptance.
#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>

#include <unistd.h>
#endif

#include "frost/io/file_system.hpp"
#include "frost/rpc/json.hpp"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace frost::app::test {

// Real loopback transport, mutual HMAC and exact project/session envelopes. Commands can be held,
// rejected or completed explicitly; state changes only through independently delivered push events.
// The fixture implements protocol inputs, not FrostEditor semantics or acceptance evidence.
class EngineSessionPeer {
public:
    using Json = rpc::Json;
    explicit EngineSessionPeer(std::string project, char identity = 'a')
        : m_project(io::realFileSystem().canonicalPath(project).value_or(std::move(project))), m_instance(32, identity),
          m_generation(32, identity == 'b' ? 'c' : 'b') {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return;
        m_initialized = true;
#endif
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
        m_port = ntohs(address.sin_port);
        m_worker = std::jthread([this](std::stop_token stop) { serve(stop); });
    }
    ~EngineSessionPeer() {
        stop();
#ifdef _WIN32
        if (m_initialized)
            WSACleanup();
#endif
    }
    EngineSessionPeer(const EngineSessionPeer&) = delete;
    EngineSessionPeer& operator=(const EngineSessionPeer&) = delete;
    const std::string& instanceId() const noexcept { return m_instance; }
    std::uint16_t port() const noexcept { return m_port; }
    Json record(std::string_view advertisedProject = {}) const {
        return identity()
            .set("host", "127.0.0.1")
            .set("port", static_cast<std::uint64_t>(m_port))
            .set("tokenRequired", true)
            .set("token", m_secret)
            .set("contractVersion", 2)
            .set("projectPath", advertisedProject.empty() ? m_project : std::string(advertisedProject));
    }
    void stop() {
        m_worker.request_stop();
        {
            std::lock_guard lock(m_mutex);
            if (m_socket != kInvalid) {
#ifdef _WIN32
                shutdown(m_socket, SD_BOTH);
#else
                shutdown(m_socket, SHUT_RDWR);
#endif
            }
        }
        if (m_worker.joinable())
            m_worker.join();
        if (m_listener != kInvalid) {
            closeSocket(m_listener);
            m_listener = kInvalid;
        }
    }
    std::atomic<bool> stallChallenge{false}, closed{false}, heartbeats{true};
    std::atomic<std::int64_t> processId{0};
    std::atomic<unsigned> authenticated{0};
    std::vector<Json> requests(std::string_view method) const {
        std::lock_guard lock(m_mutex);
        std::vector<Json> found;
        for (const auto& request : m_requests)
            if (request.stringAt("method") == method)
                found.push_back(request);
        return found;
    }
    std::size_t replies(std::string_view method) const {
        std::lock_guard lock(m_mutex);
        const auto found = m_replied.find(std::string(method));
        return found == m_replied.end() ? 0 : found->second;
    }
    void hold(std::string method) {
        std::lock_guard lock(m_mutex);
        m_held.insert(std::move(method));
    }
    void result(std::string method, Json value) {
        std::lock_guard lock(m_mutex);
        m_results.insert_or_assign(std::move(method), std::move(value));
    }
    void refuse(std::string method, int code, std::string message) {
        std::lock_guard lock(m_mutex);
        m_errors.insert_or_assign(std::move(method),
                                  Json::object().set("code", code).set("message", std::move(message)));
    }
    void release(std::string_view method, Json value) {
        std::lock_guard lock(m_mutex);
        bool found = false;
        for (auto at = m_pending.begin(); at != m_pending.end();) {
            if (at->second == method) {
                found = true;
                m_outgoing.push_back(
                    {Json::object().set("jsonrpc", "2.0").set("id", at->first).set("result", value), at->second});
                at = m_pending.erase(at);
            } else
                ++at;
        }
        // Requests are observable as soon as their frame is parsed. Remember an early release
        // too, so a test cannot accidentally pass without transmitting its intended late reply.
        if (!found)
            m_releases.insert_or_assign(std::string(method), std::move(value));
    }
    void event(std::string topic, Json data) {
        std::lock_guard lock(m_mutex);
        m_events.emplace_back(std::move(topic), std::move(data));
    }
    void log(std::string message, std::string path = {}, int line = 0) {
        auto data = Json::object().set("level", "info").set("logger", "Fixture").set("message", std::move(message));
        if (!path.empty())
            data.set("source", Json::object().set("path", std::move(path)).set("line", line));
        event("log.entry", std::move(data));
    }
    void state(std::string mode, bool paused = false, std::uint64_t frames = 0) {
        event("play.state", Json::object()
                                .set("state", paused ? std::string("Paused") : mode)
                                .set("mode", mode)
                                .set("paused", paused)
                                .set("frames", frames));
    }

private:
#ifdef _WIN32
    using Socket = SOCKET;
    static constexpr Socket kInvalid = INVALID_SOCKET;
    static void closeSocket(Socket value) { closesocket(value); }
#else
    using Socket = int;
    static constexpr Socket kInvalid = -1;
    static void closeSocket(Socket value) { close(value); }
#endif
    struct Outgoing {
        Json value;
        std::string method;
    };
    bool ready(Socket value, long milliseconds) const {
        fd_set read;
        FD_ZERO(&read);
        FD_SET(value, &read);
        timeval duration{0, milliseconds * 1000};
        return select(static_cast<int>(value + 1), &read, nullptr, nullptr, &duration) > 0;
    }
    Json identity() const {
        return Json::object()
            .set("instanceId", m_instance)
            .set("generation", m_generation)
            .set("projectPath", m_project);
    }
    std::string transcript(std::string_view role, std::string_view client) const {
        return "FrostEngine.Session.v2\nrole=" + std::string(role) + "\ninstanceId=" + m_instance +
               "\ngeneration=" + m_generation + "\nprojectPath=" + m_project + "\nclientNonce=" + std::string(client) +
               "\nserverNonce=" + std::string(64, 'c') +
               "\ncapabilities=session.v2,auth.hmac-sha256,events.v1,heartbeat.v1\nheartbeatIntervalMs="
               "100\nheartbeatTimeoutMs=2000\n";
    }
    std::string mac(std::string_view text) const {
        const QByteArray bytes(text.data(), static_cast<qsizetype>(text.size()));
        const QByteArray key(m_secret.data(), static_cast<qsizetype>(m_secret.size()));
        return QMessageAuthenticationCode::hash(bytes, key, QCryptographicHash::Sha256).toHex().toStdString();
    }
    Json schema() const {
        auto commands = Json::array();
        for (const std::string_view name :
             {"play.start", "play.stop", "play.state", "play.pause", "play.step", "play.simulate", "script.validate",
              "test.run", "log.get", "editor.focus"}) {
            const bool mutation = (name.starts_with("play.") && name != "play.state") || name == "test.run";
            commands.push(Json::object()
                              .set("name", name)
                              .set("category", "Fixture")
                              .set("flags", mutation ? Json::array({"RequiresEditor", "Mutating"})
                                                     : Json::array({"RequiresEditor"}))
                              .set("idePolicy", Json::object()
                                                    .set("allowed", true)
                                                    .set("states", Json::array({"Edit", "Play", "Simulate", "Paused",
                                                                                "DebuggerPaused"}))
                                                    .set("requiresMutationPermission", mutation)
                                                    .set("requiredCapabilities", Json::array({"session.v2"}))));
        }
        // Present in schema, deliberately outside IDE policy: the refusal test must not send it.
        commands.push(Json::object()
                          .set("name", "scene.delete")
                          .set("category", "Scene")
                          .set("flags", Json::array({"Mutating"})));
        return Json::object()
            .set("schema", "frostengine.schema-export")
            .set("schemaVersion", 1)
            .set("engine", Json::object().set("version", "fixture"))
            .set("costTags", Json::array())
            .set("bindingKinds", Json::array())
            .set("components", Json::array())
            .set("lua", Json::object()
                            .set("version", "Lua 5.4")
                            .set("scriptsRoot", "Scripts")
                            .set("modulePathPattern", "Scripts/?.lua")
                            .set("entries", Json::array())
                            .set("aliases", Json::array()))
            .set("events", Json::array())
            .set("commands", std::move(commands));
    }
    bool sendJson(Socket socket, const Json& value) {
        const std::string bytes = value.dump() + "\n";
        std::size_t offset = 0;
        while (offset < bytes.size()) {
#ifdef _WIN32
            const auto written = send(socket, bytes.data() + offset, static_cast<int>(bytes.size() - offset), 0);
#else
            const auto written = send(socket, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
#endif
            if (written <= 0)
                return false;
            offset += static_cast<std::size_t>(written);
        }
        return true;
    }
    void serve(std::stop_token stop) {
        while (!stop.stop_requested() && !ready(m_listener, 10)) {
        }
        if (stop.stop_requested())
            return;
        const Socket socket = accept(m_listener, nullptr, nullptr);
        if (socket == kInvalid)
            return;
        {
            std::lock_guard lock(m_mutex);
            m_socket = socket;
        }
        bool paired = false, subscribed = false;
        std::uint64_t sequence = 0;
        std::string buffer, clientNonce;
        auto heartbeatAt = std::chrono::steady_clock::now();
        bool alive = true;
        while (alive && !stop.stop_requested()) {
            if (subscribed) {
                std::deque<Outgoing> outgoing;
                std::deque<std::pair<std::string, Json>> events;
                {
                    std::lock_guard lock(m_mutex);
                    outgoing.swap(m_outgoing);
                    events.swap(m_events);
                }
                for (const auto& reply : outgoing) {
                    if (!sendJson(socket, reply.value)) {
                        alive = false;
                        break;
                    }
                    std::lock_guard lock(m_mutex);
                    ++m_replied[reply.method];
                }
                for (auto& [topic, data] : events) {
                    if (!sendJson(socket, Json::object()
                                              .set("jsonrpc", "2.0")
                                              .set("method", "engine.event")
                                              .set("params", identity()
                                                                 .set("event", topic)
                                                                 .set("sequence", ++sequence)
                                                                 .set("data", std::move(data))))) {
                        alive = false;
                        break;
                    }
                }
                if (heartbeats.load() && std::chrono::steady_clock::now() >= heartbeatAt) {
                    if (!sendJson(socket, Json::object()
                                              .set("jsonrpc", "2.0")
                                              .set("method", "engine.heartbeat")
                                              .set("params", identity().set("contractVersion", 2))))
                        break;
                    heartbeatAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
                }
            }
            if (!alive || !ready(socket, 2))
                continue;
            std::array<char, 4096> chunk{};
            const auto read = recv(socket, chunk.data(), static_cast<int>(chunk.size()), 0);
            if (read <= 0)
                break;
            buffer.append(chunk.data(), static_cast<std::size_t>(read));
            if (buffer.size() > 1024U * 1024U)
                break;
            for (;;) {
                const auto end = buffer.find('\n');
                if (end == std::string::npos)
                    break;
                auto request = rpc::parseJson(std::string_view(buffer).substr(0, end));
                buffer.erase(0, end + 1);
                if (!request || !request->isObject() || request->stringAt("jsonrpc") != "2.0" ||
                    !request->unsignedAt("id")) {
                    alive = false;
                    break;
                }
                const std::string method(request->stringAt("method"));
                const auto id = request->unsignedAt("id").value_or(0);
                const auto* params = request->find("params");
                if (!params || !params->isObject()) {
                    alive = false;
                    break;
                }
                {
                    std::lock_guard lock(m_mutex);
                    m_requests.push_back(*request);
                }
                Json result;
                if (method == "auth.challenge") {
                    if (stallChallenge.load())
                        continue;
                    if (paired || !clientNonce.empty()) {
                        alive = false;
                        break;
                    }
                    clientNonce = params ? std::string(params->stringAt("clientNonce")) : std::string{};
                    if (clientNonce.size() != 64 || !params || params->stringAt("projectPath") != m_project) {
                        alive = false;
                        break;
                    }
                    result = identity()
                                 .set("contractVersion", 2)
                                 .set("algorithm", "hmac-sha256")
                                 .set("clientNonce", clientNonce)
                                 .set("serverNonce", std::string(64, 'c'))
                                 .set("capabilities",
                                      Json::array({"session.v2", "auth.hmac-sha256", "events.v1", "heartbeat.v1"}))
                                 .set("heartbeatIntervalMs", 100)
                                 .set("heartbeatTimeoutMs", 2000)
                                 .set("serverProof", mac(transcript("server", clientNonce)));
                } else if (method == "auth.prove") {
                    if (paired || clientNonce.size() != 64) {
                        alive = false;
                        break;
                    }
                    paired = params && params->stringAt("proof") == mac(transcript("client", clientNonce));
                    if (!paired) {
                        alive = false;
                        break;
                    }
                    ++authenticated;
                    result = identity().set("authenticated", true).set("contractVersion", 2);
                } else {
                    const auto* session = request->find("session");
                    if (!paired || !session || *session != identity()) {
                        alive = false;
                        break;
                    }
                    if (method == "server.info") {
                        result = identity()
                                     .set("contractVersion", 2)
                                     .set("sessionProtocolVersion", 2)
                                     .set("authenticated", true)
                                     .set("role", "editor");
                        if (processId.load() > 0)
                            result.set("processId", processId.load());
                    } else if (method == "schema.export")
                        result = schema();
                    else if (method == "server.subscribe") {
                        subscribed = true;
                        result = identity()
                                     .set("sequence", sequence)
                                     .set("snapshots", Json::object().set("play.state", Json::object()
                                                                                            .set("state", "Edit")
                                                                                            .set("mode", "Edit")
                                                                                            .set("paused", false)
                                                                                            .set("frames", 0)));
                    } else {
                        bool held;
                        Json error;
                        {
                            std::lock_guard lock(m_mutex);
                            held = m_held.contains(method);
                            if (held) {
                                if (const auto released = m_releases.find(method); released != m_releases.end()) {
                                    m_outgoing.push_back({Json::object()
                                                              .set("jsonrpc", "2.0")
                                                              .set("id", id)
                                                              .set("result", released->second),
                                                          method});
                                    m_releases.erase(released);
                                } else {
                                    m_pending.emplace(id, method);
                                }
                            }
                            if (const auto at = m_results.find(method); at != m_results.end())
                                result = at->second;
                            else
                                result = params ? *params : Json::object();
                            if (const auto at = m_errors.find(method); at != m_errors.end())
                                error = at->second;
                        }
                        if (held)
                            continue;
                        if (!error.isNull()) {
                            if (!sendJson(
                                    socket,
                                    Json::object().set("jsonrpc", "2.0").set("id", id).set("error", std::move(error))))
                                alive = false;
                            continue;
                        }
                    }
                }
                if (!sendJson(socket,
                              Json::object().set("jsonrpc", "2.0").set("id", id).set("result", std::move(result)))) {
                    alive = false;
                    break;
                }
                {
                    std::lock_guard lock(m_mutex);
                    ++m_replied[method];
                }
            }
        }
        {
            std::lock_guard lock(m_mutex);
            m_socket = kInvalid;
            closeSocket(socket);
        }
        closed = true;
    }
    std::string m_project, m_instance, m_generation;
    const std::string m_secret = "local-app-peer-secret";
    Socket m_listener = kInvalid, m_socket = kInvalid;
    std::uint16_t m_port = 0;
    std::jthread m_worker;
    mutable std::mutex m_mutex;
    std::vector<Json> m_requests;
    std::map<std::string, std::size_t> m_replied;
    std::set<std::string> m_held;
    std::map<std::string, Json> m_results, m_errors, m_releases;
    std::map<std::uint64_t, std::string> m_pending;
    std::deque<Outgoing> m_outgoing;
    std::deque<std::pair<std::string, Json>> m_events;
#ifdef _WIN32
    bool m_initialized = false;
#endif
};
} // namespace frost::app::test
