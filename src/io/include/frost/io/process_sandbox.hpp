// SPDX-License-Identifier: Apache-2.0
// Declares process sandbox validation and platform launch preparation.
#pragma once
#include "frost/io/process.hpp"
#include "frost/io/path.hpp"
namespace frost::io {
struct SandboxStagingFile {
    std::string sourcePath, relativePath, contents;
    SandboxAccess access = SandboxAccess::Read;
};
// Pure admission; no environment inheritance, PTY, extra handles, raw command line or detached tree.
[[nodiscard]] Result<void> validateProcessSandbox(const ProcessSpec& spec, bool windowsRules, bool prepared = true);
[[nodiscard]] bool sandboxAllowsPath(const ProcessSandboxProfile& profile, std::string_view path,
    SandboxAccess access, PathStyle style = kNativePathStyle);
// Captured, pin-approved explicit files only. Creates a NEW private immutable staging generation;
// never changes an original file or its ACL. Returns exact mapped executable/argument paths.
// Generations are retained; collection while a process might use them is deliberately separate.
// Windows LPAC denies network by default, but has documented OS ambient system access; arbitrary
// workspace content must be brokered over the inherited stdio protocol, never granted by ACL.
[[nodiscard]] Result<ProcessSpec> stageWindowsSandboxProcess(const ProcessSpec& original,
    std::span<const SandboxStagingFile> files, std::string_view privateParent);
} // namespace frost::io
