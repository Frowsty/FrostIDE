// SPDX-License-Identifier: Apache-2.0
// Explicit lifetime barrier for attached DAP transport retirement after GUI shutdown.
#pragma once
#include "frost/debug/debug_session.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
namespace frost::ui {
// Shared by a workspace's controllers. retire() queues bounded detach work and returns promptly;
// workers own the entire session/transport until actual ACK or uncertain cleanup, never a host kill.
// The application MUST drain after window/event-loop teardown and before borrowed launcher, trust,
// filesystem, admission services or logging die. Revocation alone is not a lifetime barrier.
// drain()/destruction may wait and must not run from a GUI shutdown handler or retirement callback.
// No new retirements may be submitted after the caller begins its final drain. Thread-safe.
class DebugRetirementBatch final {
public:
    DebugRetirementBatch();
    ~DebugRetirementBatch();
    DebugRetirementBatch(const DebugRetirementBatch&) = delete;
    DebugRetirementBatch& operator=(const DebugRetirementBatch&) = delete;
    void retire(std::unique_ptr<debug::DebugSession> session, std::chrono::milliseconds grace);
    [[nodiscard]] std::size_t pending() const noexcept;
    void drain();

private:
    struct State;
    std::shared_ptr<State> m_state;
};
// Aggregate resource count for acceptance observations; it is NOT a borrowed-service lifetime
// barrier. The explicitly scoped batch's drain() is the required barrier.
[[nodiscard]] std::size_t pendingDebugDetachments() noexcept;
} // namespace frost::ui
