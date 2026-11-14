// SPDX-License-Identifier: Apache-2.0
// Private native-host binding and bounded raw startup queue; credentials never enter UI state.
#pragma once
#include "frost/ui/debug_session_registry.hpp"

#include "engine_native_host.hpp"

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
namespace frost::app {
struct EngineRunAdmission;
struct EngineInstanceRecord;
struct EngineNativeHostState {
    static constexpr std::size_t maxOutputBytes = 1024U * 1024U;
    static constexpr std::size_t maxOutputEvents = 4096;
    EngineNativeHostToken token;
    run::RunConfiguration declared;
    std::shared_ptr<EngineRunAdmission> admission;
    std::atomic<bool> active{true}, discardRaw{false};
    std::atomic<std::int64_t> pid{0};
    QPointer<ui::DebugSessionRegistry> registry;
    ui::DebugSessionId session = 0;
    ui::DebugGroupId group = 0;
    std::uint64_t generation = 0;
    std::mutex mutex;
    std::deque<std::pair<std::string, std::string>> output;
    std::size_t bytes = 0;
    bool overflow = false;
    std::map<std::string, std::string> partial;
    std::shared_ptr<EngineInstanceRecord>
        endpoint; // Private credential record, reused only for a fresh HMAC challenge.
};
// Thread-safe private ingress shared by production and bounded-ingress source regressions.
[[nodiscard]] EngineNativePrivateOutput nativeEngineHostOutput(std::shared_ptr<EngineNativeHostState> host);
// GUI-only, checks exact registry/controller generation and launch ownership before Stop.
[[nodiscard]] Result<void> stopNativeEngineHost(const std::shared_ptr<EngineNativeHostState>& host);
[[nodiscard]] bool nativeEngineHostCurrent(const std::shared_ptr<EngineNativeHostState>& host);
} // namespace frost::app
