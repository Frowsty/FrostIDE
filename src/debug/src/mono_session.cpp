// SPDX-License-Identifier: Apache-2.0
// Track Mono debugging phases and invalidate stale generations.
#include "frost/debug/mono_session.hpp"

#include <limits>

namespace frost::debug::mono {
Result<void> SessionState::transition(Phase expected, Phase next) {
    if (m_phase != expected)
        return fail(ErrorCode::Busy, "invalid Mono session transition");
    m_phase = next;
    return {};
}
Result<void> SessionState::handshakeComplete() { return transition(Phase::Handshake, Phase::Version); }
Result<void> SessionState::versionReply(std::uint32_t major, std::uint32_t minor) {
    if (m_phase != Phase::Version)
        return fail(ErrorCode::Busy, "unexpected Mono version reply");
    if (major != 2 || minor < 58)
        return fail(ErrorCode::Unsupported, "Mono protocol 2.58 unavailable");
    m_phase = Phase::Negotiating;
    return {};
}
Result<void> SessionState::negotiated(bool suspended) {
    return transition(Phase::Negotiating, suspended ? Phase::Suspended : Phase::Running);
}
Result<void> SessionState::resumed() {
    if (m_phase != Phase::Suspended)
        return fail(ErrorCode::Busy, "invalid Mono resume");
    invalidate();
    if (m_phase == Phase::Ended)
        return fail(ErrorCode::OutOfRange, "Mono generation exhausted");
    m_phase = Phase::Running;
    return {};
}
Result<void> SessionState::stopped() {
    if (m_phase != Phase::Running)
        return fail(ErrorCode::Busy, "invalid Mono stop");
    invalidate();
    if (m_phase == Phase::Ended)
        return fail(ErrorCode::OutOfRange, "Mono generation exhausted");
    m_phase = Phase::Suspended;
    return {};
}
void SessionState::invalidate() noexcept {
    // Exhaustion ends the session instead of making an old generation current again.
    if (m_generation == std::numeric_limits<std::uint64_t>::max())
        m_phase = Phase::Ended;
    else
        ++m_generation;
}
void SessionState::reload() noexcept {
    if (m_phase == Phase::Closing || m_phase == Phase::Ended)
        return;
    if (m_phase != Phase::Running && m_phase != Phase::Suspended && m_phase != Phase::Reloading) {
        ended();
        return;
    }
    invalidate();
    if (m_phase != Phase::Ended)
        m_phase = Phase::Reloading;
}
Result<void> SessionState::reloaded(bool suspended) {
    return transition(Phase::Reloading, suspended ? Phase::Suspended : Phase::Running);
}
void SessionState::closing() noexcept {
    if (m_phase == Phase::Ended || m_phase == Phase::Closing)
        return;
    invalidate();
    if (m_phase != Phase::Ended)
        m_phase = Phase::Closing;
}
void SessionState::ended() noexcept {
    if (m_phase != Phase::Ended)
        invalidate();
    m_phase = Phase::Ended;
}
} // namespace frost::debug::mono
