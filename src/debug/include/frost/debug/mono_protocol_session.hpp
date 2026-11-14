// SPDX-License-Identifier: Apache-2.0
// Owns framing and serialized operation admission for one Mono transport. Endpoint verification,
// transport lifetime and source/PDB validation remain mandatory owner responsibilities.
#pragma once
#include "frost/debug/mono_gate.hpp"
#include "frost/debug/mono_wire.hpp"

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace frost::debug::mono {
class ProtocolSession final {
public:
    ProtocolSession() = default;
    ProtocolSession(const ProtocolSession&) = delete ("a Mono connection has one owner");
    ProtocolSession& operator=(const ProtocolSession&) = delete ("a Mono connection has one owner");
    // Caller sends the fixed DWP handshake only after independently verifying the endpoint.
    // Events are returned to the owner. A malformed response permanently ends the session.
    [[nodiscard]] Result<std::vector<Packet>> feed(std::span<const std::uint8_t> bytes);
    [[nodiscard]] Result<std::vector<std::uint8_t>> version();
    [[nodiscard]] Result<std::vector<std::uint8_t>> negotiate();
    [[nodiscard]] Result<std::vector<std::uint8_t>> suspend();
    [[nodiscard]] Result<std::vector<std::uint8_t>> resume();
    [[nodiscard]] Result<std::vector<std::uint8_t>> allThreads();
    [[nodiscard]] Result<std::vector<std::uint8_t>> rootDomain();
    [[nodiscard]] Result<std::vector<std::uint8_t>> domainAssemblies(std::uint32_t domain);
    [[nodiscard]] Result<std::vector<std::uint8_t>> types(std::string_view name);
    [[nodiscard]] Result<std::vector<std::uint8_t>> metadata(MetadataQuery query, std::uint32_t id);
    [[nodiscard]] Result<std::vector<std::uint8_t>> methodFromToken(std::uint32_t assembly, std::uint32_t token);
    [[nodiscard]] Result<std::vector<std::uint8_t>> breakpoint(std::uint32_t method, std::uint64_t offset);
    [[nodiscard]] Result<std::vector<std::uint8_t>> clearBreakpoint(std::uint32_t id);
    [[nodiscard]] Result<std::vector<std::uint8_t>> frames(std::uint32_t thread);
    [[nodiscard]] Result<std::vector<std::uint8_t>> local(std::uint32_t thread, std::uint32_t frame,
                                                          std::uint32_t ordinal);
    [[nodiscard]] Result<std::vector<std::uint8_t>> step(std::uint32_t thread);
    [[nodiscard]] Result<std::vector<std::uint8_t>> clearStep();
    [[nodiscard]] Result<std::vector<std::uint8_t>> dispose();
    [[nodiscard]] const SessionState& state() const noexcept { return m_gate.state(); }
    [[nodiscard]] bool uncleanClose() const noexcept { return m_gate.uncleanClose(); }
    // Reload recovery is not implemented: invalidate and end this connection conservatively.
    void reload() noexcept;
    void close() noexcept;

private:
    using Encoder = std::function<Result<std::vector<std::uint8_t>>(std::string)>;
    [[nodiscard]] Result<std::vector<std::uint8_t>> prepare(Operation operation, const Encoder& encoder,
                                                            std::uint32_t requestId = 0);
    Wire m_wire;
    SessionGate m_gate;
    std::optional<Ticket> m_ticket;
    std::optional<Operation> m_operation;
    std::optional<Packet> m_deferredStop;
    std::optional<Packet> m_deferredNegotiation;
    bool m_initiallySuspended = false;
    bool m_initialStateKnown = false;
};
} // namespace frost::debug::mono
