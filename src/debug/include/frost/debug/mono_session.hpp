// SPDX-License-Identifier: Apache-2.0
// Session lifetime and generation state; transport and Editor identity are verified by the owner.
#pragma once
#include "frost/core/error.hpp"

#include <cstdint>

namespace frost::debug::mono {

enum class Phase { Handshake, Version, Negotiating, Suspended, Running, Reloading, Closing, Ended };

// Single owner. Async completions carry generation() and are ignored when it changes. This is not
// endpoint authentication: the caller must verify the selected Editor before sending any bytes.
class SessionState final {
public:
    SessionState() = default;
    SessionState(const SessionState&) = delete ("a Mono session has one owner");
    SessionState& operator=(const SessionState&) = delete ("a Mono session has one owner");
    SessionState(SessionState&&) = delete ("a Mono session has one owner");
    SessionState& operator=(SessionState&&) = delete ("a Mono session has one owner");
    [[nodiscard]] Phase phase() const noexcept { return m_phase; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }
    [[nodiscard]] bool current(std::uint64_t generation) const noexcept {
        return m_phase != Phase::Ended && generation == m_generation;
    }
    [[nodiscard]] Result<void> handshakeComplete();
    [[nodiscard]] Result<void> versionReply(std::uint32_t major, std::uint32_t minor);
    // Called only after the protocol negotiation reply and the initial runtime state are known.
    [[nodiscard]] Result<void> negotiated(bool suspended);
    [[nodiscard]] Result<void> resumed();
    [[nodiscard]] Result<void> stopped();
    // Invalidates frames, method IDs and source/breakpoint bindings; callers must re-resolve them.
    void reload() noexcept;
    [[nodiscard]] Result<void> reloaded(bool suspended);
    void closing() noexcept;
    void ended() noexcept;

private:
    [[nodiscard]] Result<void> transition(Phase expected, Phase next);
    void invalidate() noexcept;
    Phase m_phase = Phase::Handshake;
    std::uint64_t m_generation = 1;
};

} // namespace frost::debug::mono
