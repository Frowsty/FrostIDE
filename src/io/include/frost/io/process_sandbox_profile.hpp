// SPDX-License-Identifier: Apache-2.0
// Defines the filesystem access rights in a process sandbox profile.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace frost::io {
enum class SandboxAccess : std::uint8_t { Read = 1, Write = 2, Execute = 4 };
constexpr SandboxAccess operator|(SandboxAccess a, SandboxAccess b) noexcept {
    return static_cast<SandboxAccess>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}
constexpr bool hasSandboxAccess(SandboxAccess value, SandboxAccess bit) noexcept {
    return (static_cast<unsigned>(value) & static_cast<unsigned>(bit)) == static_cast<unsigned>(bit);
}
struct ProcessSandboxPath {
    std::string path;
    SandboxAccess access = SandboxAccess::Read;
    bool recursive = false; // Directory subtree; false names exactly one file/directory.
};
enum class SandboxNetwork : std::uint8_t { Deny, InternetClient };
struct ProcessSandboxProfile {
    std::vector<ProcessSandboxPath> paths;
    SandboxNetwork network = SandboxNetwork::Deny;
    // Owned Windows staging only, populated by stageWindowsSandboxProcess. Never descriptor input.
    std::string windowsStagingRoot;
    std::string windowsAppContainerName;
};
} // namespace frost::io
