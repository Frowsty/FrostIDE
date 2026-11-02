// SPDX-License-Identifier: Apache-2.0
//
// What Run starts for one project configuration (W1, ADR 0040): the properties MSBuild reports for
// it (TargetPath and Visual Studio's local-debugger settings, the per-user .vcxproj.user included),
// the static fallback for an MSBuild without -getProperty, $(Macro) and %VAR% expansion, the
// debugger environment, the PE subsystem of the program, and the process spec of each console mode.
//
/*
 * Properties. runPropertyQuerySpec asks MSBuild itself (17.8+) with -getProperty, so imported .props
 * files, the user's SDK property sheets and the .vcxproj.user file are honoured exactly as Visual
 * Studio sees them. The query evaluates the project and runs no target, but evaluation can still run
 * project code (property functions), so only callers that passed the trust gate (PLAN.md section
 * 3.5) start it. When MSBuild rejects the switch (MSB1001), staticRunProperties reads the .vcxproj
 * and .vcxproj.user without evaluation and applies MSBuild's C++ defaults, with a warning.
 *
 * Expansion. MSBuild has already expanded what it evaluated; expandRunMacros runs once more over
 * the command, the arguments and the working directory (the fallback has nothing else) with the
 * names of runMacros, then the environment; unknown names expand to "", as in MSBuild. Debugger
 * environment values also expand %NAME% from the process environment (Visual Studio's syntax).
 *
 * Console modes (runProcessSpec):
 *   - RunConsole: the program's stdin, stdout and stderr are pipes to FrostIDE's Run console;
 *   - ExternalConsole (Windows): a console window of its own, through
 *         cmd.exe /d /s /c ""<program>" <arguments> & ... & pause & call exit %^FROST_RUN_EXIT%"
 *     which prints "Process finished with exit code N." and "Press any key to continue . . .",
 *     keeps the window open like Visual Studio's Ctrl+F5, and exits with the program's code. The
 *     arguments pass through cmd.exe there, which expands %VAR% and treats & | < > outside quotes
 *     as operators (as Visual Studio's own cmd wrapper did);
 *   - NoConsole (GUI-subsystem programs): stdout and stderr still go to the Run console, stdin
 *     is the null device.
 * Every mode contains the tree (killOnClose), sets the working directory and the environment
 * (merged over FrostIDE's, or replacing it when LocalDebuggerMergeEnvironment is false), and drops
 * NoDefaultCurrentDirectoryInExePath, which the user's own program does not expect (ADR 0011, E2).
 *
 * Arguments. In RunConsole and NoConsole modes the argument string is split with the MSVC CRT's
 * rules (splitWindowsArguments) and re-quoted by frost::io, so a CRT program receives exactly the
 * argv Visual Studio would give it. Redirection (< > in the arguments, which Visual Studio's
 * debugger implements itself) is not interpreted there. TODO(M4): redirection in run options.
 *
 * Everything here is pure and portable except queryRunProperties (a child process) and
 * systemCommandProcessor (Windows); paths follow Windows rules on every platform where they come
 * from MSBuild. Thread-safe (no shared state).
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"
#include "frost/project/vcxproj.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::build {

// What a project configuration runs, as MSBuild (or the fallback) reports it.
struct RunProperties {
    std::string targetPath;            // $(TargetPath): the built program, absolute.
    std::string targetDir;             // $(TargetDir), ending in a separator.
    std::string projectDir;            // $(ProjectDir), ending in a separator.
    std::string solutionDir;           // $(SolutionDir), ending in a separator.
    std::string configurationType;     // "Application", "DynamicLibrary", "StaticLibrary", ...
    std::string command;               // LocalDebuggerCommand; "" means targetPath.
    std::string commandArguments;      // LocalDebuggerCommandArguments: one command-line string.
    std::string workingDirectory;      // LocalDebuggerWorkingDirectory; "" means projectDir.
    std::string environment;           // LocalDebuggerEnvironment: NAME=VALUE lines.
    bool mergeEnvironment = true;      // LocalDebuggerMergeEnvironment; only "false" turns it off.
    bool fromMsBuild = true;           // False when staticRunProperties produced them.
    std::vector<std::string> warnings; // Why the fallback was used and what it could not know.

    friend bool operator==(const RunProperties&, const RunProperties&) = default;
};

// The MSBuild properties the query asks for, in order: TargetPath, TargetDir, ProjectDir,
// SolutionDir, ConfigurationType, LocalDebuggerCommand, LocalDebuggerCommandArguments,
// LocalDebuggerWorkingDirectory, LocalDebuggerEnvironment, LocalDebuggerMergeEnvironment.
[[nodiscard]] std::span<const std::string_view> runPropertyNames() noexcept;

// One project configuration to ask about.
struct RunPropertyRequest {
    std::string msbuildPath;   // Absolute MSBuild.exe.
    std::string projectPath;   // Absolute .vcxproj.
    std::string solutionPath;  // Absolute .sln or .slnx; "" passes no Solution* properties.
    std::string configuration; // The project's configuration ("Debug"), from the solution's mapping.
    std::string platform;      // The project's platform ("x64", "Win32").
};

/*
 * MSBuild.exe <project> -getProperty:<name>... -p:Configuration=<c> -p:Platform=<p> [the Solution*
 * properties Visual Studio passes] -nologo, in the project's directory, with the build driver's
 * environment (English, no terminal logger, no node reuse). Evaluation only: no target runs.
 * Pure and portable (Windows path rules). Errors: InvalidArgument (a path that is not absolute,
 * an empty configuration or platform).
 */
[[nodiscard]] Result<io::ProcessSpec> runPropertyQuerySpec(const RunPropertyRequest& request);

// Parses the JSON MSBuild prints for several -getProperty switches ({"Properties": {...}});
// missing properties stay empty. Errors: Corrupt (not that JSON).
[[nodiscard]] Result<RunProperties> parseRunProperties(std::string_view json);

/*
 * Runs runPropertyQuerySpec through `launcher` (blocking, up to `timeout`) and parses the answer.
 * Trusted callers only (header comment). Errors: Unsupported (not Windows, or this MSBuild has no
 * -getProperty: its MSB1001 "unknown switch"), IoError (MSBuild failed; the message holds its
 * output), Timeout, Corrupt, the launcher's errors.
 */
[[nodiscard]] Result<RunProperties> queryRunProperties(io::IProcessLauncher& launcher,
                                                       const RunPropertyRequest& request, IClock::Duration timeout);

/*
 * The fallback without MSBuild: the .vcxproj's and then the .vcxproj.user's properties for the
 * configuration (project::parseConfigurationProperties), expanded with runMacros' names, over
 * MSBuild's C++ defaults: OutDir $(SolutionDir)$(Platform)\$(Configuration)\ ($(SolutionDir)
 * $(Configuration)\ for Win32), TargetName $(ProjectName), TargetExt .exe or .dll by
 * ConfigurationType, the debugger settings as written. fromMsBuild is false and `warnings` says
 * that imports were not read. A missing .user file is not an error. Errors: those of readFile
 * for the project; Corrupt (malformed XML).
 */
[[nodiscard]] Result<RunProperties> staticRunProperties(io::IFileSystem& fs, const RunPropertyRequest& request,
                                                        const project::EnvironmentLookup& environment = {});

// Lower-case macro names to values, for expandRunMacros.
using RunMacros = std::map<std::string, std::string, std::less<>>;

// ProjectDir, TargetDir, TargetPath, SolutionDir, ProjectName, Configuration and Platform from
// `properties` and the request; TargetName and TargetExt derived from targetPath.
[[nodiscard]] RunMacros runMacros(const RunProperties& properties, const RunPropertyRequest& request);

// `text` with each $(Name) replaced: `macros` first (ASCII case ignored), then `environment`
// (empty: the process environment); unknown names become "". An unclosed "$(" stays as written.
[[nodiscard]] std::string expandRunMacros(std::string_view text, const RunMacros& macros,
                                          const project::EnvironmentLookup& environment = {});

// `text` with each %NAME% that `environment` defines replaced by its value; undefined names and a
// lone '%' stay as written (cmd.exe's rule).
[[nodiscard]] std::string expandPercentVariables(std::string_view text, const project::EnvironmentLookup& environment);

/*
 * LocalDebuggerEnvironment's lines: NAME=VALUE separated by CR, LF or CRLF; blank lines are
 * skipped, a line without '=' or with an empty name is skipped with a warning in `warnings`
 * (optional). Values have %NAME% expanded (expandPercentVariables). Order is kept; a later line
 * for the same name wins when the spec is applied.
 */
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
parseDebuggerEnvironment(std::string_view text, const project::EnvironmentLookup& environment,
                         std::vector<std::string>* warnings = nullptr);

/*
 * Splits an argument string into argv with the MSVC CRT's rules ("Parsing C++ command-line
 * arguments", https://learn.microsoft.com/en-us/cpp/cpp/main-function-command-line-args): spaces
 * and tabs separate arguments outside quotes; a quote toggles quoting; 2n backslashes before a
 * quote give n backslashes and a toggle, 2n+1 give n backslashes and a literal quote; "" inside
 * quotes is a literal quote; other backslashes are literal; "" alone is an empty argument. The
 * inverse of io::appendWindowsArgument. O(n).
 */
[[nodiscard]] std::vector<std::string> splitWindowsArguments(std::string_view text);

// The Windows subsystem a program image declares.
enum class Subsystem : std::uint8_t { Unknown, Console, Gui };

// "Unknown", "Console" or "GUI".
[[nodiscard]] std::string_view toString(Subsystem subsystem) noexcept;

// The Subsystem field of a PE image's optional header: IMAGE_SUBSYSTEM_WINDOWS_CUI (3) is
// Console, IMAGE_SUBSYSTEM_WINDOWS_GUI (2) is Gui, anything else Unknown. Only the headers are
// looked at. Errors: Corrupt (no MZ or PE signature, or truncated headers).
[[nodiscard]] Result<Subsystem> peSubsystem(std::string_view image);

// Where a program's console goes (header comment).
enum class ConsoleMode : std::uint8_t { RunConsole, ExternalConsole, NoConsole };

// "Run console", "external console" or "no console".
[[nodiscard]] std::string_view toString(ConsoleMode mode) noexcept;

// A resolved run: what to start, where and with which environment.
struct RunCommand {
    std::string executable;       // Absolute.
    std::string arguments;        // The expanded argument string, as the user wrote it.
    std::string workingDirectory; // Absolute.
    // NAME=VALUE pairs set over the process environment (or forming the whole of it).
    std::vector<std::pair<std::string, std::string>> environment;
    bool mergeEnvironment = true;
    std::vector<std::string> warnings; // Skipped environment lines.

    // "\"<executable>\" <arguments>", for the Run console's first line.
    [[nodiscard]] std::string displayCommandLine() const;

    friend bool operator==(const RunCommand&, const RunCommand&) = default;
};

/*
 * Resolves `properties`: the command (LocalDebuggerCommand, else TargetPath), the arguments, the
 * working directory (LocalDebuggerWorkingDirectory, else ProjectDir) and the environment expanded
 * with `macros`; a relative working directory resolves against ProjectDir; the environment parsed
 * (parseDebuggerEnvironment). Errors:
 * InvalidArgument (no command at all, or a command or directory that is not absolute after
 * expansion).
 */
[[nodiscard]] Result<RunCommand> resolveRunCommand(const RunProperties& properties, const RunMacros& macros,
                                                   const project::EnvironmentLookup& environment = {});

/*
 * The process spec for `command` in `mode` (header comment). `cmdPath` (absolute cmd.exe) is
 * needed for ExternalConsole only. Errors: InvalidArgument (ExternalConsole without cmdPath, or a
 * quote in the executable path, which no Windows command line can carry).
 */
[[nodiscard]] Result<io::ProcessSpec> runProcessSpec(const RunCommand& command, ConsoleMode mode,
                                                     std::string_view cmdPath = {});

// The cmd.exe of this Windows installation (GetSystemDirectoryW + "\cmd.exe"). Errors: Unsupported
// (not Windows), IoError.
[[nodiscard]] Result<std::string> systemCommandProcessor();

} // namespace frost::build
