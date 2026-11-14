// SPDX-License-Identifier: Apache-2.0
// Mutual HMAC authentication of the private Content-Length FrostEngine Lua DAP socket.
#include "frost/debug/lua_debug_endpoint.hpp"

#include "frost/io/path.hpp"
#include "frost/rpc/framing.hpp"

#include "lua_dap_transport.hpp"
#include "lua_dap_wire.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include <sodium.h>

namespace frost::debug {
LuaDebugEndpoint::LuaDebugEndpoint(std::string credential) : m_credential(std::move(credential)) {}
LuaDebugEndpoint::~LuaDebugEndpoint() { sodium_memzero(m_credential.data(), m_credential.size()); }
LuaDebugEndpoint& LuaDebugEndpoint::operator=(LuaDebugEndpoint&& other) noexcept {
    if (this != &other) {
        sodium_memzero(m_credential.data(), m_credential.size());
        m_credential = std::move(other.m_credential);
        identity = std::move(other.identity);
        port = other.port;
        expiresAt = other.expiresAt;
    }
    return *this;
}
class LuaDebugEndpointAccess {
public:
    static std::string_view credential(const LuaDebugEndpoint& endpoint) { return endpoint.m_credential; }
    static std::string consume(LuaDebugEndpoint& endpoint) { return std::move(endpoint.m_credential); }
    static LuaDebugConnection connection(std::unique_ptr<IDapTransport> transport, LuaDebugIdentity identity,
                                         std::function<Result<void>(std::string_view)> writeAdmission) {
        return LuaDebugConnection(std::move(transport), std::move(identity), std::move(writeAdmission));
    }
};
namespace {
using Json = rpc::Json;
bool hex(std::string_view text, std::size_t length) {
    return text.size() == length &&
           std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool matches(const Json& body, const LuaDebugIdentity& identity) {
    return body.stringAt("instanceId") == identity.instanceId && body.stringAt("generation") == identity.generation &&
           body.stringAt("projectPath") == identity.projectPath && body.integerAt("processId") == identity.processId &&
           body.stringAt("debugSessionId") == identity.debugSessionId && body.integerAt("protocolVersion") == 1;
}
std::string transcript(const LuaDebugIdentity& identity, std::string_view role, std::string_view client,
                       std::string_view server) {
    // Frozen public contract: Frost-Tests/Data/Automation/ScriptDebugProtocol-v1.json in the engine.
    return "FrostEngine.ScriptDebug.v1\nrole=" + std::string(role) + "\ninstanceId=" + identity.instanceId +
           "\ngeneration=" + identity.generation + "\nprojectPath=" + identity.projectPath +
           "\nprocessId=" + std::to_string(identity.processId) + "\ndebugSessionId=" + identity.debugSessionId +
           "\nclientNonce=" + std::string(client) + "\nserverNonce=" + std::string(server) + "\n";
}
std::string mac(std::string_view credential, std::string_view text) {
    crypto_auth_hmacsha256_state state{};
    std::array<unsigned char, crypto_auth_hmacsha256_BYTES> bytes{};
    std::array<char, crypto_auth_hmacsha256_BYTES * 2 + 1> encoded{};
    crypto_auth_hmacsha256_init(&state, reinterpret_cast<const unsigned char*>(credential.data()), credential.size());
    crypto_auth_hmacsha256_update(&state, reinterpret_cast<const unsigned char*>(text.data()), text.size());
    crypto_auth_hmacsha256_final(&state, bytes.data());
    sodium_bin2hex(encoded.data(), encoded.size(), bytes.data(), bytes.size());
    sodium_memzero(&state, sizeof(state));
    sodium_memzero(bytes.data(), bytes.size());
    return encoded.data();
}
Result<Json> exchange(io::LoopbackStream& stream, std::int64_t sequence, std::string_view command, Json arguments,
                      std::int64_t& lastServerSequence) {
    auto request = Json::object()
                       .set("seq", sequence)
                       .set("type", "request")
                       .set("command", command)
                       .set("arguments", std::move(arguments));
    if (auto sent = stream.write(rpc::encodeFrame(request.dump())); !sent)
        return std::unexpected(sent.error());
    auto frame = detail::readLuaDapFrame(stream);
    if (!frame)
        return std::unexpected(frame.error());
    auto response = rpc::parseJson(*frame);
    if (!response || !response->isObject() || response->stringAt("type") != "response" ||
        response->integerAt("request_seq") != sequence || response->stringAt("command") != command ||
        !response->integerAt("seq") || *response->integerAt("seq") <= lastServerSequence ||
        !response->boolAt("success"))
        return fail(ErrorCode::Corrupt, "invalid Lua DAP authentication response");
    lastServerSequence = *response->integerAt("seq");
    if (response->boolAt("success") != true)
        return fail(ErrorCode::PermissionDenied, "Lua DAP authentication was refused");
    const auto* body = response->find("body");
    if (!body || !body->isObject())
        return fail(ErrorCode::Corrupt, "Lua DAP authentication omitted its body");
    return *body;
}
} // namespace

Result<LuaDebugConnection> connectLuaDebugEndpoint(LuaDebugEndpoint endpoint, LuaDebugAdmission admission,
                                                   io::LoopbackStream::Deadline deadline, std::stop_token stop,
                                                   LuaDebugTransportCallbacks callbacks) {
    const auto& identity = endpoint.identity;
    const auto credential = LuaDebugEndpointAccess::credential(endpoint);
    const auto now = std::chrono::steady_clock::now();
    if (!admission || endpoint.port == 0 || !hex(credential, 64) || !hex(identity.instanceId, 32) ||
        !hex(identity.generation, 32) || !hex(identity.debugSessionId, 32) || identity.processId <= 0 ||
        identity.connectionGeneration == 0 || !io::isAbsolutePath(identity.projectPath) ||
        identity.projectPath.size() > 32768 || identity.projectPath.find_first_of("\r\n") != std::string::npos ||
        identity.projectPath.find('\0') != std::string::npos)
        return fail(ErrorCode::InvalidArgument, "invalid private Lua debug endpoint identity");
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "Lua debug setup was cancelled");
    if (endpoint.expiresAt <= now || deadline <= now)
        return fail(ErrorCode::Timeout, "Lua debug bootstrap lease or setup deadline expired");
    if (endpoint.expiresAt - now > std::chrono::seconds(30))
        return fail(ErrorCode::InvalidArgument, "Lua debug bootstrap lease exceeds its bound");
    if (sodium_init() < 0)
        return fail(ErrorCode::Internal, "Lua debug authentication is unavailable");
    auto admitted = admission(identity, "frost/authChallenge");
    if (!admitted)
        return fail(admitted.error().code(), "Lua debug setup admission was revoked");
    auto stream = io::LoopbackStream::connect(endpoint.port, std::min(deadline, endpoint.expiresAt), stop);
    if (!stream)
        return std::unexpected(stream.error());
    std::array<unsigned char, 32> random{};
    std::array<char, 65> clientText{};
    randombytes_buf(random.data(), random.size());
    sodium_bin2hex(clientText.data(), clientText.size(), random.data(), random.size());
    sodium_memzero(random.data(), random.size());
    const std::string client(clientText.data());
    std::int64_t lastServerSequence = 0;
    admitted = admission(identity, "frost/authChallenge");
    if (!admitted || stop.stop_requested())
        return fail(admitted ? ErrorCode::Cancelled : admitted.error().code(), "Lua debug setup admission was revoked");
    auto challenge = exchange(**stream, 1, "frost/authChallenge",
                              Json::object().set("debugSessionId", identity.debugSessionId).set("clientNonce", client),
                              lastServerSequence);
    if (!challenge)
        return std::unexpected(challenge.error());
    const auto server = challenge->stringAt("serverNonce");
    const auto proof = challenge->stringAt("serverProof");
    if (!matches(*challenge, identity) || challenge->stringAt("algorithm") != "hmac-sha256" ||
        challenge->stringAt("clientNonce") != client || !hex(server, 64) || !hex(proof, 64))
        return fail(ErrorCode::PermissionDenied, "Lua debug server identity or challenge does not match");
    const std::string expected = mac(credential, transcript(identity, "server", client, server));
    if (sodium_memcmp(expected.data(), proof.data(), expected.size()) != 0)
        return fail(ErrorCode::PermissionDenied, "Lua debug server authentication failed");
    admitted = admission(identity, "frost/authProve");
    if (!admitted || stop.stop_requested())
        return fail(admitted ? ErrorCode::Cancelled : admitted.error().code(), "Lua debug setup admission was revoked");
    auto proved = exchange(**stream, 2, "frost/authProve",
                           Json::object().set("proof", mac(credential, transcript(identity, "client", client, server))),
                           lastServerSequence);
    if (!proved)
        return std::unexpected(proved.error());
    if (!matches(*proved, identity) || proved->boolAt("authenticated") != true)
        return fail(ErrorCode::PermissionDenied, "Lua debug authentication identity changed");
    admitted = admission(identity, "<adopt>");
    if (!admitted || stop.stop_requested())
        return fail(admitted ? ErrorCode::Cancelled : admitted.error().code(), "Lua debug setup admission was revoked");
    if (std::chrono::steady_clock::now() >= std::min(deadline, endpoint.expiresAt))
        return fail(ErrorCode::Timeout, "Lua debug setup deadline expired");
    auto writeAdmission = [admission, identity, stop](std::string_view command) -> Result<void> {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Lua debugger owner was revoked");
        return admission(identity, command);
    };
    (*stream)->beginDuplex();
    auto transport =
        detail::makeLuaDapTransport(std::move(*stream), identity, LuaDebugEndpointAccess::consume(endpoint),
                                    std::move(admission), stop, lastServerSequence, std::move(callbacks));
    return LuaDebugEndpointAccess::connection(std::move(transport), std::move(endpoint.identity),
                                              std::move(writeAdmission));
}

Result<LuaDebugPlayConnection> connectLuaDebugPlayEndpoint(std::uint16_t, std::string_view,
                                                           io::LoopbackStream::Deadline, std::stop_token) {
    return fail(ErrorCode::Unsupported, "the experimental Lua token protocol has been retired");
}
Result<std::unique_ptr<io::LoopbackStream>> connectLuaDebugEndpoint(std::uint16_t, std::string_view,
                                                                    io::LoopbackStream::Deadline, bool, std::stop_token,
                                                                    std::uint64_t*) {
    return fail(ErrorCode::Unsupported, "the experimental Lua token protocol has been retired");
}
} // namespace frost::debug
