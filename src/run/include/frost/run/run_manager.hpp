// SPDX-License-Identifier: Apache-2.0
//
// RunManager (RUN-1, PLAN.md Appendix C R2): starts run configurations (run_configuration.hpp)
// through io::IProcessLauncher, streams their output, reports how they ended, and Stop ends the
// whole process tree. The one place FrostIDE runs a user's program from a run configuration: the
// Run toolbar and tool window (RUN-3), the generated CMake/MSBuild types (RUN-2), the test runner
// (RUN-7) and the debugger (prepare mode) build on it. Qt-free.
//
/*
 * A run goes through these phases:
 *
 *     BeforeLaunch -> Running [-> Stopping] -> Finished
 *
 *   - BeforeLaunch: from start() until the program starts. Only when the configuration has
 *     beforeLaunch Build or no program yet, the manager calls RunManagerOptions::beforeLaunch,
 *     which builds what the configuration names and resolves the program (RUN-2 supplies it for
 *     cmake-target and msbuild; the CMake integration runs its own build first today). The
 *     handler returns at once and calls `done` exactly once (a later call is ignored), on any
 *     thread (also inside the call), with the LaunchPlan to start or the error that stops the run
 *     (a failed build). stop() requests the stop token; a handler that honours it calls done with
 *     Cancelled. Whatever done reports after a stop, nothing is started.
 *   - Running: build::RunSession runs the plan: the absolute program, its arguments, working
 *     directory and environment, stdin a pipe (sendInput), stdout and stderr piped and streamed as
 *     UTF-8 chunks (not lines), the tree contained (a Job Object on Windows, a new session on
 *     POSIX; io/process.hpp "Process tree").
 *   - Stopping: stop() ended the process tree (every descendant, not just the program); the
 *     program's exit is still being collected.
 *   - Finished: RunResult says how: the exit code, stopped, or the error that kept it from running.
 *     A program that exits by itself leaves what it started running, as one started from Explorer
 *     or Visual Studio would (RunSession, "End of a run").
 * RunMode::Prepare stops after BeforeLaunch: the run finishes with RunResult::plan and starts
 * nothing, so the debugger can hand the same program to lldb-dap.
 *
 * One run per configuration id at a time: start() for an id that is still active fails with Busy
 * (the UI asks "stop and rerun?"). Different configurations run side by side.
 *
 * Callbacks. The listener's (RunManagerOptions::listener) and each run's own (RunHandlers) run on
 * the manager's dispatcher thread, one at a time, in event order: for an event the listener first,
 * then the run's handler. onStarted precedes every other event of its run and onFinished is its
 * last. A callback may call every member except the destructor and waitForIdle; it must return
 * quickly (the UI posts to its own thread). Queries return snapshots and may run on any thread.
 *
 * Lifetime. The launcher and the clock must outlive the manager. The destructor stops every run
 * (the trees end), waits for the programs to end and stops the dispatcher; events still queued are
 * dropped, and a before-launch `done` that arrives later is ignored.
 */
#pragma once

#include "frost/build/run_session.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"
#include "frost/run/run_configuration.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::run {

using RunId = std::uint64_t; // 1, 2, ... per manager; 0 is never a run.

enum class RunPhase : std::uint8_t { BeforeLaunch, Running, Stopping, Finished };

// "BeforeLaunch", "Running", "Stopping" or "Finished".
[[nodiscard]] std::string_view toString(RunPhase phase) noexcept;

enum class RunMode : std::uint8_t {
    Run,     // Start the program.
    Prepare, // Run the before-launch step and resolve the plan only (header comment).
};

using OutputStream = build::RunStream; // Stdout or Stderr.

// Exactly what is started. Produced by launchPlanFor or the before-launch handler.
struct LaunchPlan {
    std::string program{};                // Absolute.
    std::vector<std::string> arguments{}; // argv[1..].
    std::string workingDirectory{};       // Absolute.
    std::vector<EnvironmentEntry> environment{};

    friend bool operator==(const LaunchPlan&, const LaunchPlan&) = default;
};

/*
 * The plan for a configuration whose program is known: its program, arguments and environment,
 * and its working directory or else the program's folder; both normalised. Errors: InvalidArgument
 * (no program, or validateRunConfiguration fails). Pure.
 */
[[nodiscard]] Result<LaunchPlan> launchPlanFor(const RunConfiguration& configuration);

// The command line as the Run tool window shows it: on Windows the CreateProcessW command line
// (io::buildWindowsCommandLine), elsewhere the arguments with POSIX shell quoting where needed.
// For display only; nothing parses it. Pure.
[[nodiscard]] std::string displayCommandLine(const LaunchPlan& plan);

// What a before-launch handler reports: the plan to start, or why the run cannot go on.
using BeforeLaunchDone = std::function<void(Result<LaunchPlan> plan)>;
/*
 * Builds and resolves `configuration` (header comment, BeforeLaunch). Must not block: it starts the
 * work and returns. `stop` is requested when the run is stopped or the manager is destroyed. Called
 * on the dispatcher thread.
 */
using BeforeLaunchHandler =
    std::function<void(const RunConfiguration& configuration, std::stop_token stop, BeforeLaunchDone done)>;

// One run as the UI shows it (a snapshot).
struct RunStatus {
    RunId id = 0;
    RunConfiguration configuration{}; // As it was when start() accepted it.
    RunMode mode = RunMode::Run;
    RunPhase phase = RunPhase::BeforeLaunch;
    std::string commandLine{};   // displayCommandLine of the plan; known from Running on.
    io::ProcessId processId = 0; // The program, the root of the tree; known from Running on.
    bool acceptsInput = false;   // sendInput can still reach the program.
};

// How a run ended.
struct RunResult {
    RunId id = 0;
    std::string configurationId{};
    RunMode mode = RunMode::Run;
    // Why nothing ran: the before-launch step failed or was cancelled, or the launch failed.
    std::optional<Error> error{};
    bool beforeLaunchFailed = false;  // The error came from the before-launch handler.
    bool stopped = false;             // stop() ended the program or its before-launch step.
    std::optional<int> exitCode{};    // When the program ran (io::ExitStatus::exitCode's meaning).
    IClock::Duration duration{};      // The program's run time, from launch to exit.
    bool cleanupFailed = false;       // The tree could not be ended (build::RunExit::cleanupFailed).
    std::optional<LaunchPlan> plan{}; // The resolved plan: what ran, or what Prepare resolved.

    [[nodiscard]] bool ran() const noexcept { return exitCode.has_value(); }
};

// The callbacks of one run (start()); every member optional (header comment, "Callbacks").
struct RunHandlers {
    std::function<void(const RunStatus& run)> onPhaseChanged{}; // Running, Stopping.
    std::function<void(OutputStream stream, std::string_view text)> onOutput{};
    std::function<void(const RunResult& result)> onFinished{};
};

// The callbacks for every run, for the Run tool window and the toolbar (RUN-3).
struct RunListener {
    std::function<void(const RunStatus& run)> onStarted{};      // start() accepted it.
    std::function<void(const RunStatus& run)> onPhaseChanged{}; // Running, Stopping.
    std::function<void(RunId run, OutputStream stream, std::string_view text)> onOutput{};
    std::function<void(const RunResult& result)> onFinished{};
};

struct RunManagerOptions {
    // Empty: configurations that need a before-launch step are refused (Unsupported).
    BeforeLaunchHandler beforeLaunch{};
    RunListener listener{};
    build::RunSessionOptions session{};
    IClock* clock = nullptr; // Null: systemClock().
};

class RunManager {
public:
    // Creates the manager and starts its dispatcher thread (header comment for the lifetimes).
    [[nodiscard]] static std::unique_ptr<RunManager> create(io::IProcessLauncher& launcher,
                                                            RunManagerOptions options = {});

    ~RunManager();
    RunManager(const RunManager&) = delete ("owns a thread and the runs");
    RunManager& operator=(const RunManager&) = delete ("owns a thread and the runs");

    /*
     * Starts `configuration` (header comment); returns once the run is accepted. The trust gate is
     * the caller's: only a configuration of a trusted workspace may be passed. Launch errors are
     * reported through onFinished, not here. Errors: InvalidArgument (validateRunConfiguration),
     * Busy (a run of the same configuration id is active), Unsupported (a before-launch step or
     * program resolution is needed and no handler is set), Cancelled (the manager is closing).
     */
    [[nodiscard]] Result<RunId> start(const RunConfiguration& configuration, RunHandlers handlers = {},
                                      RunMode mode = RunMode::Run);

    // Stops one run: its before-launch step is cancelled, or its whole process tree ends. Idempotent;
    // nothing for an unknown or finished run.
    void stop(RunId run);
    // Stops every active run.
    void stopAll();

    // Queues `bytes` for the program's stdin (the Run console adds the "\n"). Errors: NotFound (no
    // such active run, or it is not Running), InvalidArgument (its input is closed).
    [[nodiscard]] Result<void> sendInput(RunId run, std::string bytes);
    // Closes the program's stdin once queued input is written (end of file). Idempotent.
    void closeInput(RunId run);

    // The active run's status; nullopt once it has finished (or for an unknown id).
    [[nodiscard]] std::optional<RunStatus> status(RunId run) const;
    // The runs that have not finished, oldest first.
    [[nodiscard]] std::vector<RunStatus> activeRuns() const;
    // The active run of a configuration id, if any.
    [[nodiscard]] std::optional<RunId> activeRun(std::string_view configurationId) const;

    // Blocks until no run is active and every event queued so far has been delivered, or `timeout`
    // passes; returns whether it got there. Not from a callback.
    [[nodiscard]] bool waitForIdle(IClock::Duration timeout) const;

    struct State; // Shared with the dispatcher, the sessions' callbacks and before-launch handlers.

private:
    explicit RunManager(std::shared_ptr<State> state) noexcept;

    std::shared_ptr<State> m_state;
};

} // namespace frost::run
