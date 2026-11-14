// SPDX-License-Identifier: Apache-2.0
// Private FrostEngine Lua DAP lease and mutually authenticated loopback connection.
#pragma once

#include "frost/core/error.hpp"
#include "frost/debug/dap_client.hpp"
#include "frost/io/loopback_stream.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace frost::debug {

// Exact identity copied from the private authenticated R6 bootstrap, never from public discovery.
// connectionGeneration is IDE-local and participates in admission, not the wire HMAC transcript.
struct LuaDebugIdentity {
    std::string projectPath, instanceId, generation, debugSessionId;
    std::int64_t processId = 0;
    std::uint64_t connectionGeneration = 0;
    friend bool operator==(const LuaDebugIdentity&, const LuaDebugIdentity&) = default;
};

// Required thread-safe, fast admission. Capture exact physical project/trust revision, engine
// connection and registry session id/controller generation; never inspect QObjects on a worker.
// Called before connecting/authenticating and before every outgoing standard DAP frame.
using LuaDebugAdmission = std::function<Result<void>(const LuaDebugIdentity&, std::string_view command)>;

// Observes an actual peer ACK for an outgoing standard disconnect request. Invoked at most once
// on an I/O thread; close/refusal before that ACK reports failure. This is not a local detach result.
struct LuaDebugTransportCallbacks {
    std::function<void(Result<void>)> onAttached;
    std::function<void(Result<void>)> onDetached;
};

// One-use private lease. No JSON/formatter conversion exists. Move-only; erased on destruction.
class LuaDebugEndpoint {
public:
    explicit LuaDebugEndpoint(std::string credential = {});
    ~LuaDebugEndpoint();
    LuaDebugEndpoint(const LuaDebugEndpoint&) = delete;
    LuaDebugEndpoint& operator=(const LuaDebugEndpoint&) = delete;
    LuaDebugEndpoint(LuaDebugEndpoint&&) noexcept = default;
    LuaDebugEndpoint& operator=(LuaDebugEndpoint&&) noexcept;
    LuaDebugIdentity identity;
    std::uint16_t port = 0;
    std::chrono::steady_clock::time_point expiresAt{};

private:
    friend class LuaDebugEndpointAccess;
    std::string m_credential;
};

// Only connectLuaDebugEndpoint can create an authenticated handoff. Consumed exactly once by the
// adapter factory; destruction closes a connection that was never adopted by a GUI session.
class LuaDebugConnection {
public:
    LuaDebugConnection(LuaDebugConnection&&) noexcept = default;
    LuaDebugConnection& operator=(LuaDebugConnection&&) noexcept = default;
    LuaDebugConnection(const LuaDebugConnection&) = delete;
    LuaDebugConnection& operator=(const LuaDebugConnection&) = delete;
    [[nodiscard]] const LuaDebugIdentity& identity() const noexcept { return m_identity; }
    [[nodiscard]] std::unique_ptr<IDapTransport> takeTransport() && noexcept { return std::move(m_transport); }
    [[nodiscard]] std::function<Result<void>(std::string_view)> takeWriteAdmission() && noexcept {
        return std::move(m_writeAdmission);
    }

private:
    friend class LuaDebugEndpointAccess;
    LuaDebugConnection(std::unique_ptr<IDapTransport> transport, LuaDebugIdentity identity,
                       std::function<Result<void>(std::string_view)> writeAdmission)
        : m_transport(std::move(transport)), m_identity(std::move(identity)),
          m_writeAdmission(std::move(writeAdmission)) {}
    std::unique_ptr<IDapTransport> m_transport;
    LuaDebugIdentity m_identity;
    std::function<Result<void>(std::string_view)> m_writeAdmission;
};

// Worker-only. Validates the exact identity/30s lease, performs DAP frost/authChallenge/authProve
// with fresh nonces and both role-bound HMACs, and returns a standard Content-Length DAP stream.
// Admission is required. stop must combine connection/project/trust/session revocation, and stays
// bound to the returned transport (closing it wakes blocked reads). Deadline covers all dribble
// and handshake work; successful sessions outlive the initial lease. Errors are generic and never
// echo credentials or unauthenticated payloads. Bounds: 1 MiB payload, 8 KiB header, no fallback.
[[nodiscard]] Result<LuaDebugConnection> connectLuaDebugEndpoint(LuaDebugEndpoint endpoint, LuaDebugAdmission admission,
                                                                 io::LoopbackStream::Deadline deadline,
                                                                 std::stop_token stop = {},
                                                                 LuaDebugTransportCallbacks callbacks = {});

// Retired experimental-token entry points remain source compatible and fail closed without any
// connection. Their custom protocol and supplied DAP replies are not a contract-v1 fallback.
struct LuaDebugPlayConnection {
    std::unique_ptr<io::LoopbackStream> stream;
    std::uint64_t connectionGeneration = 0, operationId = 0;
    std::int64_t nextWireRequest = 0;
};
[[nodiscard]] Result<LuaDebugPlayConnection> connectLuaDebugPlayEndpoint(std::uint16_t port, std::string_view token,
                                                                         io::LoopbackStream::Deadline deadline,
                                                                         std::stop_token stop = {});
[[nodiscard]] Result<std::unique_ptr<io::LoopbackStream>>
connectLuaDebugEndpoint(std::uint16_t port, std::string_view token, io::LoopbackStream::Deadline deadline,
                        bool armPause = false, std::stop_token stop = {},
                        std::uint64_t* connectionGeneration = nullptr);

} // namespace frost::debug
