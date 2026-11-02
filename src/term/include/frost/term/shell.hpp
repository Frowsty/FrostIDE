// SPDX-License-Identifier: Apache-2.0
//
// Which shell a terminal tab starts, where, and with which environment (ADR 0051): the default shell
// detection, the trust rule of PLAN.md section 3.5 for the terminal, and the terminal settings.
/*
 * Trust rule. With a project root open that the user has not trusted, the terminal is restricted: it
 * runs the application-scope shell (terminal.shell is an Application-only key, so a project file can
 * never name it, and the default is detected without looking inside the root) at its absolute path,
 * with the user's home as working directory and NoDefaultCurrentDirectoryInExePath=1, so a command
 * typed there never resolves to a binary planted in the root. With a trusted root the shell starts in
 * the root and NoDefaultCurrentDirectoryInExePath is removed, for Visual Studio-compatible cmd.exe
 * behaviour (ADR 0011, E2). Without a project the shell starts in the home directory, unrestricted.
 *
 * Environment. TERM=xterm-256color, COLORTERM=truecolor, TERM_PROGRAM=FrostIDE and
 * TERM_PROGRAM_VERSION=<FrostIDE version> on top of FrostIDE's own environment.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/process.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::settings {
class SettingsSnapshot;
} // namespace frost::settings

namespace frost::term {

// What the shell detection looks at; the system probe reads the real environment and file system.
struct ShellProbe {
    // An environment variable of this process, nullopt when unset.
    std::function<std::optional<std::string>(std::string_view name)> environment;
    // True when `path` (absolute) is an existing regular file the user may execute.
    std::function<bool(const std::string& path)> isExecutable;
    // Windows: the system directory (C:\Windows\System32); unused on Linux.
    std::string systemDirectory;
};

// The probe of this process (GetEnvironmentVariableW / getenv, GetFileAttributesW / access(X_OK)).
[[nodiscard]] ShellProbe systemShellProbe();

struct ShellCommand {
    std::string executable; // Absolute.
    std::vector<std::string> arguments;

    friend bool operator==(const ShellCommand&, const ShellCommand&) = default;
};

/*
 * The default shell. Windows: pwsh.exe (%ProgramFiles%\PowerShell\7, then the absolute PATH entries
 * that are not inside `excludeRoot`), else Windows PowerShell (<system>\WindowsPowerShell\v1.0\
 * powershell.exe), else <system>\cmd.exe. Linux: $SHELL when it is absolute and executable, else
 * /bin/bash, else /bin/sh. `excludeRoot` is the open project root ("" for none): the detection never
 * picks a binary inside it. Errors: NotFound when none exists.
 */
[[nodiscard]] Result<ShellCommand> defaultShell(const ShellProbe& probe, std::string_view excludeRoot);

// The terminal's settings (terminal.shell and terminal.scrollbackLines).
struct TerminalSettings {
    // An absolute path, or "" for defaultShell().
    std::string shell;
    std::size_t scrollbackLines = 10000;
};

// The terminal keys of `snapshot` (frost::settings' built-in registry).
[[nodiscard]] TerminalSettings terminalSettings(const settings::SettingsSnapshot& snapshot);

struct TerminalLaunchRequest {
    // The open project root (absolute), "" for none.
    std::string projectRoot;
    // Whether the user trusts projectRoot (TrustStore::isTrusted).
    bool trusted = false;
    // terminal.shell; "" picks defaultShell().
    std::string configuredShell;
    // The folder to start in instead of the project root ("Open in Terminal"); "" for none. Ignored
    // when the terminal is restricted.
    std::string workingDirectory;
};

struct TerminalLaunch {
    // The spec for IPtyLauncher::open (streams Inherit, killOnClose).
    io::ProcessSpec spec;
    // True under the restricted rule (an untrusted project root).
    bool restricted = false;
};

/*
 * Applies the rules of the header comment. Errors: InvalidArgument (a configuredShell or
 * workingDirectory that is not absolute), NotFound (no shell; no home directory when one is needed).
 */
[[nodiscard]] Result<TerminalLaunch> planTerminalLaunch(const TerminalLaunchRequest& request, const ShellProbe& probe);

// Whether `path` lies in `root` or below it (lexically, after normalisation; ASCII case ignored on
// Windows). False for an empty root.
[[nodiscard]] bool isInsideRoot(std::string_view path, std::string_view root);

} // namespace frost::term
