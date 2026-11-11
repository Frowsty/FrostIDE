// SPDX-License-Identifier: Apache-2.0
//
// RunConfiguration (RUN-1, PLAN.md Appendix C R2): the Qt-free run configuration model that the Run
// toolbar, the Run/Debug Configurations dialog, the generated CMake and MSBuild configurations
// (RUN-2), the test runner (RUN-7) and the debugger share, its validation, and its per-workspace
// JSON store under <workspace>/.frost/runConfigurations/. RUN-5 (R3) adds before-launch tasks,
// compound configurations and attach configurations.
//
/*
 * Model. A configuration says what to run and how; it never holds results. Its type says where the
 * program comes from:
 *   - native:        `program` is an absolute path given by the user; nothing is built first.
 *   - cmake-target:  `target` names a CMake target of the open CMake project; `program` is empty
 *                    until the before-launch step resolves the target's artifact (RUN-2), or an
 *                    absolute path the user pinned.
 *   - msbuild:       `target` is the absolute path of a .vcxproj (or other project file); `program`
 *                    is resolved like cmake-target's (RUN-2, TargetPath), or pinned.
 *   - compound:      `configurations` lists the ids of the configurations it starts together, in
 *                    that order (RUN-5). Run-only: compound debugging is DBG-10's. It has no
 *                    program, target, arguments, environment or before-launch steps of its own.
 *   - attach:        `target` names the running process the debugger attaches to: its executable's
 *                    file name ("FrostEditor.exe", ASCII case ignored on Windows) or the absolute
 *                    path of its executable (RUN-5). Debug-only: there is nothing to run. It has no
 *                    program, arguments, working directory, environment or before-launch steps.
 *   - frost-editor / frost-player: `program` selects the actual editor/player executable and build
 *                    configuration; `engineProjectPath` names an absolute project directory or
 *                    .frostproj. launchPlanFor adds --project with that path (RUN-11).
 * beforeLaunch Build asks the RunManager's before-launch handler to build `target` first (only for
 * cmake-target and msbuild: a native program has nothing FrostIDE could build).
 *
 * Before-launch tasks (RUN-5, Rider's "Run External Tool"): `beforeLaunchTasks` are programs the
 * RunManager runs one after another, after the Build step and before the program starts (for Run and
 * for Debug), each through IProcessLauncher with its output streamed to the run. A task that exits
 * with a code other than 0, or cannot start, stops the run: the program is not started. Each task's
 * program is absolute; its working directory is its own, else the configuration's, else the
 * program's folder; it gets FrostIDE's own environment (not the configuration's changes). Tasks are
 * allowed on native, cmake-target and FrostEngine configurations; msbuild configurations run through the
 * RunService, which has no task step yet, so they refuse tasks rather than skip them.
 *
 * Store. One UTF-8 JSON file per workspace, <workspace>/.frost/runConfigurations/configurations.json
 * (settings::ProjectLayout::runConfigurations; never a file of the user's project such as a .vcxproj
 * or CMakePresets.json):
 *
 *     {
 *       "version": 1,
 *       "configurations": [
 *         {
 *           "id": "frost-editor",
 *           "name": "FrostEditor",
 *           "type": "cmake-target",
 *           "target": "FrostEditor",
 *           "program": "",
 *           "arguments": ["--project", "Demo"],
 *           "workingDirectory": "",
 *           "environment": [{"name": "FROST_LOG", "value": "debug"}, {"name": "TMP", "unset": true}],
 *           "beforeLaunch": "build"
 *         }
 *       ]
 *     }
 *
 * Version 2 (RUN-5) adds two optional members, written only when not empty:
 * "beforeLaunchTasks": [{"program": "C:\\tools\\gen.exe", "arguments": ["--out", "gen"],
 * "workingDirectory": ""}] and, for a compound configuration, "configurations": ["cmake.FrostEditor",
 * "native.server"]; and the types "compound" and "attach". A store that uses none of them is still
 * written as version 1, so a FrostIDE before RUN-5 reads it; one that uses them is version 2, which
 * such a FrostIDE refuses (Unsupported) instead of dropping what it does not know.
 *
 * Version 3 (RUN-11) adds "frost-editor", "frost-player" and "engineProjectPath". Stores
 * using these types require version 3; ordinary stores retain their version 1 or 2 format.
 *
 * Reading is strict about what it understands and lenient about what it does not: unknown members
 * are ignored (a later version-1 writer may add some), but a missing or wrong-typed known member,
 * an unknown type or beforeLaunch spelling, an invalid configuration (validateRunConfiguration) or
 * a duplicate id refuses the whole file (Corrupt, naming the entry), so a damaged file is reported
 * and never silently overwritten with fewer configurations. A newer version is Unsupported.
 *
 * Thread safety: plain values and pure functions; the store functions are as thread-safe as the
 * IFileSystem passed in (the real one is), but concurrent saves to one file race (last one wins).
 */
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::run {

enum class RunType : std::uint8_t { Native, CMakeTarget, MSBuild, Compound, Attach, FrostEditor, FrostPlayer };

// JSON spelling, including "frost-editor" and "frost-player" (RUN-11).
[[nodiscard]] std::string_view toString(RunType type) noexcept;
// The type for its JSON spelling; nullopt for anything else. Case-sensitive.
[[nodiscard]] std::optional<RunType> parseRunType(std::string_view text) noexcept;

// What happens before the program starts (header comment).
enum class BeforeLaunch : std::uint8_t { None, Build };

// "none" or "build" (the JSON spelling).
[[nodiscard]] std::string_view toString(BeforeLaunch step) noexcept;
[[nodiscard]] std::optional<BeforeLaunch> parseBeforeLaunch(std::string_view text) noexcept;

// One environment change, applied in order on top of FrostIDE's own environment: a value sets the
// variable, nullopt removes it (io::ProcessSpec::environment's rule).
struct EnvironmentEntry {
    std::string name{};
    std::optional<std::string> value{};

    friend bool operator==(const EnvironmentEntry&, const EnvironmentEntry&) = default;
};

// One before-launch task (header comment, "Before-launch tasks").
struct BeforeLaunchTask {
    std::string program{};                // Absolute.
    std::vector<std::string> arguments{}; // argv[1..], passed exactly.
    std::string workingDirectory{};       // Absolute, or empty (header comment).

    friend bool operator==(const BeforeLaunchTask&, const BeforeLaunchTask&) = default;
};

struct RunConfiguration {
    // Unique within the workspace and stable across renames: 1 to 128 of [A-Za-z0-9._-].
    std::string id{};
    // What the Run toolbar and the Run tool window show; non-empty UTF-8.
    std::string name{};
    RunType type = RunType::Native;
    // cmake-target: the CMake target's name; msbuild: the absolute project file path; attach: the
    // process's executable name or absolute path; native and compound: empty.
    std::string target{};
    // The absolute program path (UTF-8), or empty when the before-launch handler resolves it.
    std::string program{};
    // argv[1..], passed exactly (io::IProcessLauncher's quoting rules).
    std::vector<std::string> arguments{};
    // Absolute, or empty for the program's own folder.
    std::string workingDirectory{};
    std::vector<EnvironmentEntry> environment{};
    BeforeLaunch beforeLaunch = BeforeLaunch::None;
    // Run after the Build step, before the program (header comment); native, cmake-target and engine types.
    std::vector<BeforeLaunchTask> beforeLaunchTasks{};
    // compound: the member configurations' ids, in start order; empty for every other type.
    std::vector<std::string> configurations{};

    // RUN-11: absolute project directory or .frostproj file. The explicit program selects the
    // editor/player build configuration. Empty on other types. Both types own their launched process;
    // pairing with an external editor is an EngineSession operation, never a run configuration.
    std::string engineProjectPath{};

    friend bool operator==(const RunConfiguration&, const RunConfiguration&) = default;
};

// Whether `configuration` uses a version-2 feature (header comment, "Version 2"). Pure.
[[nodiscard]] bool usesRun5Features(const RunConfiguration& configuration) noexcept;

// Whether a running process whose executable is `executablePath` (absolute) is the one an attach
// configuration's `target` names (header comment, attach). Pure.
[[nodiscard]] bool attachTargetMatches(std::string_view target, std::string_view executablePath) noexcept;

/*
 * Checks the rules in the comments above: the id's characters and length; a non-empty name;
 * native: a program, no target and no Build step; cmake-target: a target; msbuild: an absolute
 * target; compound: at least one member, each a valid id, none twice and not its own; attach: a
 * target that is a file name or an absolute path; compound and attach: nothing else set; tasks only
 * on native, cmake-target and FrostEngine types, each with an absolute program and an empty or absolute working
 * directory; program and workingDirectory empty or absolute (io::normalizePath accepts them);
 * environment names non-empty and free of '=' and NUL, values and arguments free of NUL.
 * Errors: InvalidArgument naming the configuration and the field. Pure. O(size).
 */
[[nodiscard]] Result<void> validateRunConfiguration(const RunConfiguration& configuration);

// The store's newest format version; files with a higher one are refused (Unsupported). Stores
// retain version 1 or 2 when no RUN-11 types are present (header comment, "Version 3").
inline constexpr int kRunConfigurationsVersion = 3;

// Whether this is a RUN-11 editor/player configuration. Pure, constant time.
[[nodiscard]] bool isFrostEngineRunType(RunType type) noexcept;

// Parses a store file's text (header comment). Errors: Corrupt (malformed JSON, a bad entry, a
// duplicate id), Unsupported (a newer version). O(size).
[[nodiscard]] Result<std::vector<RunConfiguration>> parseRunConfigurations(std::string_view json);

// The store file's text for `configurations`, indented, ending in a newline; parseRunConfigurations
// reads it back equal. Precondition: each passes validateRunConfiguration and ids are unique (not
// checked here; saveRunConfigurations checks). O(size).
[[nodiscard]] std::string serializeRunConfigurations(std::span<const RunConfiguration> configurations);

// <workspaceRoot>/.frost/runConfigurations/configurations.json. Errors: InvalidArgument (the root is
// not absolute). Pure.
[[nodiscard]] Result<std::string> workspaceRunConfigurationsPath(std::string_view workspaceRoot);

// Reads the store at `path` (absolute). A missing file holds no configurations. Errors: those of
// parseRunConfigurations and readFile (blocking; the file is small).
[[nodiscard]] Result<std::vector<RunConfiguration>> loadRunConfigurations(io::IFileSystem& fs, std::string_view path);

/*
 * Writes `configurations` to `path` atomically (writeFileAtomic), creating the parent directories.
 * Under a workspace's .frost/ use saveWorkspaceRunConfigurations, which also writes .frost/'s
 * .gitignore. Errors: InvalidArgument (a configuration fails validation, or two share an id; nothing
 * is written), the file system's errors.
 */
[[nodiscard]] Result<void> saveRunConfigurations(io::IFileSystem& fs, std::string_view path,
                                                 std::span<const RunConfiguration> configurations);

// saveRunConfigurations to workspaceRunConfigurationsPath(root), after settings::ensureProjectLayout
// (which creates .frost/ and its .gitignore the first time). Errors: as both.
[[nodiscard]] Result<void> saveWorkspaceRunConfigurations(io::IFileSystem& fs, std::string_view workspaceRoot,
                                                          std::span<const RunConfiguration> configurations);

} // namespace frost::run
