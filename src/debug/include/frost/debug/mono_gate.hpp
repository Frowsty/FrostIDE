// SPDX-License-Identifier: Apache-2.0
// Admission and completion rules for one Mono 2.58 connection. No transport or Editor access.
#pragma once
#include "frost/core/error.hpp"
#include "frost/debug/mono_session.hpp"

#include <cstdint>
#include <optional>
#include <unordered_set>

namespace frost::debug::mono {
enum class Operation {
    Version,
    Negotiate,
    Suspend,
    Resume,
    Metadata,
    Breakpoint,
    ClearBreakpoint,
    Frames,
    Local,
    Step,
    ClearStep,
    Dispose
};
struct Ticket {
    std::uint64_t serial = 0;
    std::uint64_t generation = 0;
};

// A serialized session owner must invoke these gates around actual wire requests. Source identity,
// endpoint ownership and exact event/frame binding remain separate prerequisites before integration.
// An ended gate is not evidence that an Editor detached or resumed safely.
class SessionGate final {
public:
    SessionGate() = default;
    SessionGate(const SessionGate&) = delete ("a Mono connection has one owner");
    SessionGate& operator=(const SessionGate&) = delete ("a Mono connection has one owner");
    [[nodiscard]] const SessionState& state() const noexcept { return m_state; }
    [[nodiscard]] Result<void> handshakeComplete() { return m_state.handshakeComplete(); }
    [[nodiscard]] Result<Ticket> begin(Operation operation, std::uint32_t requestId = 0);
    [[nodiscard]] Result<void> complete(Ticket ticket, bool success, bool initiallySuspended = false,
                                        std::uint32_t major = 0, std::uint32_t minor = 0,
                                        std::uint32_t eventRequest = 0);
    void stopped();
    void reload() noexcept;
    [[nodiscard]] Result<void> reloaded(bool suspended) { return m_state.reloaded(suspended); }
    void close() noexcept;
    [[nodiscard]] std::optional<std::uint32_t> activeStep() const noexcept { return m_step; }
    [[nodiscard]] bool ownsBreakpoint(std::uint32_t id) const noexcept { return m_breakpoints.contains(id); }
    [[nodiscard]] bool uncleanClose() const noexcept { return m_unclean; }

private:
    struct Pending {
        Operation operation;
        std::uint32_t requestId;
        Ticket ticket;
    };
    SessionState m_state;
    std::optional<Pending> m_pending;
    std::optional<std::uint32_t> m_step;
    std::unordered_set<std::uint32_t> m_breakpoints;
    bool m_unknownRequest = false;
    bool m_unclean = false;
    std::uint64_t m_serial = 1;
};
} // namespace frost::debug::mono
