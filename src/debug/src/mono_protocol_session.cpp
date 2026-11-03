// SPDX-License-Identifier: Apache-2.0
// Correlate Mono wire packets with session operations and events.
#include "frost/debug/mono_protocol_session.hpp"

#include "frost/debug/mono_responses.hpp"

#include <utility>

namespace frost::debug::mono {
namespace {
std::uint32_t read32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
} // namespace
Result<std::vector<Packet>> ProtocolSession::feed(std::span<const std::uint8_t> bytes) {
    const bool hadHandshake = m_wire.ready();
    auto packets = m_wire.feed(bytes);
    if (!packets) {
        close();
        return std::unexpected(packets.error());
    }
    if (!hadHandshake && m_wire.ready()) {
        auto result = m_gate.handshakeComplete();
        if (!result) {
            close();
            return std::unexpected(result.error());
        }
    }
    std::vector<Packet> delivered;
    for (const auto& packet : *packets) {
        if (!packet.reply) {
            if (packet.payload.size() < 6) {
                close();
                return fail(ErrorCode::Corrupt, "invalid Mono event");
            }
            const auto kind = packet.payload[5];
            if (kind == 10 || kind == 11) {
                auto stop = decodeStop(packet.payload);
                if (!stop || (kind == 10 && !m_gate.ownsBreakpoint(stop->request)) ||
                    (kind == 11 && m_gate.activeStep() != stop->request)) {
                    close();
                    return fail(ErrorCode::Corrupt, "unowned Mono stop event");
                }
                if (m_operation && *m_operation == Operation::Resume) {
                    if (m_deferredStop) {
                        close();
                        return fail(ErrorCode::Corrupt, "multiple Mono stops before resume acknowledgement");
                    }
                    m_deferredStop = packet;
                    continue;
                }
                m_gate.stopped();
                if (m_gate.state().phase() == Phase::Ended) {
                    close();
                    return fail(ErrorCode::Corrupt, "unexpected Mono stop state");
                }
            } else if (kind == 0 &&
                       (m_gate.state().phase() == Phase::Version || m_gate.state().phase() == Phase::Negotiating)) {
                const auto& e = packet.payload;
                if (m_initialStateKnown || e.size() != 18 || (e[0] != 0 && e[0] != 2) || read32(e.data() + 1) != 1 ||
                    read32(e.data() + 6) != 0 || read32(e.data() + 10) == 0 || read32(e.data() + 14) == 0) {
                    close();
                    return fail(ErrorCode::Corrupt, "invalid Mono startup event");
                }
                m_initiallySuspended = e[0] == 2;
                m_initialStateKnown = true;
                if (m_deferredNegotiation) {
                    if (!m_ticket || !m_operation || *m_operation != Operation::Negotiate) {
                        close();
                        return fail(ErrorCode::Corrupt, "unexpected deferred Mono negotiation");
                    }
                    const auto ticket = *m_ticket;
                    m_ticket.reset();
                    m_operation.reset();
                    auto completed = m_gate.complete(ticket, true, m_initiallySuspended);
                    if (!completed) {
                        close();
                        return std::unexpected(completed.error());
                    }
                    delivered.push_back(std::move(*m_deferredNegotiation));
                    m_deferredNegotiation.reset();
                }
            } else {
                close();
                return fail(ErrorCode::Unsupported, "unsupported Mono event");
            }
            delivered.push_back(packet);
            continue;
        }
        if (!m_ticket || !m_operation || packet.label != std::to_string(m_ticket->serial)) {
            close();
            return fail(ErrorCode::Corrupt, "unexpected Mono session reply");
        }
        std::uint32_t major = 0, minor = 0, eventRequest = 0;
        const auto& p = packet.payload;
        if (packet.error == 0 && *m_operation == Operation::Version) {
            if (p.size() < 12 || read32(p.data()) > 4096 || read32(p.data()) != p.size() - 12) {
                close();
                return fail(ErrorCode::Corrupt, "invalid Mono version reply");
            }
            major = read32(p.data() + p.size() - 8);
            minor = read32(p.data() + p.size() - 4);
        }
        if (packet.error == 0 && (*m_operation == Operation::Step || *m_operation == Operation::Breakpoint)) {
            if (p.size() != 4) {
                close();
                return fail(ErrorCode::Corrupt, "invalid Mono event request reply");
            }
            eventRequest = read32(p.data());
        }
        if (packet.error == 0 &&
            (*m_operation == Operation::Negotiate || *m_operation == Operation::Suspend ||
             *m_operation == Operation::Resume || *m_operation == Operation::ClearBreakpoint ||
             *m_operation == Operation::ClearStep || *m_operation == Operation::Dispose) &&
            !p.empty()) {
            close();
            return fail(ErrorCode::Corrupt, "unexpected Mono acknowledgement data");
        }
        if (packet.error == 0 && *m_operation == Operation::Negotiate && !m_initialStateKnown) {
            if (m_deferredNegotiation) {
                close();
                return fail(ErrorCode::Corrupt, "duplicate Mono negotiation acknowledgement");
            }
            m_deferredNegotiation = packet;
            continue;
        }
        const auto ticket = *m_ticket;
        const auto operation = *m_operation;
        m_ticket.reset();
        m_operation.reset();
        auto completed = m_gate.complete(ticket, packet.error == 0, m_initiallySuspended, major, minor, eventRequest);
        if (!completed) {
            close();
            return std::unexpected(completed.error());
        }
        delivered.push_back(packet);
        if (operation == Operation::Resume && m_deferredStop) {
            m_gate.stopped();
            if (m_gate.state().phase() == Phase::Ended) {
                close();
                return fail(ErrorCode::Corrupt, "unexpected deferred Mono stop state");
            }
            delivered.push_back(std::move(*m_deferredStop));
            m_deferredStop.reset();
        }
    }
    return delivered;
}
Result<std::vector<std::uint8_t>> ProtocolSession::prepare(Operation op, Encoder encoder, std::uint32_t requestId) {
    auto ticket = m_gate.begin(op, requestId);
    if (!ticket)
        return std::unexpected(ticket.error());
    auto bytes = encoder(std::to_string(ticket->serial));
    if (!bytes) {
        close();
        return std::unexpected(bytes.error());
    }
    m_ticket = *ticket;
    m_operation = op;
    return bytes;
}
Result<std::vector<std::uint8_t>> ProtocolSession::version() {
    return prepare(Operation::Version, [this](std::string l) { return m_wire.version(std::move(l)); });
}
Result<std::vector<std::uint8_t>> ProtocolSession::negotiate() {
    return prepare(Operation::Negotiate, [this](std::string l) { return m_wire.negotiate258(std::move(l)); });
}
Result<std::vector<std::uint8_t>> ProtocolSession::suspend() {
    return prepare(Operation::Suspend, [this](std::string l) { return m_wire.suspend(std::move(l)); });
}
Result<std::vector<std::uint8_t>> ProtocolSession::resume() {
    return prepare(Operation::Resume, [this](std::string l) { return m_wire.resume(std::move(l)); });
}
Result<std::vector<std::uint8_t>> ProtocolSession::allThreads() {
    return prepare(Operation::Metadata, [this](std::string l) { return m_wire.allThreads(std::move(l)); });
}
Result<std::vector<std::uint8_t>> ProtocolSession::rootDomain() {
    return prepare(Operation::Metadata, [this](std::string l) { return m_wire.rootDomain(std::move(l)); });
}
Result<std::vector<std::uint8_t>> ProtocolSession::domainAssemblies(std::uint32_t domain) {
    return prepare(Operation::Metadata,
                   [this, domain](std::string l) { return m_wire.domainAssemblies(domain, std::move(l)); });
}
Result<std::vector<std::uint8_t>> ProtocolSession::types(std::string_view name) {
    return prepare(Operation::Metadata, [this, name](std::string l) { return m_wire.types(name, std::move(l)); });
}
Result<std::vector<std::uint8_t>> ProtocolSession::metadata(MetadataQuery q, std::uint32_t id) {
    return prepare(Operation::Metadata, [this, q, id](std::string l) { return m_wire.metadata(q, id, std::move(l)); });
}
Result<std::vector<std::uint8_t>> ProtocolSession::methodFromToken(std::uint32_t assembly, std::uint32_t token) {
    return prepare(Operation::Metadata, [this, assembly, token](std::string l) {
        return m_wire.methodFromToken(assembly, token, std::move(l));
    });
}
Result<std::vector<std::uint8_t>> ProtocolSession::breakpoint(std::uint32_t method, std::uint64_t offset) {
    return prepare(Operation::Breakpoint,
                   [this, method, offset](std::string l) { return m_wire.breakpoint(method, offset, std::move(l)); });
}
Result<std::vector<std::uint8_t>> ProtocolSession::clearBreakpoint(std::uint32_t id) {
    return prepare(
        Operation::ClearBreakpoint, [this, id](std::string l) { return m_wire.clearEvent(10, id, std::move(l)); }, id);
}
Result<std::vector<std::uint8_t>> ProtocolSession::frames(std::uint32_t thread) {
    return prepare(Operation::Frames, [this, thread](std::string l) { return m_wire.frames(thread, std::move(l)); });
}
Result<std::vector<std::uint8_t>> ProtocolSession::local(std::uint32_t thread, std::uint32_t frame,
                                                         std::uint32_t ordinal) {
    return prepare(Operation::Local, [this, thread, frame, ordinal](std::string l) {
        return m_wire.local(thread, frame, ordinal, std::move(l));
    });
}
Result<std::vector<std::uint8_t>> ProtocolSession::step(std::uint32_t thread) {
    return prepare(Operation::Step, [this, thread](std::string l) { return m_wire.stepOver(thread, std::move(l)); });
}
Result<std::vector<std::uint8_t>> ProtocolSession::clearStep() {
    const auto id = m_gate.activeStep();
    if (!id)
        return fail(ErrorCode::Busy, "no Mono step request");
    return prepare(Operation::ClearStep,
                   [this, id](std::string l) { return m_wire.clearEvent(11, *id, std::move(l)); });
}
Result<std::vector<std::uint8_t>> ProtocolSession::dispose() {
    return prepare(Operation::Dispose, [this](std::string l) { return m_wire.dispose(std::move(l)); });
}
void ProtocolSession::reload() noexcept { close(); }
void ProtocolSession::close() noexcept {
    m_deferredStop.reset();
    m_deferredNegotiation.reset();
    m_ticket.reset();
    m_operation.reset();
    m_gate.close();
    m_wire.close();
}
} // namespace frost::debug::mono
