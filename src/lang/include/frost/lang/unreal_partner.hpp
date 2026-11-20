// SPDX-License-Identifier: Apache-2.0
// IDE-owned wire contract; no Unreal plug-in code/dependency is part of this library.
#pragma once
#include "frost/lang/json.hpp"
namespace frost::lang {
struct UnrealBlueprintCall {
    std::string blueprint, function, node;
};
struct UnrealPartnerEvent {
    std::string kind, status, message, scope;
    std::uint64_t id = 0, droppedLogs = 0;
    bool playing = false, paused = false;
    std::vector<UnrealBlueprintCall> calls;
};
// Literal allowlisted operations only. Token is never persisted/logged; no shell/console route.
[[nodiscard]] Result<std::string> unrealPartnerRequest(std::uint64_t id, std::string_view token,
                                                       std::string_view projectFile, std::string_view operation);
// Strict bounded UTF-8/duplicate-free JSON and exact project identity validation. Calls are actual
// reported loaded graph nodes, not a whole-project Blueprint index or inferred native USRs.
[[nodiscard]] Result<UnrealPartnerEvent> parseUnrealPartnerEvent(std::string_view frame, std::string_view projectFile);
} // namespace frost::lang
