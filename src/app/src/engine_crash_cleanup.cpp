// SPDX-License-Identifier: Apache-2.0
// Exact authenticated crash observation survives automatic process cleanup, never planned Stop.
#include "engine_crash_observer.hpp"
namespace frost::app {
bool preserveEngineCrashObservation(const std::shared_ptr<const EngineCrashSessionLease>& lease,
                                    std::int64_t expectedPid, bool stopped, std::optional<int> exitCode) noexcept {
    return lease && lease->admitted() && expectedPid > 0 && lease->identity().processId == expectedPid &&
           (!lease->live() || (!stopped && exitCode.has_value()));
}
} // namespace frost::app
