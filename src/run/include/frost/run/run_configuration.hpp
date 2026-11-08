// SPDX-License-Identifier: Apache-2.0
//
// RunConfiguration (RUN-1, PLAN.md Appendix C R2): the Qt-free run configuration model that the Run
// toolbar, the Run/Debug Configurations dialog, the generated CMake and MSBuild configurations
// (RUN-2), the test runner (RUN-7) and the debugger share, its validation, and its per-workspace
// JSON store under <workspace>/.frost/runConfigurations/.
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
 * beforeLaunch Build asks the RunManager's before-launch handler to build `target` first (only for
 * cmake-target and msbuild: a native program has nothing FrostIDE could build). RUN-5 adds more
 * before-launch steps.
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

enum class RunType : std::uint8_t { Native, CMakeTarget, MSBuild };

// "native", "cmake-target" or "msbuild" (the JSON spelling).
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

struct RunConfiguration {
    // Unique within the workspace and stable across renames: 1 to 128 of [A-Za-z0-9._-].
    std::string id{};
    // What the Run toolbar and the Run tool window show; non-empty UTF-8.
    std::string name{};
    RunType type = RunType::Native;
    // cmake-target: the CMake target's name; msbuild: the absolute project file path; native: empty.
    std::string target{};
    // The absolute program path (UTF-8), or empty when the before-launch handler resolves it.
    std::string program{};
    // argv[1..], passed exactly (io::IProcessLauncher's quoting rules).
    std::vector<std::string> arguments{};
    // Absolute, or empty for the program's own folder.
    std::string workingDirectory{};
    std::vector<EnvironmentEntry> environment{};
    BeforeLaunch beforeLaunch = BeforeLaunch::None;

    friend bool operator==(const RunConfiguration&, const RunConfiguration&) = default;
};

/*
 * Checks the rules in the comments above: the id's characters and length; a non-empty name;
 * native: a program, no target and no Build step; cmake-target: a target; msbuild: an absolute
 * target; program and workingDirectory empty or absolute (io::normalizePath accepts them);
 * environment names non-empty and free of '=' and NUL, values and arguments free of NUL.
 * Errors: InvalidArgument naming the configuration and the field. Pure. O(size).
 */
[[nodiscard]] Result<void> validateRunConfiguration(const RunConfiguration& configuration);

// The store's format version; files with a higher one are refused (Unsupported).
inline constexpr int kRunConfigurationsVersion = 1;

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
