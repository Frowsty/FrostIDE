// SPDX-License-Identifier: Apache-2.0
//
// BuildService: the Qt-free W1 build service that the Build menu, the toolbar's configuration
// combo, the build output window, the error list and the editor's marks bind to (ADR 0032,
// ADR 0035). It holds the open solution and its active configuration, runs one MSBuild build at
// a time with the trust gate and Save All before it, and keeps the last build's diagnostics.
//
/*
 * Active configuration. When a solution opens, the active solution configuration
 * (Configuration|Platform) is the one persisted for that solution path, if the solution still
 * has it; else the first whose configuration name starts with "Debug" (ASCII case ignored); else
 * the first. setActiveConfiguration persists the choice in a small UTF-8 text file
 * (BuildServiceOptions::stateFilePath; defaultStatePath() puts it beside the trust store): a
 * header line "# FrostIDE active configurations v1", then "<solution path>\t<Configuration|Platform>"
 * per solution. Lines that do not parse are skipped. A newer version or a missing header leaves
 * the file alone (nothing is restored, saving fails with Unsupported or Corrupt).
 *
 * A build. build(action, project) checks the request on the calling thread (a solution is open,
 * nothing else is building, the project builds in the active configuration) and then runs it
 * through these states on the service's thread:
 *
 *     Idle -> [NeedsTrust ->] SavingDocuments -> Starting -> Running [-> Cancelling] -> Idle
 *
 *   - NeedsTrust: project::checkBuildTrust failed, so the build would run project code from a
 *     folder the user has not trusted. onNeedsTrust lists the untrusted folders of
 *     project::buildTrustRoots; trust(root) trusts one and, once all are trusted, the build goes
 *     on; cancel() drops it.
 *   - SavingDocuments: the Save-All hook (setSaveAllHook) is called with a completion and the
 *     build waits until the UI calls it. A failure ends the build without starting MSBuild
 *     (BuildOutcome::error). Without a hook this state passes at once.
 *   - Starting: MSBuild is found (BuildServiceOptions::msbuildPath, else vswhere on the first
 *     build; cached) and started on the solution with the active configuration: the whole solution,
 *     or one project through the solution's own per-project target, named as MSBuild names it
 *     (solutionProjectName: "Folder\Sub\Name"), so the solution's configuration mapping and
 *     build order apply.
 *   - Running: output lines, diagnostics and progress arrive; cancel() ends the build's process
 *     tree (Cancelling until MSBuild is gone).
 * Compile (compile(), Visual Studio's Ctrl+F7, SLN-9) runs the same pipeline for one source file:
 * the project that compiles it is the first of SolutionModel::projectsContaining whose item at that
 * path is a ClCompile item and that the active configuration builds (compileProjectFor). In
 * Starting, MSBuild is first asked for that project's ClCompile items in the mapped project
 * configuration (msbuild_compile.hpp; BuildServiceOptions::compileItems) and the file's evaluated
 * Identity becomes SelectedFiles of a Compile request on the project file, with the solution's
 * Solution* properties; a file MSBuild does not list ends the build with NotFound before anything
 * compiles (never a whole-project compile). Nothing else is built (no project references).
 * One build at a time: build() while a build is anywhere in that pipeline fails with Busy, and
 * nothing is queued; the UI greys the commands out while state() is not Idle. Switching the
 * configuration or the solution is refused with Busy during a build too.
 *
 * Diagnostics. A new build clears the previous one's (onDiagnosticsCleared). Each diagnostic is
 * resolved to an absolute path and a jump decision (frost/build/build_diagnostics.hpp) and
 * indexed: diagnostics(), diagnosticsByFile(), diagnosticCounts(), diagnosticsForDocument().
 *
 * Progress. At minimal verbosity MSBuild names each project when it produced its output
 * ("Lib.vcxproj -> C:\...\Lib.lib"). onProgress counts those against the projects the build is
 * expected to build (those the configuration builds; for one project, it and its dependencies).
 * Projects without an output (Clean, utility projects) are not counted, and started projects are
 * not reported at all. TODO(M4): exact per-project events through FrostIDE.MSBuildLogger.
 *
 * Output. onOutput passes MSBuild's lines (Stdout, Stderr) and the service's own status lines
 * (Status: the line naming what starts, and the summary at the end).
 *
 * Threads. Every member may be called from any thread unless noted. Callbacks run on the
 * service's own thread, never on the caller's and never two at a time, in the order the events
 * happened; the UI marshals what it needs to its thread. A callback may call every member except
 * the destructor and waitForIdle. The Save-All hook is called on the service's thread too, and its
 * completion may be called from any thread, once (later calls are ignored). Queries return
 * snapshots.
 *
 * Lifetime. The file system, launcher and trust store must outlive the service. The destructor
 * cancels a running build, waits for its processes to end and stops the service's thread;
 * callbacks still queued then are dropped.
 */
#pragma once

#include "frost/build/build_diagnostics.hpp"
#include "frost/build/build_session.hpp"
#include "frost/build/msbuild_compile.hpp"
#include "frost/build/msbuild_driver.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/project/solution_model.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
class IProcessLauncher;
} // namespace frost::io

namespace frost::project {
class TrustStore;
} // namespace frost::project

namespace frost::build {

enum class BuildState : std::uint8_t {
    Idle,            // No build; build() is accepted.
    NeedsTrust,      // Waiting for trust(); see untrustedRoots().
    SavingDocuments, // The Save-All hook runs.
    Starting,        // Finding and launching MSBuild.
    Running,         // MSBuild runs.
    Cancelling,      // cancel() was called; MSBuild's process tree is ending.
};

// "Idle", "NeedsTrust", "SavingDocuments", "Starting", "Running" or "Cancelling".
[[nodiscard]] std::string_view toString(BuildState state) noexcept;

// What a build builds.
struct BuildTarget {
    BuildAction action = BuildAction::Build;
    std::optional<project::ProjectId> project{};    // Empty: the whole solution.
    std::string projectName{};                      // solutionProjectName of `project`; "" for the solution.
    project::SolutionConfiguration configuration{}; // The solution configuration MSBuild gets.
    // What `project` builds as in `configuration` (the solution's mapping); empty for the solution.
    std::optional<project::ProjectConfigurationMapping> projectConfiguration{};
    std::string file{}; // Compile: the source file (absolute); "" for the other actions.
};

struct BuildProgress {
    std::size_t finishedProjects = 0; // Projects that reported their output so far.
    std::size_t expectedProjects = 0; // Projects the build should build (header comment).
    std::string lastProject{};        // The project that reported last ("Lib"), or "".
    std::string lastOutput{};         // Its output path.

    friend bool operator==(const BuildProgress&, const BuildProgress&) = default;
};

// How a build ended.
struct BuildOutcome {
    BuildTarget target{};
    // MSBuild's result; for a build that never started, success = false (and cancelled = true
    // after a cancel()).
    BuildResult result{};
    std::optional<Error> error{}; // Why the build did not start (Save All failed, no MSBuild, a launch error).
    DiagnosticCounts counts{};    // Over the build's diagnostics.

    [[nodiscard]] bool succeeded() const noexcept { return result.success && !error; }
};

enum class BuildOutputKind : std::uint8_t { Stdout, Stderr, Status };

// Called once when one build() ends, right after onFinished, on the service's thread.
using BuildCompletion = std::function<void(const BuildOutcome& outcome)>;
// Called once by the Save-All hook when saving is done; a failure aborts the build.
using SaveAllCompletion = std::function<void(Result<void> saved)>;
// The UI's Save All (see the header comment for the thread it is called on).
using SaveAllHook = std::function<void(SaveAllCompletion done)>;
// Starts one MSBuild build; the default is startMsBuild with the service's launcher. Tests and
// other drivers substitute their own.
using BuildStarter =
    std::function<Result<std::unique_ptr<BuildSession>>(const MsBuildRequest& request, BuildCallbacks callbacks)>;
// Lists a project configuration's ClCompile items for Compile; the default is queryCompileItems with
// the service's launcher (a two-minute limit). Tests substitute their own.
using CompileItemSource = std::function<Result<std::vector<CompileItem>>(const RunPropertyRequest& request)>;

// Every callback is optional and runs on the service's thread (header comment).
struct BuildServiceCallbacks {
    std::function<void(BuildState state)> onStateChanged{};
    // The folders to ask the user about (state NeedsTrust); again after each trust() that left some.
    std::function<void(const std::vector<std::string>& untrustedRoots)> onNeedsTrust{};
    std::function<void(BuildOutputKind kind, std::string_view line)> onOutput{};
    std::function<void(const BuildDiagnostic& diagnostic)> onDiagnostic{};
    std::function<void()> onDiagnosticsCleared{};
    std::function<void(const BuildProgress& progress)> onProgress{};
    std::function<void(const BuildOutcome& outcome)> onFinished{}; // Before the state returns to Idle.
    std::function<void(const project::SolutionConfiguration& configuration)> onActiveConfigurationChanged{};
    // Optional per-event GUI marshalling fence; the producer supplies the originating job identity.
    std::function<void(BuildOutputKind, std::string_view, std::function<bool()>)> onOutputFenced{};
    std::function<void(const BuildDiagnostic&, std::function<bool()>)> onDiagnosticFenced{};
    std::function<void(const BuildOutcome&, std::function<bool()>)> onFinishedFenced{};
};

struct BuildServiceOptions {
    std::string stateFilePath{};      // The persisted active configurations (absolute); "" persists nothing.
    std::string msbuildPath{};        // Absolute MSBuild.exe; "" finds one with vswhere at the first build.
    std::string vsInstanceId{};       // The Visual Studio instance vswhere picks (MsBuildDiscoveryOptions).
    unsigned maxCpuCount = 0;         // MsBuildRequest::maxCpuCount.
    std::string loggerAssembly{}; // Packaged FrostBuildLogger; empty retains console parser.
    std::string binaryLogDirectory{}; // Private absolute artifact directory; one binlog per build.

    BuildStarter startBuild{};        // "" uses startMsBuild.
    CompileItemSource compileItems{}; // "" uses queryCompileItems.
};

// MSBuild's name for project `id` of `solution`: the names of its solution folders and its own,
// joined with '\' ("Engine\Core\Core"), as MSBuild's solution targets are named (the driver then
// replaces the characters MSBuild does). Precondition (FROST_PRE): id < solution.projects().size().
[[nodiscard]] std::string solutionProjectName(const project::SolutionModel& solution, project::ProjectId id);

class BuildService {
public:
    // Creates the service and starts its thread (header comment for the references' lifetimes).
    [[nodiscard]] static std::unique_ptr<BuildService> create(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                              project::TrustStore& trustStore,
                                                              BuildServiceOptions options = {},
                                                              BuildServiceCallbacks callbacks = {});

    // "active-configurations.txt" in the trust store's directory (project::TrustStore::defaultStorePath).
    // Errors: those of defaultStorePath.
    [[nodiscard]] static Result<std::string> defaultStatePath();

    ~BuildService();
    BuildService(const BuildService&) = delete ("owns a thread and a build");
    BuildService& operator=(const BuildService&) = delete ("owns a thread and a build");

    /*
     * Makes `solution` the open solution, chooses its active configuration (header comment) and
     * clears the diagnostics. Reads the state file (small; blocking). Errors: InvalidArgument (null),
     * Busy (a build is in progress).
     */
    [[nodiscard]] Result<void> openSolution(std::shared_ptr<const project::SolutionModel> solution);
    // Closes the open solution, if any, and clears the diagnostics. Errors: Busy (a build is in progress).
    [[nodiscard]] Result<void> closeSolution();
    [[nodiscard]] std::shared_ptr<const project::SolutionModel> solution() const;

    // The choices for the configuration combo: the solution's configurations in solution order;
    // empty without a solution.
    [[nodiscard]] std::vector<project::SolutionConfiguration> configurations() const;
    [[nodiscard]] std::optional<std::size_t> activeConfigurationIndex() const;
    [[nodiscard]] std::optional<project::SolutionConfiguration> activeConfiguration() const;
    /*
     * Makes configurations()[index] active and persists it (a blocking write of the small state
     * file). The choice applies even when saving fails; the error is returned. Errors:
     * InvalidArgument (no solution), OutOfRange, Busy (a build is in progress), the file errors.
     */
    [[nodiscard]] Result<void> setActiveConfiguration(std::size_t index);
    // The same by name, "Debug|x64" (ASCII case ignored). Errors: as above; NotFound for an unknown name.
    [[nodiscard]] Result<void> setActiveConfiguration(std::string_view name);

    // Installs the UI's Save All (empty: none). Takes effect for builds that have not reached
    // SavingDocuments yet.
    void setSaveAllHook(SaveAllHook hook);

    /*
     * Starts `action` for the whole solution (no project) or for one project of it, with the
     * active configuration; returns once the request is accepted (the build runs on). `onDone`
     * (optional) is called once with this build's outcome, right after onFinished, also when it is
     * cancelled before MSBuild starts; it is dropped if the service is destroyed first. The run
     * service uses it to start the program after the build (ADR 0040). Errors: InvalidArgument (no
     * solution open, no configuration, or `project` is out of range, has no file or is
     * unavailable), Unsupported (the project is not built in the active configuration), Busy
     * (another build is in progress).
     */
    [[nodiscard]] Result<void> build(BuildAction action, std::optional<project::ProjectId> project = std::nullopt,
                                     BuildCompletion onDone = {});

    /*
     * The project a Compile of `filePath` (absolute) uses in the active configuration (header
     * comment, "Compile"), or nullopt: no solution or configuration, or no project the active
     * configuration builds has a ClCompile item at that path. For the command's enablement. O(1)
     * plus the projects containing the file.
     */
    [[nodiscard]] std::optional<project::ProjectId> compileProjectFor(std::string_view filePath) const;

    /*
     * Starts a Compile of the source file `filePath` (absolute) in the active configuration
     * (header comment, "Compile"); `onDone` as for build(). Returns once the request is accepted.
     * Errors: InvalidArgument (no solution open, no configuration, a relative path), NotFound
     * (no project the active configuration builds compiles the file), Busy (a build or project
     * edit is in progress). Later failures (MSBuild does not list the file) end the build with the
     * outcome's error.
     */
    [[nodiscard]] Result<void> compile(std::string_view filePath, BuildCompletion onDone = {});

    // Reserves the open solution against builds and configuration/project changes while its
    // definition files are edited. finishProjectEdit publishes a replacement before releasing it.
    [[nodiscard]] Result<void> beginProjectEdit();
    [[nodiscard]] Result<void> finishProjectEdit(std::shared_ptr<const project::SolutionModel> solution);
    void cancelProjectEdit();
    [[nodiscard]] bool projectEditInProgress() const;
    [[nodiscard]] Result<void> reserveRun();
    void releaseRun();
    // Reserve against builds, runs and project edits during an external debugger handoff.
    // Release is generation-checked, so an old completion cannot release a successor.
    [[nodiscard]] bool externalLaunchAdmissionBusy() const;
    [[nodiscard]] Result<std::uint64_t> reserveExternalLaunch();
    void releaseExternalLaunch(std::uint64_t token);
    [[nodiscard]] Result<std::uint64_t> reserveExternalChild(std::uint64_t owner);
    void releaseExternalChild(std::uint64_t owner, std::uint64_t child);

    // Cancels the build in progress, if any: before MSBuild starts it ends at once (cancelled);
    // while MSBuild runs, its process tree is ended. Idempotent.
    void cancel();

    /*
     * Trusts `root` in the trust store and, when the build waiting in NeedsTrust has no untrusted
     * folder left, lets it go on. Works in any state (the trust is stored). Errors: those of
     * TrustStore::trust.
     */
    [[nodiscard]] Result<void> trust(std::string_view root);

    [[nodiscard]] BuildState state() const;
    // The folders a build waiting in NeedsTrust needs trusted; empty in other states.
    [[nodiscard]] std::vector<std::string> untrustedRoots() const;
    // What the build in progress builds; empty when Idle.
    [[nodiscard]] std::optional<BuildTarget> currentBuild() const;
    [[nodiscard]] BuildProgress progress() const;
    // How the last build ended; empty before the first one ends (or after the solution changed).
    [[nodiscard]] std::optional<BuildOutcome> lastOutcome() const;

    // The current (or last) build's diagnostics in arrival order.
    [[nodiscard]] std::vector<BuildDiagnostic> diagnostics() const;
    [[nodiscard]] std::vector<DiagnosticFileGroup> diagnosticsByFile() const;
    [[nodiscard]] DiagnosticCounts diagnosticCounts() const;
    // The diagnostics reported for the document at `path` (absolute), for the editor's marks.
    [[nodiscard]] std::vector<BuildDiagnostic> diagnosticsForDocument(std::string_view path) const;

    // Blocks until the state is Idle and every callback queued so far has run, or `timeout`
    // passes; returns whether it got there. Not from a callback.
    [[nodiscard]] bool waitForIdle(IClock::Duration timeout) const;

    struct State; // Shared with the service's thread and the build's callbacks; defined in the implementation.

private:
    explicit BuildService(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> m_state;
};

} // namespace frost::build
