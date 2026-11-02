// SPDX-License-Identifier: Apache-2.0
//
// The SaveManager's policies (ADR 0028) and how they come from frost::settings' save.* and
// localHistory.* keys (ADR 0021): autosave on frame deactivation and on idle, hot exit, and
// local-history recording with its retention limits.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/settings/store.hpp"
#include "frost/storage/local_history.hpp"

#include <chrono>
#include <cstdint>

namespace frost::document {

// When the editor saves on its own. A plain value.
struct AutosavePolicy {
    bool onFrameDeactivation = true;                       // save.autosave.onFrameDeactivation
    bool onIdle = true;                                    // save.autosave.onIdle
    IClock::Duration idleDelay = std::chrono::seconds(15); // save.autosave.idleSeconds

    friend bool operator==(const AutosavePolicy&, const AutosavePolicy&) = default;
};

/*
 * Everything the SaveManager reads from settings. The defaults equal the built-in registry's
 * defaults (DocumentPolicies.DefaultsMatchTheRegistry checks it); production code takes
 * fromSettings() and re-applies it when the settings store publishes a change. A plain value.
 */
struct SavePolicies {
    AutosavePolicy autosave{};
    bool hotExit = true;      // save.hotExit: journal unsaved buffers for hot exit and crash recovery.
    bool localHistory = true; // localHistory.enabled: record a revision at every save.
    // localHistory.maxAgeDays, localHistory.maxTotalMiB, localHistory.keepNewestPerPath.
    storage::RetentionPolicy retention{std::chrono::days(5), std::uint64_t{1024} << 20, true};

    // The policies the effective settings ask for. Precondition: `snapshot` comes from a store over
    // the built-in registry (settings::builtinRegistry()), which has every key read here. O(keys).
    [[nodiscard]] static SavePolicies fromSettings(const settings::SettingsSnapshot& snapshot);
};

} // namespace frost::document
