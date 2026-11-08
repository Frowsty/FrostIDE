// SPDX-License-Identifier: Apache-2.0
//
// The run configuration types (RUN-2, PLAN.md Appendix C R2): the configurations FrostIDE generates
// from a project model, one per thing that can run, and the native type the user adds by hand.
// Absorbs CMK-6 (per-target CMake run configurations). Qt-free and pure.
//
/*
 * Generated configurations. Each is a plain RunConfiguration (run_configuration.hpp) with a stable,
 * derived id, so the user's edits to it (arguments, working directory, environment, name) can be
 * stored under that id and laid over the next generation (RunConfigurationSet,
 * run_configuration_set.hpp):
 *   - CMake (cmakeRunConfigurations): one "cmake-target" configuration per target whose File API
 *     codemodel type is EXECUTABLE, in the codemodel's order, id "cmake.<target>", name the
 *     target's name, beforeLaunch Build (the target is built first, as Rider and CLion do) and the
 *     working directory the caller names (the project's source directory: what Visual Studio's
 *     CMake support and FrostEngine's VS_DEBUGGER_WORKING_DIRECTORY use). The program stays empty:
 *     the before-launch step resolves it from the target's artifacts after the build
 *     (cmakeTargetProgram), because a reconfigure can move it.
 *   - MSBuild (msbuildRunConfigurations): one "msbuild" configuration per loaded C++ project of a
 *     solution whose ConfigurationType is Application in the given solution configuration (the
 *     project's mapped configuration, else its first value), the projects the startup project combo
 *     can run (build::RunService); id "msbuild.<project>", target the .vcxproj, beforeLaunch Build.
 *     Program, arguments, working directory and environment come from MSBuild at run time
 *     (TargetPath and the .vcxproj.user debugger settings, build/run_target.hpp), so they stay empty
 *     here. .NET projects (OutputType Exe/WinExe) run through the managed debug path, not here: the
 *     solution model does not evaluate their OutputType. TODO(M4): LTR-3 adds .NET run types.
 *   - Native (nativeRunConfiguration): any absolute program, added by the user.
 * Ids are "<prefix>.<name>" with every character outside the id alphabet ([A-Za-z0-9._-]) replaced
 * by '_' and cut to 128 characters (runConfigurationIdFor), so two targets whose names differ only in
 * such characters get the same id; the generators then add "-2", "-3", ... (uniqueRunConfigurationId).
 *
 * Thread safety: pure functions over values. Complexity: linear in the model's size.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/run/run_configuration.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::project {
struct CMakeProject;
struct CMakeTarget;
class SolutionModel;
} // namespace frost::project

namespace frost::run {

// The id prefixes of the generated types (header comment).
inline constexpr std::string_view kCMakeIdPrefix = "cmake";
inline constexpr std::string_view kMSBuildIdPrefix = "msbuild";
inline constexpr std::string_view kNativeIdPrefix = "native";

// "<prefix>.<name>" in the id alphabet, at most 128 characters (header comment). Pure.
[[nodiscard]] std::string runConfigurationIdFor(std::string_view prefix, std::string_view name);

// `base` if no configuration in `taken` has that id, else base + "-2", "-3", ... (cut so the id
// stays within 128 characters). Pure. O(taken).
[[nodiscard]] std::string uniqueRunConfigurationId(std::string_view base, std::span<const RunConfiguration> taken);

// One configuration per EXECUTABLE target of `model` (header comment, "CMake"); `workingDirectory`
// (absolute, or empty for the program's folder) is each one's working directory. Pure.
[[nodiscard]] std::vector<RunConfiguration> cmakeRunConfigurations(const project::CMakeProject& model,
                                                                   std::string_view workingDirectory);

/*
 * The program a built EXECUTABLE target runs: on Windows its first artifact ending in ".exe" (ASCII
 * case ignored), else its first artifact that is not a ".pdb"; elsewhere its first artifact (the
 * File API lists the executable first). Errors: NotFound (the target has no such artifact). Pure.
 */
[[nodiscard]] Result<std::string> cmakeTargetProgram(const project::CMakeTarget& target);

// One configuration per loaded C++ Application project of `solution` in solution configuration
// `configuration` (an index into solution.configurations(); nullopt: each project's first
// ConfigurationType), in solution order (header comment, "MSBuild"). Pure.
[[nodiscard]] std::vector<RunConfiguration> msbuildRunConfigurations(const project::SolutionModel& solution,
                                                                     std::optional<std::size_t> configuration);

// A native configuration named `name` that runs `program` (absolute), with an id derived from the
// name that no configuration in `taken` has. The result may still fail validation (an empty name, a
// relative program): the dialog validates before it saves. Pure.
[[nodiscard]] RunConfiguration nativeRunConfiguration(std::string_view name, std::string_view program,
                                                      std::span<const RunConfiguration> taken);

} // namespace frost::run
