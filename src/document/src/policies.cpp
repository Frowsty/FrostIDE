// SPDX-License-Identifier: Apache-2.0
//
// SavePolicies::fromSettings: the save.* and localHistory.* keys (ADR 0021) as SaveManager policies.

#include "frost/document/policies.hpp"

#include "frost/settings/keys.hpp"

#include <chrono>
#include <cstdint>

namespace frost::document {

SavePolicies SavePolicies::fromSettings(const settings::SettingsSnapshot& snapshot) {
    namespace key = settings::key;
    SavePolicies policies;
    policies.autosave.onFrameDeactivation = snapshot.get(key::saveAutosaveOnFrameDeactivation);
    policies.autosave.onIdle = snapshot.get(key::saveAutosaveOnIdle);
    policies.autosave.idleDelay = std::chrono::seconds(snapshot.get(key::saveAutosaveIdleSeconds));
    policies.hotExit = snapshot.get(key::saveHotExit);
    policies.localHistory = snapshot.get(key::localHistoryEnabled);
    // The registry's schemas bound both numbers (1..3650 days, 16..1048576 MiB), so neither is 0.
    policies.retention =
        storage::RetentionPolicy(std::chrono::days(snapshot.get(key::localHistoryMaxAgeDays)),
                                 static_cast<std::uint64_t>(snapshot.get(key::localHistoryMaxTotalMiB)) << 20,
                                 snapshot.get(key::localHistoryKeepNewestPerPath));
    return policies;
}

} // namespace frost::document
