// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/run/run_configuration.hpp"
namespace frost::run {
struct UnrealEditorRunRequest {
    std::string id;
    std::string name;
    std::string editor; // Explicit user-selected installed executable, never inferred from target.
    std::string projectFile;
    std::vector<std::string> arguments; // User's editor arguments after the project argument.
};
// Produces an ordinary native configuration supported by existing Run/Debug. No auto-build,
// launch, plugin pairing, license acceptance or ownership of an already-running Editor.
[[nodiscard]] Result<RunConfiguration> unrealEditorRunConfiguration(const UnrealEditorRunRequest& request);
} // namespace frost::run
