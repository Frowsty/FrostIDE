// SPDX-License-Identifier: Apache-2.0
// Bounded FrostEngine installation and instance discovery for the app's pairing and run workflows.
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/lang/json.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::app {

struct EngineInstallation {
    std::string program;    // Absolute GUI editor executable.
    std::string cliProgram; // Absolute console CLI executable, when the platform supplies one.
    std::string root;
    std::string preset;
    std::string playerProgram; // Empty when no real sibling FrostRuntime executable exists.
};

struct EngineInstanceRecord {
    std::string instanceId;
    std::string projectRoot;
    std::uint16_t port = 0;
    lang::Json record; // Contains pairing material: never display, log or serialize into run configurations.
};

// Filesystem-only worker operations. Caller gates discovery on project trust. All input paths
// are absolute; output executable/identity paths are canonical. Directory scans are bounded and
// cancelled between entries. The filesystem must outlive the call. Errors identify unavailable
// roots or corrupt discovery; invalid individual records are ignored. No engine process starts.
[[nodiscard]] Result<std::vector<EngineInstallation>> locateEngineInstallations(io::IFileSystem& fs,
                                                                                std::string_view projectRoot,
                                                                                std::string_view configuredProgram = {},
                                                                                std::stop_token stop = {});
[[nodiscard]] Result<std::vector<EngineInstanceRecord>> listEngineInstances(io::IFileSystem& fs,
                                                                            std::string_view automationDirectory,
                                                                            std::string_view projectRoot,
                                                                            std::stop_token stop = {});

// Worker-only current physical identity check. A mismatch permanently revokes this captured
// admission token; callers create another token only after an explicit project/session reset.
[[nodiscard]] bool retainEngineProjectIdentity(io::IFileSystem& fs, std::string_view lexicalRoot,
                                               std::string_view originalKey,
                                               const std::shared_ptr<std::atomic<bool>>& admission);

} // namespace frost::app
