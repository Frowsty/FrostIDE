// SPDX-License-Identifier: Apache-2.0
// Exact native DAP evidence lease for R6 heartbeat expiry only; no synthetic heartbeat or process ownership.
#pragma once
#include "frost/core/error.hpp"

#include <cstdint>
#include <memory>
#include <utility>

namespace frost::engine {
struct NativeDebuggerBinding {
    std::uint64_t registrySessionId = 0, controllerGeneration = 0;
    std::int64_t processId = 0;
    friend bool operator==(const NativeDebuggerBinding&, const NativeDebuggerBinding&) = default;
};
enum class NativeAdapterEvent : std::uint8_t { Stopped, Continued, Ended };
struct NativeAdapterObservation {
    NativeDebuggerBinding binding;
    // Monotonic actual adapter-event sequence within this controller generation, never a UI revision.
    std::uint64_t eventSequence = 0;
    NativeAdapterEvent event = NativeAdapterEvent::Ended;
};
class NativePauseLease {
public:
    NativePauseLease(const NativePauseLease&) = delete;
    NativePauseLease& operator=(const NativePauseLease&) = delete;
    NativePauseLease(NativePauseLease&&) noexcept = default;
    NativePauseLease& operator=(NativePauseLease&&) noexcept;
    ~NativePauseLease();
    // GUI publisher only supplies actual tagged native DAP stopped/continued/end evidence. Wrong
    // bindings refuse; stale/out-of-order events do nothing. End/revoke never grants resume grace.
    [[nodiscard]] Result<void> observe(const NativeAdapterObservation& observation);
    void revoke() noexcept;
    struct State;

private:
    friend class EngineSession;
    explicit NativePauseLease(std::shared_ptr<State> state) : m_state(std::move(state)) {}
    std::shared_ptr<State> m_state;
};
} // namespace frost::engine
