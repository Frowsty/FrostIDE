// SPDX-License-Identifier: Apache-2.0
//
// Finding git (M5, ADR 0048): the tools.git.path setting, then Git for Windows' install
// directories, then the directories of PATH, searched here (never by the OS), so the result is an
// absolute path IProcessLauncher can start (PLAN.md section 3.5).
/*
 * Order (findGit):
 *   1. GitSearch::configuredPath (tools.git.path). When set it must name an existing absolute
 *      file; there is no fallback, so a typo is reported rather than papered over.
 *   2. GitSearch::installDirectories: on Windows <ProgramW6432 / ProgramFiles / ProgramFiles(x86)
 *      / LOCALAPPDATA\Programs>\Git\cmd (Git for Windows' launcher, which sets up its own runtime);
 *      on Linux /usr/bin and /usr/local/bin.
 *   3. GitSearch::pathVariable: each absolute entry of PATH, in order. Relative entries (".")
 *      are skipped, so a planted git in the current or project directory is never found.
 */
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::settings {
class SettingsSnapshot;
} // namespace frost::settings

namespace frost::vcs {

enum class GitOrigin : std::uint8_t { Setting, InstallDirectory, Path };

// "setting", "install directory" or "PATH".
[[nodiscard]] std::string_view toString(GitOrigin origin) noexcept;

struct GitLocation {
    std::string path; // Absolute and normalised.
    GitOrigin origin = GitOrigin::Path;
};

// Where findGit looks (header comment). Plain value; tests fill it by hand.
struct GitSearch {
    std::string configuredPath;
    std::vector<std::string> installDirectories;
    std::string pathVariable;
    // Automatic candidates within these project roots are never executable tools.
    std::vector<std::string> excludedDirectories;
};

// The tools.git.path value of `settings` ("" when unset).
[[nodiscard]] std::string configuredGitPath(const settings::SettingsSnapshot& settings);

// The search for this machine: `configuredPath`, the platform's install directories (read from
// the environment with the wide APIs on Windows) and the PATH variable.
[[nodiscard]] GitSearch defaultGitSearch(std::string configuredPath = {});

/*
 * The git executable `search` finds (header comment). Only checks that the file exists; the
 * runner's first command proves that it runs. Errors: InvalidArgument (configuredPath is relative),
 * NotFound (configuredPath does not exist, or nothing was found; the message lists what was tried).
 */
[[nodiscard]] Result<GitLocation> findGit(io::IFileSystem& fs, const GitSearch& search);

} // namespace frost::vcs
