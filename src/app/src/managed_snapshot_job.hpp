// SPDX-License-Identifier: Apache-2.0
// Shared GUI-lifetime/cancellation gate for verified managed source preparation and Unity Mono admission.
#pragma once
#include "frost/core/mutex.hpp"

#include <atomic>
namespace frost::app {
class Workbench;
struct ManagedSnapshotJob {
    Mutex mutex;
    Workbench* target FROST_GUARDED_BY(mutex) = nullptr;
    std::atomic_bool cancelled{false};
};
} // namespace frost::app
