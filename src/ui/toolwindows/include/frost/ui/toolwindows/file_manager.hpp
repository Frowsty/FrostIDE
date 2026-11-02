// SPDX-License-Identifier: Apache-2.0
//
// Open Containing Folder (W1, ADR 0039): the file-manager launch behind the Solution Explorer's
// command, built as a ProcessSpec first so tests check it against the scripted launcher.
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/process.hpp"

#include <string_view>

namespace frost::ui {

/*
 * The command that shows `path` (absolute) in the platform's file manager:
 *   Windows: <Windows directory>\explorer.exe "/select,<path>" for a file (the folder opens with
 *            the file selected) and explorer.exe <path> for a directory; the Windows directory
 *            comes from GetWindowsDirectoryW, never from PATH;
 *   Linux:   /usr/bin/xdg-open <directory>, the file's directory for a file (xdg-open cannot
 *            select one).
 * The child is not contained (killOnClose off) and gets null standard streams, so it outlives the
 * IDE. Errors: InvalidArgument (a relative path), Unsupported (another platform), IoError (the
 * Windows directory cannot be read).
 */
[[nodiscard]] Result<io::ProcessSpec> fileManagerSpec(std::string_view path, bool isDirectory);

/*
 * Starts fileManagerSpec(path, isDirectory) through `launcher` and lets it run (nothing waits for
 * it). On Linux the exited child stays a zombie until the IDE exits (TODO(M3): the supervisor reaps
 * detached children). Errors: those of fileManagerSpec and IProcessLauncher::launch.
 */
[[nodiscard]] Result<void> revealInFileManager(io::IProcessLauncher& launcher, std::string_view path, bool isDirectory);

} // namespace frost::ui
