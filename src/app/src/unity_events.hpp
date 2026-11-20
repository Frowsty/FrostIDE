// SPDX-License-Identifier: Apache-2.0
// Strict decoding of authenticated per-domain Unity event journal continuations.
#pragma once
#include "frost/lang/json.hpp"

#include <vector>
namespace frost::app {
struct UnityBridgeEvent {
    std::int64_t sequence = 0;
    std::string kind, level, message, stack, file, assembly;
    int line = 0, column = 0;
    bool truncated = false;
};
struct UnityEventPage {
    std::int64_t first = 1, next = 0, last = 0, lost = 0, projectVersion = 0;
    bool compiling = false;
    std::vector<UnityBridgeEvent> events;
};
// At most eight entries; verifies sequence continuity, loss, limits and exact continuation semantics.
// Pure O(response size), no process/file access; malformed pages never partially publish.
[[nodiscard]] Result<UnityEventPage> parseUnityEvents(const lang::Json& response, std::int64_t cursor);
} // namespace frost::app
