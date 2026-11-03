// SPDX-License-Identifier: Apache-2.0
// Gate Mono protocol operations against session state and owned requests.
#include "frost/debug/mono_gate.hpp"

#include <limits>

namespace frost::debug::mono {
Result<Ticket> SessionGate::begin(Operation op, std::uint32_t requestId) {
    if (m_pending)
        return fail(ErrorCode::Busy, "Mono request already pending");
    if (m_unknownRequest)
        return fail(ErrorCode::Busy, "Mono event request outcome unknown");
    if (op != Operation::ClearBreakpoint && requestId != 0)
        return fail(ErrorCode::InvalidArgument, "unexpected Mono event request ID");
    const auto phase = m_state.phase();
    bool allowed = false;
    switch (op) {
    case Operation::Version: allowed = phase == Phase::Version; break;
    case Operation::Negotiate: allowed = phase == Phase::Negotiating; break;
    case Operation::Suspend: allowed = phase == Phase::Running; break;
    case Operation::Resume: allowed = phase == Phase::Suspended; break;
    case Operation::Metadata:
    case Operation::Breakpoint:
    case Operation::Frames:
    case Operation::Local: allowed = phase == Phase::Suspended; break;
    case Operation::ClearBreakpoint:
        allowed = (phase == Phase::Suspended || phase == Phase::Reloading) && m_breakpoints.contains(requestId);
        break;
    case Operation::Step: allowed = phase == Phase::Suspended && !m_step; break;
    case Operation::ClearStep:
        allowed = (phase == Phase::Suspended || phase == Phase::Reloading) && m_step.has_value();
        break;
    case Operation::Dispose:
        allowed = (phase == Phase::Suspended || phase == Phase::Running) && !m_step && m_breakpoints.empty();
        break;
    }
    if (!allowed)
        return fail(ErrorCode::Busy, "Mono operation not admitted in current state");
    if (m_serial == std::numeric_limits<std::uint64_t>::max()) {
        close();
        return fail(ErrorCode::OutOfRange, "Mono request sequence exhausted");
    }
    // Invalidate stopped-frame identities as soon as resume is issued, not on its reply.
    if (op == Operation::Resume) {
        auto result = m_state.resumed();
        if (!result)
            return std::unexpected(result.error());
    }
    Ticket ticket{m_serial++, m_state.generation()};
    m_pending = Pending{op, requestId, ticket};
    return ticket;
}
Result<void> SessionGate::complete(Ticket ticket, bool success, bool initiallySuspended, std::uint32_t major,
                                   std::uint32_t minor, std::uint32_t eventRequest) {
    if (!m_pending || m_pending->ticket.serial != ticket.serial || m_pending->ticket.generation != ticket.generation ||
        !m_state.current(ticket.generation))
        return fail(ErrorCode::Cancelled, "stale Mono completion");
    const auto op = m_pending->operation;
    const auto requestId = m_pending->requestId;
    m_pending.reset();
    if (!success) {
        if (op == Operation::Step || op == Operation::Breakpoint)
            m_unknownRequest = true;
        close();
        return fail(ErrorCode::IoError, "Mono request failed");
    }
    switch (op) {
    case Operation::Version: {
        auto result = m_state.versionReply(major, minor);
        if (!result)
            close();
        return result;
    }
    case Operation::Negotiate: return m_state.negotiated(initiallySuspended);
    case Operation::Suspend: return m_state.stopped();
    case Operation::Resume: break;
    case Operation::Breakpoint:
        if (!eventRequest || m_breakpoints.size() >= 4096 || m_breakpoints.contains(eventRequest)) {
            close();
            return fail(ErrorCode::Corrupt, "invalid Mono breakpoint request");
        }
        m_breakpoints.insert(eventRequest);
        break;
    case Operation::ClearBreakpoint: m_breakpoints.erase(requestId); break;
    case Operation::Step:
        if (!eventRequest) {
            close();
            return fail(ErrorCode::Corrupt, "null Mono step request");
        }
        m_step = eventRequest;
        break;
    case Operation::ClearStep: m_step.reset(); break;
    case Operation::Dispose: m_state.ended(); break;
    default: break;
    }
    return {};
}
void SessionGate::stopped() {
    if (m_pending && (m_pending->operation == Operation::Step || m_pending->operation == Operation::Breakpoint))
        m_unknownRequest = true;
    m_pending.reset();
    if (m_state.phase() == Phase::Running)
        (void)m_state.stopped();
    else
        close();
}
void SessionGate::reload() noexcept {
    if (m_pending && (m_pending->operation == Operation::Step || m_pending->operation == Operation::Breakpoint))
        m_unknownRequest = true;
    m_pending.reset();
    m_state.reload();
    if (m_state.phase() == Phase::Ended)
        m_unclean = true;
}
void SessionGate::close() noexcept {
    if (m_state.phase() != Phase::Ended || m_unknownRequest || m_step || !m_breakpoints.empty() ||
        (m_pending && (m_pending->operation == Operation::Step || m_pending->operation == Operation::Breakpoint)))
        m_unclean = true;
    m_pending.reset();
    m_step.reset();
    m_breakpoints.clear();
    m_state.ended();
}
} // namespace frost::debug::mono
