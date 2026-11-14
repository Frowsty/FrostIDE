// SPDX-License-Identifier: Apache-2.0
//
// The MSBuild build driver: Build, Rebuild and Clean of a whole .sln/.slnx or of one project in
// it, run through IProcessLauncher as a BuildSession (frost/build/build_session.hpp).
//
/*
 * The command line (msbuildProcessSpec):
 *
 *     MSBuild.exe <solution or project> -t:<target> -p:Configuration=<c> -p:Platform=<p>
 *                 [-p:SolutionDir=... and the other Solution* properties] [-p:<extra>...]
 *                 -m[:N] -nologo -nodeReuse:false -v:<verbosity>
 *                 -clp:NoSummary;ForceNoAlign;DisableConsoleColor
 *
 *   - The target is the action ("Build", "Rebuild", "Clean"). One project of a solution is built
 *     through the solution's own per-project targets, "<Name>" for Build and "<Name>:Rebuild" or
 *     "<Name>:Clean", so the solution's configuration mapping and build order apply; or, with
 *     projectPath, the project file itself is built with the Solution* properties Visual Studio
 *     passes (SolutionDir with its trailing backslash, SolutionPath, SolutionName, SolutionFileName,
 *     SolutionExt), so the project's $(SolutionDir) paths resolve.
 *   - Compile (Visual Studio's Ctrl+F7, SLN-9) builds the ClCompile target of one project file
 *     (projectPath, never a solution's per-project target) with -p:SelectedFiles=<items>: the
 *     C++ targets then compile only the ClCompile items whose Identity (the Include as MSBuild
 *     evaluated it, "src\a.cpp", not the full path) equals one of them, plus the project's
 *     precompiled-header source, and build no referenced project. An Identity that matches no
 *     item compiles the whole project, so callers take it from MSBuild's own evaluation
 *     (msbuild_compile.hpp) rather than spelling it themselves.
 *   - -nodeReuse:false (and MSBUILDDISABLENODEREUSE=1): no MSBuild node outlives the build.
 *   - The console logger prints one line per diagnostic in the canonical format
 *     (frost/build/diagnostic.hpp): NoSummary drops the repeated summary, ForceNoAlign the
 *     wrapping, DisableConsoleColor the escape codes.
 *   - The working directory is the directory of the solution (or project) file.
 *
 * The environment (the parent's, changed as follows):
 *   - DOTNET_CLI_UI_LANGUAGE: the request's uiLanguage, by default en-US whatever the Windows UI
 *     language is, so the diagnostic parser always sees English category words (ADR 0035). MSBuild
 *     writes UTF-8 only when this override is set (otherwise it uses the console's OEM code page,
 *     measured with MSBuild 17.14 on 2026-11-01: "é" arrived as CP437's 0x82).
 *   - VSLANG=1033 (English) for the tools MSBuild runs (cl, link), unless the request names another
 *     uiLanguage; they then follow the Windows UI language.
 *   - MSBUILDTERMINALLOGGER=off, so a user's opt-in to the terminal logger cannot change the format.
 *   - MSBUILDDISABLENODEREUSE=1 (see above).
 *   - NoDefaultCurrentDirectoryInExePath removed: builds run the user's own scripts (Exec tasks,
 *     build events), which get Visual Studio's cmd.exe behaviour (ADR 0011, E2).
 *   - _MSPDBSRV_ENDPOINT_: a name unique to the build, so the compiler's PDB server is the build's
 *     own. The session ends it when MSBuild exits, which must not break a concurrent Visual Studio
 *     build that would otherwise share it. (Set by startMsBuild only.)
 *
 * After MSBuild exits, startMsBuild's session ends only the build infrastructure still in the job
 * (msbuildInfrastructureImages: worker nodes, the PDB server, the compiler server) and lets
 * anything else a build step started keep running, as Visual Studio does (ADR 0035). Cancelling
 * still ends the whole tree.
 *
 * Trust: the request's `trusted` flag must be set (the trust store and prompt are other slices'),
 * or nothing starts and the error is Untrusted. Off Windows startMsBuild fails with Unsupported;
 * msbuildProcessSpec is portable, so the command line is tested on every platform.
 * TODO(M4): FrostIDE.MSBuildLogger for structured events, binary logs, and the per-instance
 * vcvars environment for tools that need it.
 */
#pragma once

#include "frost/build/build_session.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::build {

// Compile is the single-file compile (header comment); it is the last value so the others keep theirs.
enum class BuildAction : std::uint8_t { Build, Rebuild, Clean, Compile };

// "Build", "Rebuild", "Clean" or "Compile": the action's name in status lines. The MSBuild target
// is the name for the first three and "ClCompile" for Compile (msbuildTargetName).
[[nodiscard]] std::string_view toString(BuildAction action) noexcept;

// The MSBuild target an action runs on a project or solution file: "Build", "Rebuild", "Clean" or
// "ClCompile".
[[nodiscard]] std::string_view msbuildTargetName(BuildAction action) noexcept;

// MSBuild's -v levels.
enum class MsBuildVerbosity : std::uint8_t { Quiet, Minimal, Normal, Detailed, Diagnostic };

struct MsBuildRequest {
    std::string msbuildPath{};  // Absolute path of MSBuild.exe (VsInstance::msbuildPath or the user's override).
    std::string solutionPath{}; // Absolute .sln or .slnx; may be empty only when projectPath is set.
    // One project of the solution, by its name in the solution, solution folders included
    // ("Folder\Lib"); empty builds the whole solution. Excludes projectPath.
    std::string projectName{};
    // A project file to build directly (absolute); the Solution* properties come from solutionPath
    // when it is set. Excludes projectName.
    std::string projectPath{};
    std::string configuration{}; // "Debug"; required.
    std::string platform{};      // "x64", "Win32", "Any CPU"; required.
    BuildAction action = BuildAction::Build;
    // The user trusts the folder whose code the build runs; false refuses with Untrusted.
    bool trusted = false;
    unsigned maxCpuCount = 0; // -m:N; 0 gives plain -m (one node per CPU).
    MsBuildVerbosity verbosity = MsBuildVerbosity::Minimal;
    // Extra global properties, passed as -p:Name=Value with Value escaped for MSBuild.
    std::vector<std::pair<std::string, std::string>> properties{};
    // DOTNET_CLI_UI_LANGUAGE; empty means "en-US" (header comment). Only en-US also sets VSLANG=1033.
    std::string uiLanguage{};
    // Compile only: the ClCompile items' Identities to compile (header comment), passed as one
    // -p:SelectedFiles list. Required (non-empty) for Compile, refused for the other actions.
    std::vector<std::string> selectedFiles{};
    std::string loggerAssembly{}; // Shipped ProjectHostVsProbe.exe containing FrostBuildLogger; absolute.
    std::string binaryLogPath{};  // Absolute artifact path; empty disables binary logging.
};

// The target name MSBuild gives a project of a solution: `name` with each of % $ @ ; . ( ) '
// replaced by '_' ("My.Lib" -> "My_Lib"); solution folder separators stay.
[[nodiscard]] std::string solutionTargetName(std::string_view name);

// `value` escaped for an MSBuild -p switch: % ; , become %25 %3B %2C.
[[nodiscard]] std::string escapeMsBuildValue(std::string_view value);

/*
 * The process spec for `request` (header comment), without _MSPDBSRV_ENDPOINT_. Pure and portable:
 * paths follow Windows rules on every platform. Errors: InvalidArgument (no solution or project,
 * both projectName and projectPath, a path that is not absolute, an empty configuration or
 * platform, a property name MSBuild would refuse; Compile without projectPath or without
 * selectedFiles, or with an empty one; selectedFiles for another action).
 */
[[nodiscard]] Result<io::ProcessSpec> msbuildProcessSpec(const MsBuildRequest& request);

// The image names startMsBuild's session ends after MSBuild exits: MSBuild.exe (worker nodes),
// MSBuildTaskHost.exe, mspdbsrv.exe and VBCSCompiler.exe.
[[nodiscard]] std::vector<std::string> msbuildInfrastructureImages();

/*
 * Starts the build: msbuildProcessSpec plus a unique _MSPDBSRV_ENDPOINT_, run as a BuildSession.
 * The clock must outlive the session. Errors: Untrusted (request.trusted is false; checked first,
 * on every platform); Unsupported (not Windows); those of msbuildProcessSpec; the launcher's
 * (NotFound when MSBuild.exe is gone).
 */
[[nodiscard]] Result<std::unique_ptr<BuildSession>> startMsBuild(io::IProcessLauncher& launcher,
                                                                 const MsBuildRequest& request,
                                                                 BuildCallbacks callbacks,
                                                                 IClock& clock = systemClock());

} // namespace frost::build
