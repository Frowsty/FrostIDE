// SPDX-License-Identifier: Apache-2.0
//
// Compile commands from MSBuild's own evaluation of each project (W2, ADR 0041): the provider
// between an existing compile_commands.json and the static synthesizer, so property sheets
// (.props), $(SDK) variables from the environment, per-file metadata and AdditionalOptions
// reach clangd exactly as MSBuild sees them. Windows only; trusted solutions only.
/*
 * Evaluation (msbuildEvaluationSpec). For each project of the active solution configuration:
 *
 *     MSBuild.exe <project> -nologo -nodeReuse:false -getItem:ClCompile
 *                 -getProperty:IncludePath;ExternalIncludePath;MSBuildAllProjects;...
 *                 -p:Configuration=<c> -p:Platform=<p> -p:DesignTimeBuild=true
 *                 -p:SolutionDir=<dir\> -p:SolutionPath=... -p:SolutionName=...
 *                 -p:SolutionFileName=... -p:SolutionExt=...
 *
 * MSBuild evaluates (no target runs) and prints JSON: the properties and every ClCompile item
 * with its evaluated metadata (ItemDefinitionGroups from the project and its imports applied).
 * MSBuild is started through IProcessLauncher with the absolute path
 * the caller found (frost::build's discovery; lang cannot depend on build, so it is injected),
 * with DOTNET_CLI_UI_LANGUAGE=en-US (UTF-8 output), MSBUILDTERMINALLOGGER=off and
 * MSBUILDDISABLENODEREUSE=1, in the project's directory, within `timeout` per run.
 *
 * Commands (commandsFromEvaluation), per ClCompile item not ExcludedFromBuild, in clang-cl's
 * --driver-mode=cl syntax: --target from the platform; RuntimeLibrary -> /MT /MTd /MD /MDd;
 * ExceptionHandling Sync -> /EHsc, Async -> /EHa, SyncCThrow -> /EHs, false -> nothing;
 * ConformanceMode=true -> /permissive-; TreatWChar_tAsBuiltInType=false -> /Zc:wchar_t-;
 * CompileAs CompileAsC -> /TC, CompileAsCpp -> /TP; PreprocessorDefinitions -> /D;
 * UndefinePreprocessorDefinitions -> /U; AdditionalIncludeDirectories -> /I (relative entries
 * against the project directory); the IncludePath and ExternalIncludePath properties (the MSVC
 * and Windows SDK directories, as the project's toolset sets them) -> /imsvc; ForcedIncludeFiles
 * -> /FI; PrecompiledHeader=Use -> /FI<PrecompiledHeaderFile> (so files that rely on the PCH still
 * see its declarations); LanguageStandard / LanguageStandard_C -> /std:...; AdditionalOptions
 * passed through, except the PCH, output and parallel-build switches (/Y*, /Fp*, /Fo*, /Fd*,
 * /MP*), which clangd must not act on; then /c and the file.
 *
 * Every locate() evaluates afresh. Prior import lists cannot prove that environment variables,
 * newly created imports, glob matches or property functions have remained unchanged. The generated
 * compilation database is written in the cache directory for clangd to consume.
 *
 * Fallback and trust. Evaluation runs project logic (property functions, conditions, imports),
 * so it needs a trusted solution: when `trusted` is unset or says no, locate() returns nullopt and
 * the chain's static synthesizer answers. A project whose evaluation fails (MSBuild missing, an
 * error, a timeout, unreadable JSON) gets the static synthesizer's commands instead, with a
 * warning in the log and in warnings(). Off Windows locate() returns nullopt.
 *
 * Threads: locate() blocks (MSBuild runs); ClangdService calls it on the Background lane. One
 * caller at a time.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/msvc_environment.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::lang {

// One evaluated ClCompile item: its full path and its metadata (names lower-cased).
struct EvaluatedItem {
    std::string fullPath;
    std::map<std::string, std::string, std::less<>> metadata;
};

// What MSBuild's evaluation query printed (property names lower-cased).
struct ProjectEvaluation {
    std::map<std::string, std::string, std::less<>> properties;
    std::vector<EvaluatedItem> clCompile;
};

// Reads the -getItem/-getProperty JSON (text before the first '{' is skipped). Errors: Corrupt.
[[nodiscard]] Result<ProjectEvaluation> parseMsBuildEvaluation(std::string_view output);

// The properties the evaluation asks for.
[[nodiscard]] std::vector<std::string> evaluatedPropertyNames();

// What one evaluation run needs.
struct MsBuildEvaluationRequest {
    std::string msbuildPath;   // Absolute MSBuild.exe.
    std::string projectPath;   // Absolute .vcxproj.
    std::string solutionPath;  // Absolute .sln/.slnx (for the Solution* properties); may be empty.
    std::string configuration; // The project configuration ("Debug").
    std::string platform;      // The project platform ("x64").
    std::string preprocessTo;  // Non-empty: a -pp:<file> run instead of the query.
    std::vector<std::pair<std::string, std::optional<std::string>>> environment; // Added to the child's.
};

// The process spec of one run (see the header comment). Errors: InvalidArgument (paths not
// absolute, empty configuration or platform).
[[nodiscard]] Result<io::ProcessSpec> msbuildEvaluationSpec(const MsBuildEvaluationRequest& request);

// The imported files named by the banners of a -pp output, in order, without duplicates.
// MSBuild writes "--" in a path as "__" inside its XML comments; such paths are returned as
// written (MsBuildCompileCommandsProvider repairs them against the disk).
[[nodiscard]] std::vector<std::string> parsePreprocessedImports(std::string_view preprocessed);

// The clang-cl commands of an evaluated project (see the header comment). Pure.
[[nodiscard]] std::vector<CompileCommand> commandsFromEvaluation(const ProjectEvaluation& evaluation,
                                                                 std::string_view projectDirectory,
                                                                 std::string_view configuration,
                                                                 std::string_view platform,
                                                                 std::string_view compiler = "clang-cl");

struct MsBuildProviderOptions {
    // The solution configuration ("Debug|x64", ASCII case ignored); empty selects the first.
    std::string configuration;
    // Finds MSBuild.exe (an absolute path); called once per locate() when msbuildPath is empty.
    std::string msbuildPath;
    std::function<Result<std::string>()> locateMsBuild;
    // Live trust of the solution folder; evaluation runs only when it returns true.
    std::function<bool()> trusted;
    // The limit of one MSBuild run.
    IClock::Duration timeout = std::chrono::seconds(60);
    // Extra environment for MSBuild (tests set SDK roots here instead of in this process).
    std::vector<std::pair<std::string, std::optional<std::string>>> environment;
    std::string compiler = "clang-cl";
    std::shared_ptr<GeneratedCommandsRetirement> retirement;
    // The static synthesizer for projects whose evaluation fails, and its MSVC lookup.
    SynthesisOptions fallback;
    std::function<Result<MsvcEnvironment>()> msvc;
};

// What the last locate() did, for the log and tests.
struct MsBuildProviderStats {
    unsigned evaluated = 0; // Projects MSBuild evaluated (cache misses).
    unsigned cached = 0;    // Projects taken from the cache.
    unsigned fellBack = 0;  // Projects that got the static synthesizer's commands.
};

class MsBuildCompileCommandsProvider final : public ICompileCommandsProvider {
public:
    // `launcher`, `fs` and `model` must outlive the provider. Commands go to `cacheDirectory`.
    MsBuildCompileCommandsProvider(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                   std::shared_ptr<const project::SolutionModel> model, std::string cacheDirectory,
                                   MsBuildProviderOptions options, std::string cleanupBoundary = {},
                                   std::vector<std::string> cleanupParents = {});

    // See the header comment. Errors: the configuration's (NotFound, InvalidArgument), those of
    // locateMsBuild (the chain then falls back), the cache directory's I/O errors.
    [[nodiscard]] Result<std::optional<CompileCommandsLocation>> locate() override;
    [[nodiscard]] Result<std::optional<CompileCommandsLocation>> locate(std::stop_token stop) override;

    [[nodiscard]] const MsBuildProviderStats& lastStats() const noexcept { return m_stats; }
    // The warnings of the last locate() (one per project that fell back).
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return m_warnings; }

private:
    // locate() on Windows.
    [[nodiscard]] Result<std::optional<CompileCommandsLocation>> evaluateAll(std::stop_token stop);

    io::IProcessLauncher& m_launcher;
    io::IFileSystem& m_fs;
    std::shared_ptr<const project::SolutionModel> m_model;
    std::string m_cacheDirectory;
    std::shared_ptr<GeneratedCommandsLease> m_lease;
    MsBuildProviderOptions m_options;
    bool m_msvcLooked = false;
    MsBuildProviderStats m_stats;
    std::vector<std::string> m_warnings;
};

/*
 * The provider order of ADR 0041 for a solution: an existing compile_commands.json under
 * `root`, then MSBuild's evaluation (trusted solutions on Windows), then the static synthesizer.
 * Both FrostIDE-written databases go below `cacheDirectory` (evaluated/ and synthesized/).
 */
[[nodiscard]] std::shared_ptr<ICompileCommandsProvider>
solutionCompileCommandsChain(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                             std::shared_ptr<const project::SolutionModel> model, std::string root,
                             std::string cacheDirectory, MsBuildProviderOptions options);

} // namespace frost::lang
