// SPDX-License-Identifier: Apache-2.0
//
// RunService: the Qt-free W1 run service (ADR 0040) that Run, Stop, Rerun, the toolbar's startup
// project combo and the Run console bind to. It keeps the solution's startup project, builds it
// through the build service, asks MSBuild what to start (run_target.hpp) and runs the program in
// FrostIDE's Run console or an external console window, one run at a time per project. A Debug
// run (debug()) builds and resolves the same way and then hands the program to the debugger
// (frost::debug, ADR 0050) instead of starting it.
//
/*
 * Startup project. When a solution opens, the startup project is the one persisted for that
 * solution, if it still has that project; else the first project in solution order whose
 * ConfigurationType is Application (the active configuration's, else the project's first);
 * else the first loaded C++ project; else the first project. setStartupProject persists the
 * choice per solution in a small UTF-8 text file (RunServiceOptions::stateFilePath;
 * defaultStatePath() puts it beside the build service's active configurations): a header line
 * "# FrostIDE run settings v1", then "<solution>\t<startup project file>\t<console>" per
 * solution, where <console> is "external" or "run-console" (setUseExternalConsole). Lines that do
 * not parse are skipped; a newer version or a missing header leaves the file alone.
 *
 * A run. run(project) checks the request on the calling thread and then runs it through these
 * states on the service's thread:
 *
 *     Building -> Resolving -> Running [-> Stopping] -> Finished
 *
 *   - Building: IRunBuilder builds the project in the active configuration (BuildService: trust
 *     prompt, Save All, MSBuild). A build that fails or is cancelled ends the run without starting
 *     anything, like Visual Studio and Rider. A project the configuration does not build is not
 *     built; the run goes on with its last build (after the trust check).
 *   - Resolving: the project's TargetPath and debugger settings for the solution configuration's
 *     mapping (run_target.hpp: MSBuild -getProperty, with the static fallback when MSBuild is
 *     older than 17.8), cached per project file and configuration until the .vcxproj or
 *     .vcxproj.user changes on disk; then the program's PE subsystem picks the console mode: GUI
 *     programs run with no console, console programs in an external console window when
 *     useExternalConsole() (the default, since they are often interactive), else in the Run
 *     console. This step blocks the service's thread for the query (about a second).
 *   - Running: output, input and Stop (the whole process tree ends). Then Finished, with the exit
 *     code and the run time.
 *   - A Debug run (debug()) skips Running: once resolved it is Finished with outcome.debugged and
 *     its DebugRunHandlers get the DebugLaunch, which the debugger starts under lldb-dap.
 * One run per project: run() for a project that is already running fails with Busy, which the UI
 * turns into "stop and rerun?" (rerun() does that). Different projects may run side by side.
 * run() also fails with Busy while the build service builds. Off Windows, MSBuild solutions are
 * Unsupported (CMake runs come with M2/M3).
 *
 * Output. onOutput passes the program's stdout and stderr (UTF-8 chunks, not lines) and the
 * service's own System lines (what starts, a skipped build, why a run did not start).
 *
 * Threads. Every member may be called from any thread unless noted. Callbacks run on the
 * service's own thread, never two at a time, in event order; the UI marshals to its thread. A
 * callback may call every member except the destructor and waitForIdle. Queries return snapshots.
 *
 * Lifetime. The file system, launcher, trust store and builder must outlive the service. The
 * destructor stops every running program, waits for it to end and stops the service's thread;
 * callbacks still queued then are dropped.
 */
#pragma once

#include "frost/build/build_service.hpp"
#include "frost/build/run_session.hpp"
#include "frost/build/run_target.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/project/solution_model.hpp"

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

// What RunService needs from the build side; makeRunBuilder adapts BuildService, tests fake it.
// Thread-safe. Owned through std::unique_ptr, hence the public virtual destructor.
class IRunBuilder {
public:
    virtual ~IRunBuilder() = default;

    // The index of the open solution's active configuration, if any.
    [[nodiscard]] virtual std::optional<std::size_t> activeConfigurationIndex() const = 0;
    // Whether a build is in progress (run() then fails with Busy).
    [[nodiscard]] virtual bool building() const = 0;
    /*
     * Starts a Build of `project` in the active configuration; `done` is called once with the
     * outcome, on any thread, after this call returned. Errors: Unsupported (the configuration
     * does not build the project; the run goes on without a build), Busy, InvalidArgument.
     */
    [[nodiscard]] virtual Result<void> buildForRun(project::ProjectId project, BuildCompletion done) = 0;
    // Cancels the build buildForRun started, if it still runs.
    virtual void cancelBuild() = 0;

protected:
    IRunBuilder() = default;
    IRunBuilder(const IRunBuilder&) = default;
    IRunBuilder& operator=(const IRunBuilder&) = default;
};

// IRunBuilder over `service`, which must outlive the result.
[[nodiscard]] std::unique_ptr<IRunBuilder> makeRunBuilder(BuildService& service);

using RunId = std::uint64_t;

enum class RunState : std::uint8_t { Building, Resolving, Running, Stopping, Finished };

// "Building", "Resolving", "Running", "Stopping" or "Finished".
[[nodiscard]] std::string_view toString(RunState state) noexcept;

// One run, as the UI shows it.
struct RunInfo {
    RunId id = 0;
    project::ProjectId project{};
    std::string projectName{};
    RunState state = RunState::Building;
    project::SolutionConfiguration configuration{}; // The solution configuration it runs in.
    ConsoleMode mode = ConsoleMode::RunConsole;     // Known from Running on.
    std::string commandLine{};                      // Known from Running on.
    io::ProcessId processId = 0;                    // Known from Running on.
    bool acceptsInput = false;                      // The Run console's input line is live.
    bool debug = false;                             // A Debug run (debug()): the Run console ignores it.
};

// How a run ended.
struct RunOutcome {
    RunInfo run{};
    std::optional<Error> error{};  // Why nothing ran: the build failed, or resolving or launching failed.
    bool buildFailed = false;      // The build before the run failed or was cancelled.
    bool stopped = false;          // stop() ended the program (or the run before it started).
    std::optional<int> exitCode{}; // When the program ran.
    IClock::Duration duration{};   // The program's run time.
    bool debugged = false;         // A Debug run handed its program to the debugger.

    [[nodiscard]] bool ran() const noexcept { return exitCode.has_value(); }
};

enum class RunOutputKind : std::uint8_t { Stdout, Stderr, System };

// Every callback is optional and runs on the service's thread (header comment).
struct RunServiceCallbacks {
    std::function<void(const RunInfo& run)> onRunStarted{};      // run() accepted it (Building).
    std::function<void(const RunInfo& run)> onRunStateChanged{}; // Resolving, Running, Stopping.
    std::function<void(RunId run, RunOutputKind kind, std::string_view text)> onOutput{};
    std::function<void(const RunOutcome& outcome)> onRunFinished{};
    std::function<void(std::optional<project::ProjectId> project)> onStartupProjectChanged{};
    std::function<void(bool external)> onExternalConsoleChanged{};
};

// What a Debug run hands to the debugger: the resolved program and the folders it runs project code from.
struct DebugLaunch {
    RunCommand command{};
    Subsystem subsystem = Subsystem::Unknown;
    std::vector<std::string> trustRoots{}; // project::buildTrustRoots of the solution.
};

// The callbacks of one Debug run; both optional, on the service's thread, after the service's own
// callbacks for the same event.
struct DebugRunHandlers {
    std::function<void(RunOutputKind kind, std::string_view text)> onOutput{};
    // The run ended: `launch` is set when the program is ready to debug, else outcome says why not.
    std::function<void(const RunOutcome& outcome, const std::optional<DebugLaunch>& launch)> onFinished{};
};

// Asks what a project configuration runs; the default asks MSBuild, with the static fallback.
using RunPropertySource = std::function<Result<RunProperties>(const RunPropertyRequest& request)>;

struct RunServiceOptions {
    std::string stateFilePath{};    // The persisted run settings (absolute); "" persists nothing.
    std::string msbuildPath{};      // Absolute MSBuild.exe for the property query; "" finds one with vswhere.
    std::string vsInstanceId{};     // The Visual Studio instance vswhere picks.
    std::string cmdPath{};          // Absolute cmd.exe for the external console; "" uses systemCommandProcessor().
    RunPropertySource properties{}; // Empty: queryRunProperties, then staticRunProperties on Unsupported.
    project::EnvironmentLookup environment{}; // For expansion; empty: the process environment.
    // Whether MSBuild projects can run here: Windows only by default (tests set it elsewhere).
    bool msbuildRunSupported = io::kWindowsProcessRules;
    bool externalConsoleByDefault = true; // For solutions without a persisted choice.
    RunSessionOptions session{};
};

class RunService {
public:
    // Creates the service and starts its thread (header comment for the references' lifetimes).
    [[nodiscard]] static std::unique_ptr<RunService> create(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                            project::TrustStore& trustStore, IRunBuilder& builder,
                                                            RunServiceOptions options = {},
                                                            RunServiceCallbacks callbacks = {});

    // "run-settings.txt" beside BuildService::defaultStatePath(). Errors: those of that path.
    [[nodiscard]] static Result<std::string> defaultStatePath();

    ~RunService();
    RunService(const RunService&) = delete ("owns a thread and the runs");
    RunService& operator=(const RunService&) = delete ("owns a thread and the runs");

    /*
     * Makes `solution` the open solution and picks its startup project (header comment). Reads
     * the state file (small; blocking). Errors: InvalidArgument (null), Busy (a program runs).
     */
    [[nodiscard]] Result<void> openSolution(std::shared_ptr<const project::SolutionModel> solution);
    // Closes the solution. Errors: Busy (a program runs).
    [[nodiscard]] Result<void> closeSolution();
    [[nodiscard]] std::shared_ptr<const project::SolutionModel> solution() const;

    // The projects the startup combo offers: the loaded C++ projects, in solution order.
    [[nodiscard]] std::vector<project::ProjectId> startupCandidates() const;
    [[nodiscard]] std::optional<project::ProjectId> startupProject() const;
    /*
     * Makes `project` the startup project and persists it (a blocking write of the small state
     * file); the choice applies even when saving fails. Errors: InvalidArgument (no solution, out
     * of range, not a loaded C++ project), the file errors.
     */
    [[nodiscard]] Result<void> setStartupProject(project::ProjectId project);

    // Whether console programs run in an external console window (header comment).
    [[nodiscard]] bool useExternalConsole() const;
    // Sets it for the open solution and persists it; applies to later runs. Errors: the file errors.
    [[nodiscard]] Result<void> setUseExternalConsole(bool external);

    /*
     * Builds and runs `project` (default: the startup project); returns once the run is accepted.
     * Errors: InvalidArgument (no solution, no active configuration, no startup project, or the
     * project is out of range or not a loaded C++ project), Unsupported (MSBuild runs need Windows),
     * Busy (the project is running already, or a build is in progress).
     */
    [[nodiscard]] Result<RunId> run(std::optional<project::ProjectId> project = std::nullopt);

    // Builds and resolves `project` like run() and hands it to `handlers` instead of starting it
    // (header comment). Errors: as run().
    [[nodiscard]] Result<RunId> debug(std::optional<project::ProjectId> project, DebugRunHandlers handlers);

    // Stops the project's run, if any, and runs it again once it has ended (onRunStarted reports
    // the new run). Returns the id of the run being stopped, or of the new run when nothing ran.
    // Errors: as run(), but never Busy for the project's own run.
    [[nodiscard]] Result<RunId> rerun(std::optional<project::ProjectId> project = std::nullopt);

    // Stops one run: a build in progress is cancelled, a program's process tree ends. Idempotent.
    void stop(RunId run);
    // Stops every run.
    void stopAll();

    // Writes `bytes` to the program's stdin (the Run console's input line adds the "\n"). Errors:
    // NotFound (no such run, or it has ended), InvalidArgument (its input is not the Run console's).
    [[nodiscard]] Result<void> sendInput(RunId run, std::string bytes);
    // Closes the program's stdin (end of file). Idempotent.
    void closeInput(RunId run);

    [[nodiscard]] std::optional<RunInfo> runInfo(RunId run) const;
    // The runs that have not finished, oldest first.
    [[nodiscard]] std::vector<RunInfo> activeRuns() const;
    // The unfinished run of `project`, if any.
    [[nodiscard]] std::optional<RunId> activeRun(project::ProjectId project) const;
    // How the last run ended; empty before the first one ends.
    [[nodiscard]] std::optional<RunOutcome> lastOutcome() const;

    // Forgets the cached run properties, so the next run asks MSBuild again.
    void invalidateProperties();

    // Blocks until no run is active and every callback queued so far has run, or `timeout`
    // passes; returns whether it got there. Not from a callback.
    [[nodiscard]] bool waitForIdle(IClock::Duration timeout) const;

    struct State; // Shared with the service's thread and the runs' callbacks; defined in the implementation.

private:
    explicit RunService(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> m_state;
};

} // namespace frost::build
