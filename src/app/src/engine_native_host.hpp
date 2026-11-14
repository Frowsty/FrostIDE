// SPDX-License-Identifier: Apache-2.0
// Separate native-DAP host reservation: never a RunManager RunId or external process adoption.
#pragma once
#include "frost/run/run_manager.hpp"

#include <cstdint>
#include <functional>
#include <stop_token>
namespace frost::app {
struct EngineNativeHostToken {
    std::uint64_t value = 0;
    friend bool operator==(const EngineNativeHostToken&, const EngineNativeHostToken&) = default;
};
// Sensitive transient launch material. Only the private built-in native path may consume it; never
// serialize/show/trace this configuration or plan. Admission/resolution closures run on workers.
struct EngineNativeHostLaunch {
    EngineNativeHostToken token;
    run::RunConfiguration configuration;
    std::string trustRoot;
    std::function<Result<void>()> admission;
    std::function<Result<run::LaunchPlan>(run::LaunchPlan)> resolve;
    std::stop_token projectStop;
};
struct EngineNativeHostObservation {
    std::int64_t processId = 0;
    bool configured = false, nativeStopped = false;
};
using EngineNativePrivateOutput = std::function<void(std::string_view category, std::string_view bytes)>;
} // namespace frost::app
