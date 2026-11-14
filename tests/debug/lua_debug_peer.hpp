// SPDX-License-Identifier: Apache-2.0
// Real loopback standard-DAP protocol peer; no engine behavior/VM is simulated by this helper.
#pragma once
#include "frost/debug/lua_debug_endpoint.hpp"
#include "frost/rpc/framing.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
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

namespace frost::debug::test {
using namespace std::chrono_literals;
#ifdef _WIN32
using LuaSocket = SOCKET;
inline constexpr LuaSocket kBadLuaSocket = INVALID_SOCKET;
inline void closeLuaSocket(LuaSocket socket) { closesocket(socket); }
inline void wakeLuaSocket(LuaSocket socket) { shutdown(socket, SD_BOTH); }
#else
using LuaSocket = int;
inline constexpr LuaSocket kBadLuaSocket = -1;
inline void closeLuaSocket(LuaSocket socket) { close(socket); }
inline void wakeLuaSocket(LuaSocket socket) { shutdown(socket, SHUT_RDWR); }
#endif
inline bool readyLuaSocket(LuaSocket socket) {
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(socket, &ready);
    timeval timeout{2, 0};
    return select(static_cast<int>(socket + 1), &ready, nullptr, nullptr, &timeout) > 0;
}
inline std::string luaProof(std::string_view credential, std::string_view text) {
    crypto_auth_hmacsha256_state state{};
    std::array<unsigned char, 32> digest{};
    std::array<char, 65> out{};
    crypto_auth_hmacsha256_init(&state, reinterpret_cast<const unsigned char*>(credential.data()), credential.size());
    crypto_auth_hmacsha256_update(&state, reinterpret_cast<const unsigned char*>(text.data()), text.size());
    crypto_auth_hmacsha256_final(&state, digest.data());
    sodium_bin2hex(out.data(), out.size(), digest.data(), digest.size());
    return out.data();
}
inline constexpr std::string_view kLuaCredential = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
inline constexpr std::string_view kLuaServerNonce = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
inline LuaDebugIdentity luaIdentity() {
    return {"/fixture/project", std::string(32, 'a'), std::string(32, 'b'), std::string(32, 'c'), 12345, 7};
}
// Independent frozen wire transcript. The fixed vector is checked separately in endpoint tests.
inline std::string luaTranscript(std::string_view role, std::string_view client) {
    return "FrostEngine.ScriptDebug.v1\nrole=" + std::string(role) +
           "\ninstanceId=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\ngeneration=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
           "projectPath=/fixture/"
           "project\nprocessId=12345\ndebugSessionId=cccccccccccccccccccccccccccccccc\nclientNonce=" +
           std::string(client) + "\nserverNonce=" + std::string(kLuaServerNonce) + "\n";
}
inline rpc::Json luaIdentityBody() {
    return rpc::Json::object()
        .set("protocolVersion", 1)
        .set("instanceId", std::string(32, 'a'))
        .set("generation", std::string(32, 'b'))
        .set("projectPath", "/fixture/project")
        .set("processId", 12345)
        .set("debugSessionId", std::string(32, 'c'));
}
inline LuaDebugEndpoint luaEndpoint(std::uint16_t port) {
    LuaDebugEndpoint endpoint{std::string(kLuaCredential)};
    endpoint.identity = luaIdentity();
    endpoint.port = port;
    endpoint.expiresAt = std::chrono::steady_clock::now() + 30s;
    return endpoint;
}
inline LuaDebugAdmission admitLua() {
    return [](const LuaDebugIdentity& identity, std::string_view) -> Result<void> {
        if (identity != luaIdentity())
            return fail(ErrorCode::PermissionDenied, "test owner mismatch");
        return {};
    };
}

class LuaDapPeer {
public:
    using Handler = std::function<void(LuaDapPeer&)>;
    explicit LuaDapPeer(Handler handler, LuaDebugIdentity identity = luaIdentity()) : m_identity(std::move(identity)) {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return;
        m_winsock = true;
#endif
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == kBadLuaSocket)
            return;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 1) != 0)
            return;
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            return;
        port = ntohs(address.sin_port);
        worker = std::jthread([this, handler = std::move(handler)] {
            if (!readyLuaSocket(listener))
                return;
            const auto socket = accept(listener, nullptr, nullptr);
            if (socket == kBadLuaSocket)
                return;
            peer.store(socket);
            ++accepted;
            handler(*this);
            peer.store(kBadLuaSocket);
            wakeLuaSocket(socket);
            closeLuaSocket(socket);
            ended.store(true);
        });
    }
    ~LuaDapPeer() {
        if (listener != kBadLuaSocket)
            wakeLuaSocket(listener);
        const auto socket = peer.load();
        if (socket != kBadLuaSocket)
            wakeLuaSocket(socket);
        if (worker.joinable())
            worker.join();
        if (listener != kBadLuaSocket)
            closeLuaSocket(listener);
#ifdef _WIN32
        if (m_winsock)
            WSACleanup();
#endif
    }
    LuaDapPeer(const LuaDapPeer&) = delete;
    LuaDapPeer& operator=(const LuaDapPeer&) = delete;
    std::optional<rpc::Json> receive() {
        std::string header;
        char byte = 0;
        const auto socket = peer.load();
        while (!header.ends_with("\r\n\r\n")) {
            if (header.size() > 8192 || !readyLuaSocket(socket) || recv(socket, &byte, 1, 0) != 1)
                return {};
            header += byte;
        }
        constexpr std::string_view prefix = "Content-Length: ";
        if (!header.starts_with(prefix))
            return {};
        std::size_t size = 0;
        for (const char c : std::string_view(header).substr(prefix.size(), header.find("\r\n") - prefix.size())) {
            if (c < '0' || c > '9')
                return {};
            size = size * 10 + static_cast<unsigned>(c - '0');
            if (size > 1024 * 1024)
                return {};
        }
        std::string body(size, '\0');
        std::size_t offset = 0;
        while (offset < size) {
            if (!readyLuaSocket(socket))
                return {};
            const auto count = recv(socket, body.data() + offset, static_cast<int>(size - offset), 0);
            if (count <= 0)
                return {};
            offset += static_cast<std::size_t>(count);
        }
        auto parsed = rpc::parseJson(body);
        if (!parsed)
            return {};
        ++received;
        return *parsed;
    }
    bool sendBytes(std::string_view wire) {
        const auto socket = peer.load();
        while (!wire.empty()) {
#ifdef MSG_NOSIGNAL
            const auto count = send(socket, wire.data(), static_cast<int>(wire.size()), MSG_NOSIGNAL);
#else
            const auto count = send(socket, wire.data(), static_cast<int>(wire.size()), 0);
#endif
            if (count <= 0)
                return false;
            wire.remove_prefix(static_cast<std::size_t>(count));
        }
        return true;
    }
    bool respond(const rpc::Json& request, rpc::Json body = rpc::Json::object(), bool success = true) {
        return sendBytes(rpc::encodeFrame(rpc::Json::object()
                                              .set("seq", sequence++)
                                              .set("type", "response")
                                              .set("request_seq", request.integerAt("seq").value_or(0))
                                              .set("command", request.stringAt("command"))
                                              .set("success", success)
                                              .set("body", std::move(body))
                                              .dump()));
    }
    bool event(std::string name, rpc::Json body) {
        return sendBytes(rpc::encodeFrame(rpc::Json::object()
                                              .set("seq", sequence++)
                                              .set("type", "event")
                                              .set("event", std::move(name))
                                              .set("body", std::move(body))
                                              .dump()));
    }
    bool authenticate(std::function<void(rpc::Json&)> modifyChallenge = {},
                      std::function<void(rpc::Json&)> modifyAck = {}) {
        auto challenge = receive();
        if (!challenge || challenge->integerAt("seq") != 1 || challenge->stringAt("command") != "frost/authChallenge")
            return false;
        const auto* args = challenge->find("arguments");
        if (!args || args->stringAt("debugSessionId") != m_identity.debugSessionId)
            return false;
        const std::string client(args->stringAt("clientNonce"));
        if (client.size() != 64)
            return false;
        auto body = identityBody()
                        .set("algorithm", "hmac-sha256")
                        .set("clientNonce", client)
                        .set("serverNonce", kLuaServerNonce)
                        .set("serverProof", luaProof(kLuaCredential, ownerTranscript("server", client)));
        if (modifyChallenge)
            modifyChallenge(body);
        if (!respond(*challenge, std::move(body)))
            return false;
        auto prove = receive();
        if (!prove)
            return false;
        ++proofAttempts;
        if (prove->integerAt("seq") != 2 || prove->stringAt("command") != "frost/authProve")
            return false;
        const auto* proof = prove->find("arguments");
        if (!proof || proof->stringAt("proof") != luaProof(kLuaCredential, ownerTranscript("client", client)))
            return false;
        auto ack = identityBody().set("authenticated", true);
        if (modifyAck)
            modifyAck(ack);
        if (!respond(*prove, std::move(ack)))
            return false;
        authenticated.store(true);
        return true;
    }
    std::uint16_t port = 0;
    std::atomic<int> accepted{0}, received{0}, proofAttempts{0};
    std::atomic<bool> authenticated{false}, ended{false};

private:
    rpc::Json identityBody() const {
        return rpc::Json::object()
            .set("protocolVersion", 1)
            .set("instanceId", m_identity.instanceId)
            .set("generation", m_identity.generation)
            .set("projectPath", m_identity.projectPath)
            .set("processId", m_identity.processId)
            .set("debugSessionId", m_identity.debugSessionId);
    }
    std::string ownerTranscript(std::string_view role, std::string_view client) const {
        return "FrostEngine.ScriptDebug.v1\nrole=" + std::string(role) + "\ninstanceId=" + m_identity.instanceId +
               "\ngeneration=" + m_identity.generation + "\nprojectPath=" + m_identity.projectPath +
               "\nprocessId=" + std::to_string(m_identity.processId) + "\ndebugSessionId=" + m_identity.debugSessionId +
               "\nclientNonce=" + std::string(client) + "\nserverNonce=" + std::string(kLuaServerNonce) + "\n";
    }
    LuaDebugIdentity m_identity;
    LuaSocket listener = kBadLuaSocket;
    std::atomic<LuaSocket> peer{kBadLuaSocket};
    std::jthread worker;
    std::int64_t sequence = 1;
#ifdef _WIN32
    bool m_winsock = false;
#endif
};
} // namespace frost::debug::test
