// SPDX-License-Identifier: Apache-2.0
// FER-11 pure run configuration generation; execution remains owned by the existing RunController.
#pragma once
#include "frost/run/run_configuration.hpp"
namespace frost::run {
// Reserved generated identities: edited/stale copies must still pass the current engine admission.
[[nodiscard]] inline bool isEngineSessionToolId(std::string_view id) {
    return id.starts_with("engine.export.") || id.starts_with("engine.multiplayer.");
}
// Explicit installed editor/runtime paths and selected project, never discovered by name here.
// Produces a CLI export configuration and, when runtime is available, one owned native server,
// two native clients and their ordinary compound. No processes, files or sessions are started.
// Members use separate normal Run tabs: existing Stop and partial-start failure reporting apply.
[[nodiscard]] Result<std::vector<RunConfiguration>>
engineSessionRunConfigurations(std::string_view id, std::string_view label, std::string_view editor,
                               std::string_view runtime, std::string_view project, std::uint16_t port = 7777);
} // namespace frost::run
