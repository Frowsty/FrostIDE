// SPDX-License-Identifier: Apache-2.0
//
// RunManager (see frost/run/run_manager.hpp): the dispatcher thread and its event queue, the run
// table, the before-launch hand-off, the before-launch tasks (RUN-5), the launch through
// build::RunSession and the end of a run.

#include "frost/run/run_manager.hpp"

#include "frost/core/mutex.hpp"
#include "frost/io/path.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <map>
#include <thread>
#include <utility>

namespace frost::run {

namespace {

#ifndef _WIN32
// Characters a POSIX shell word may hold unquoted (displayCommandLine).
bool shellSafe(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           std::string_view("_./=:,+-@%").find(c) != std::string_view::npos;
}

void appendPosixWord(std::string& out, std::string_view word) {
    if (!word.empty() && std::ranges::all_of(word, shellSafe)) {
        out += word;
        return;
    }
    out += '\'';
    for (const char c : word) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += '\'';
}
#endif

// The plan with its paths checked and normalised; an empty working directory becomes the program's folder.
Result<LaunchPlan> normalizePlan(LaunchPlan plan) {
    auto program = io::normalizePath(plan.program);
    if (!program) {
        return fail(ErrorCode::InvalidArgument, std::format("the program '{}' is not an absolute path", plan.program));
    }
    plan.program = std::move(*program);
    if (plan.workingDirectory.empty()) {
        plan.workingDirectory = std::string(io::parentPath(plan.program));
    } else {
        auto directory = io::normalizePath(plan.workingDirectory);
        if (!directory) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("the working directory '{}' is not an absolute path", plan.workingDirectory));
        }
        plan.workingDirectory = std::move(*directory);
    }
    return plan;
}

io::ProcessSpec specFor(const LaunchPlan& plan) {
    io::ProcessSpec spec;
    spec.executable = plan.program;
    spec.arguments = plan.arguments;
    spec.workingDirectory = plan.workingDirectory;
    for (const EnvironmentEntry& entry : plan.environment) {
        spec.environment.emplace_back(entry.name, entry.value);
    }
    spec.stdinMode = io::Stdio::Pipe;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true; // Stop ends the whole tree (io/process.hpp, "Process tree").
    return spec;
}

// A before-launch task's spec: its program and arguments in its working directory (else the plan's),
// FrostIDE's environment, no input, output piped, the tree contained.
io::ProcessSpec taskSpecFor(const BeforeLaunchTask& task, const LaunchPlan& plan) {
    io::ProcessSpec spec;
    spec.executable = io::normalizePath(task.program).value_or(task.program);
    spec.arguments = task.arguments;
    spec.workingDirectory =
        task.workingDirectory.empty() ? plan.workingDirectory : io::normalizePath(task.workingDirectory).value_or("");
    spec.stdinMode = io::Stdio::Null;
    spec.stdoutMode = io::Stdio::Pipe;
    spec.stderrMode = io::Stdio::Pipe;
    spec.killOnClose = true;
    return spec;
}

std::string taskCommandLine(const BeforeLaunchTask& task) {
    return displayCommandLine(LaunchPlan{.program = task.program, .arguments = task.arguments});
}

bool needsBeforeLaunch(const RunConfiguration& configuration) noexcept {
    return configuration.beforeLaunch == BeforeLaunch::Build || configuration.program.empty();
}

} // namespace

std::string_view toString(RunPhase phase) noexcept {
    switch (phase) {
    case RunPhase::BeforeLaunch: return "BeforeLaunch";
    case RunPhase::Running: return "Running";
    case RunPhase::Stopping: return "Stopping";
    case RunPhase::Finished: return "Finished";
    }
    return "Finished";
}

Result<LaunchPlan> launchPlanFor(const RunConfiguration& configuration) {
    if (auto valid = validateRunConfiguration(configuration); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    if (configuration.program.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("run configuration '{}' has no program yet", configuration.name));
    }
    std::vector<std::string> arguments = configuration.arguments;
    if (isFrostEngineRunType(configuration.type)) {
        // Engine EditorOptions and RuntimeOptions both accept a directory or .frostproj here.
        arguments.insert(arguments.begin(), {"--project", configuration.engineProjectPath});
    }
    return normalizePlan(LaunchPlan{.program = configuration.program,
                                    .arguments = std::move(arguments),
                                    .workingDirectory = configuration.workingDirectory,
                                    .environment = configuration.environment});
}

std::string displayCommandLine(const LaunchPlan& plan) {
#ifdef _WIN32
    if (auto line = io::buildWindowsCommandLine(plan.program, plan.arguments)) {
        return std::move(*line);
    }
    return plan.program;
#else
    std::string out;
    appendPosixWord(out, plan.program);
    for (const std::string& argument : plan.arguments) {
        out += ' ';
        appendPosixWord(out, argument);
    }
    return out;
#endif
}

struct RunManager::State : std::enable_shared_from_this<RunManager::State> {
    State(io::IProcessLauncher& launcherRef, RunManagerOptions optionsIn)
        : launcher(launcherRef), options(std::move(optionsIn)),
          clock(options.clock != nullptr ? *options.clock : systemClock()) {}

    // One active run. Owned by `runs`; removed (and its session destroyed) when it finishes.
    struct Run {
        RunStatus status;
        std::shared_ptr<const RunHandlers> handlers;
        std::stop_source stop;
        std::unique_ptr<build::RunSession> session;
        std::optional<LaunchPlan> plan;
        bool stopRequested = false;
        bool usedBeforeLaunch = false;
        // RUN-5: the before-launch task running now (index into the configuration's tasks).
        std::unique_ptr<build::RunSession> taskSession;
        std::size_t taskIndex = 0;
    };

    io::IProcessLauncher& launcher;
    const RunManagerOptions options;
    IClock& clock;

    mutable Mutex mutex;
    std::map<RunId, Run> runs FROST_GUARDED_BY(mutex); // Active runs, oldest first.
    RunId lastId FROST_GUARDED_BY(mutex) = 0;
    bool closing FROST_GUARDED_BY(mutex) = false;
    // runs.size() for waitForIdle, which holds queueMutex and must not take `mutex`.
    std::atomic<std::size_t> activeCount{0};

    // The dispatcher: runs the queued tasks (events and run steps) in order.
    mutable Mutex queueMutex;
    mutable std::condition_variable queueChanged;
    std::deque<std::function<void()>> tasks FROST_GUARDED_BY(queueMutex);
    bool taskRunning FROST_GUARDED_BY(queueMutex) = false;
    bool stopping FROST_GUARDED_BY(queueMutex) = false;
    std::thread worker;

    void post(std::function<void()> task) {
        {
            LockGuard lock(queueMutex);
            if (stopping) {
                return;
            }
            tasks.push_back(std::move(task));
        }
        queueChanged.notify_all();
    }

    void workerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                UniqueLock lock(queueMutex);
                while (tasks.empty() && !stopping) {
                    queueChanged.wait(lock.native());
                }
                if (stopping) {
                    return; // The rest is dropped (run_manager.hpp, "Lifetime").
                }
                task = std::move(tasks.front());
                tasks.pop_front();
                taskRunning = true;
            }
            task();
            task = nullptr; // Captures die on this thread, before waitForIdle can return.
            {
                LockGuard lock(queueMutex);
                taskRunning = false;
            }
            queueChanged.notify_all();
        }
    }

    // A snapshot of a run's status with its live input state.
    static RunStatus snapshot(const Run& run) {
        RunStatus status = run.status;
        status.acceptsInput = run.session != nullptr && run.session->acceptsInput();
        return status;
    }

    // ---- Event delivery; only on the dispatcher thread, never with `mutex` held.

    void deliverPhase(const RunStatus& status, const std::shared_ptr<const RunHandlers>& handlers) const {
        if (options.listener.onPhaseChanged) {
            options.listener.onPhaseChanged(status);
        }
        if (handlers && handlers->onPhaseChanged) {
            handlers->onPhaseChanged(status);
        }
    }

    void deliverFinished(const RunResult& result, const std::shared_ptr<const RunHandlers>& handlers) {
        if (options.listener.onFinished) {
            options.listener.onFinished(result);
        }
        if (handlers && handlers->onFinished) {
            handlers->onFinished(result);
        }
        activeCount.fetch_sub(1);
    }

    // Ends a run that never started its program (or a Prepare run); `extracted` is out of `runs`.
    void finishWithout(Run extracted, std::optional<Error> error, bool beforeLaunchFailed) {
        RunResult result;
        result.id = extracted.status.id;
        result.configurationId = extracted.status.configuration.id;
        result.mode = extracted.status.mode;
        result.stopped = extracted.stopRequested;
        result.beforeLaunchFailed = beforeLaunchFailed && !extracted.stopRequested;
        if (extracted.stopRequested && !error) {
            error = Error(ErrorCode::Cancelled, "the run was stopped before its program started");
        }
        result.error = std::move(error);
        result.plan = std::move(extracted.plan);
        deliverFinished(result, extracted.handlers);
    }

    std::optional<Run> extract(RunId id) FROST_EXCLUDES(mutex) {
        LockGuard lock(mutex);
        const auto it = runs.find(id);
        if (it == runs.end()) {
            return std::nullopt;
        }
        Run run = std::move(it->second);
        runs.erase(it);
        return run;
    }

    // ---- Run steps (dispatcher thread).

    void beginRun(RunId id) {
        RunConfiguration configuration;
        std::stop_token token;
        bool before = false;
        {
            LockGuard lock(mutex);
            const auto it = runs.find(id);
            if (it == runs.end()) {
                return;
            }
            configuration = it->second.status.configuration;
            token = it->second.stop.get_token();
            before = it->second.usedBeforeLaunch;
        }
        if (!before) {
            planReady(id, launchPlanFor(configuration));
            return;
        }
        const std::weak_ptr<State> weak = weak_from_this();
        // `done` may run on any thread, inside this call too: it only queues planReady.
        options.beforeLaunch(configuration, token, [weak, id](Result<LaunchPlan> plan) {
            if (const auto self = weak.lock()) {
                State* raw = self.get();
                raw->post([raw, id, plan = std::move(plan)]() mutable { raw->planReady(id, std::move(plan)); });
            }
        });
    }

    void planReady(RunId id, Result<LaunchPlan> plan) {
        bool stopRequested = false;
        bool before = false;
        {
            LockGuard lock(mutex);
            const auto it = runs.find(id);
            if (it == runs.end() || it->second.status.phase != RunPhase::BeforeLaunch || it->second.plan) {
                return; // Past BeforeLaunch already: a second `done` call is ignored.
            }
            stopRequested = it->second.stopRequested;
            before = it->second.usedBeforeLaunch;
            if (plan) {
                if (auto normalized = normalizePlan(std::move(*plan))) {
                    it->second.plan = std::move(*normalized);
                } else {
                    plan = std::unexpected(std::move(normalized.error()));
                }
            }
        }
        if (stopRequested || !plan) {
            auto run = extract(id);
            if (!run) {
                return;
            }
            std::optional<Error> error;
            if (!plan && !stopRequested) {
                error = std::move(plan.error());
            }
            finishWithout(std::move(*run), std::move(error), before && !plan.has_value());
            return;
        }
        runTask(id, 0);
    }

    // RUN-5: runs task `index` of the run's configuration, or, past the last one, goes on: Prepare
    // finishes with the plan, Run launches the program.
    void runTask(RunId id, std::size_t index) {
        BeforeLaunchTask task;
        LaunchPlan plan;
        bool done = false;
        bool prepare = false;
        bool stopped = false;
        {
            LockGuard lock(mutex);
            const auto it = runs.find(id);
            if (it == runs.end() || !it->second.plan) {
                return;
            }
            const auto& steps = it->second.status.configuration.beforeLaunchTasks;
            prepare = it->second.status.mode == RunMode::Prepare;
            stopped = it->second.stopRequested && !steps.empty();
            done = index >= steps.size();
            if (!done) {
                task = steps[index];
                plan = *it->second.plan;
            }
        }
        if (stopped) {
            // stop() came between two tasks (or before the first): nothing more starts.
            if (auto run = extract(id)) {
                finishWithout(std::move(*run), std::nullopt, false);
            }
            return;
        }
        if (done) {
            if (prepare) {
                if (auto run = extract(id)) {
                    finishWithout(std::move(*run), std::nullopt, false);
                }
                return;
            }
            launch(id);
            return;
        }
        const std::weak_ptr<State> weak = weak_from_this();
        build::RunSessionCallbacks callbacks;
        callbacks.onOutput = [weak, id](build::RunStream stream, std::string_view text) {
            if (const auto self = weak.lock()) {
                State* raw = self.get();
                raw->post([raw, id, stream, text = std::string(text)] { raw->output(id, stream, text); });
            }
        };
        callbacks.onExited = [weak, id, index](const build::RunExit& exit) {
            if (const auto self = weak.lock()) {
                State* raw = self.get();
                raw->post([raw, id, index, exit] { raw->taskExited(id, index, exit); });
            }
        };
        if (!admitLaunch(id)) {
            return;
        }
        const std::string commandLine = taskCommandLine(task);
        auto started =
            build::RunSession::start(launcher, taskSpecFor(task, plan), std::move(callbacks), clock, options.session);
        if (!started) {
            if (auto run = extract(id)) {
                Error error = std::move(started.error());
                static_cast<void>(
                    error.withContext(std::format("starting the before-launch task {}; '{}' was not started",
                                                  commandLine, run->status.configuration.name)));
                finishWithout(std::move(*run), std::move(error), true);
            }
            return;
        }
        RunStatus status;
        std::shared_ptr<const RunHandlers> handlers;
        {
            LockGuard lock(mutex);
            const auto it = runs.find(id);
            if (it == runs.end()) {
                return; // Not reachable: only this thread removes runs without a program session.
            }
            Run& run = it->second;
            run.taskSession = std::move(*started);
            run.taskIndex = index;
            run.status.beforeLaunchTask = commandLine;
            if (run.stopRequested) {
                run.taskSession->stop(); // stop() came while the task was being launched.
            }
            status = snapshot(run);
            handlers = run.handlers;
        }
        deliverPhase(status, handlers);
    }

    void taskExited(RunId id, std::size_t index, const build::RunExit& exit) {
        std::unique_ptr<build::RunSession> finished;
        bool stopRequested = false;
        std::string commandLine;
        std::string name;
        {
            LockGuard lock(mutex);
            const auto it = runs.find(id);
            if (it == runs.end() || it->second.taskIndex != index || !it->second.taskSession) {
                return;
            }
            finished = std::move(it->second.taskSession);
            stopRequested = it->second.stopRequested;
            commandLine = it->second.status.beforeLaunchTask;
            name = it->second.status.configuration.name;
            it->second.status.beforeLaunchTask.clear();
        }
        finished.reset(); // Joins the task session's threads; its last callback was delivered.
        if (stopRequested || exit.stopped) {
            if (auto run = extract(id)) {
                finishWithout(std::move(*run), std::nullopt, false);
            }
            return;
        }
        if (exit.waitFailed || exit.exitCode != 0) {
            if (auto run = extract(id)) {
                Error error = exit.waitFailed
                                  ? Error(ErrorCode::IoError,
                                          std::format("the before-launch task {} ended with an unreadable exit status; "
                                                      "'{}' was not started",
                                                      commandLine, name))
                                  : Error(ErrorCode::IoError,
                                          std::format("the before-launch task {} exited with code {}; '{}' was not "
                                                      "started",
                                                      commandLine, exit.exitCode, name));
                finishWithout(std::move(*run), std::move(error), true);
            }
            return;
        }
        runTask(id, index + 1);
    }

    // Called without the run-table lock. A revoked queued run starts no further child.
    bool admitLaunch(RunId id) {
        std::shared_ptr<const RunHandlers> handlers;
        bool stopped = false;
        {
            LockGuard lock(mutex);
            const auto it = runs.find(id);
            if (it == runs.end()) {
                return false;
            }
            handlers = it->second.handlers;
            stopped = it->second.stopRequested;
        }
        if (stopped) {
            if (auto run = extract(id)) {
                finishWithout(std::move(*run), std::nullopt, false);
            }
            return false;
        }
        if (handlers && handlers->launchAdmission) {
            if (auto allowed = handlers->launchAdmission(); !allowed) {
                if (auto run = extract(id)) {
                    finishWithout(std::move(*run), std::move(allowed.error()), false);
                }
                return false;
            }
            // Admission can perform worker I/O. Stop may arrive while it runs; success must not
            // launch a child whose stop request was already accepted during that work.
            {
                LockGuard lock(mutex);
                const auto it = runs.find(id);
                if (it == runs.end()) {
                    return false;
                }
                stopped = it->second.stopRequested;
            }
            if (stopped) {
                if (auto run = extract(id)) {
                    finishWithout(std::move(*run), std::nullopt, false);
                }
                return false;
            }
        }
        return true;
    }

    void launch(RunId id) {
        if (!admitLaunch(id)) {
            return;
        }
        LaunchPlan plan;
        std::shared_ptr<const RunHandlers> launchHandlers;
        {
            LockGuard lock(mutex);
            const auto it = runs.find(id);
            if (it == runs.end() || !it->second.plan) {
                return;
            }
            plan = *it->second.plan;
            launchHandlers = it->second.handlers;
        }
        if (launchHandlers && launchHandlers->transformLaunchPlan) {
            auto transformed = launchHandlers->transformLaunchPlan(std::move(plan));
            if (transformed) {
                transformed = normalizePlan(std::move(*transformed));
            }
            if (!transformed) {
                if (auto run = extract(id)) {
                    finishWithout(std::move(*run), std::move(transformed.error()), false);
                }
                return;
            }
            plan = std::move(*transformed);
            {
                LockGuard lock(mutex);
                const auto it = runs.find(id);
                if (it == runs.end()) {
                    return;
                }
                it->second.plan = plan;
            }
            // Resolution may involve worker I/O. Revocation or Stop during it prevents launch.
            if (!admitLaunch(id)) {
                return;
            }
        }
        const std::weak_ptr<State> weak = weak_from_this();
        build::RunSessionCallbacks callbacks;
        callbacks.onOutput = [weak, id](build::RunStream stream, std::string_view text) {
            if (const auto self = weak.lock()) {
                State* raw = self.get();
                raw->post([raw, id, stream, text = std::string(text)] { raw->output(id, stream, text); });
            }
        };
        callbacks.onExited = [weak, id](const build::RunExit& exit) {
            if (const auto self = weak.lock()) {
                State* raw = self.get();
                raw->post([raw, id, exit] { raw->exited(id, exit); });
            }
        };
        // Launched without `mutex`: queries and stop() stay responsive while CreateProcessW runs.
        auto started = build::RunSession::start(launcher, specFor(plan), std::move(callbacks), clock, options.session);
        if (!started) {
            if (auto run = extract(id)) {
                Error error = std::move(started.error());
                static_cast<void>(error.withContext(std::format("starting {}", plan.program)));
                finishWithout(std::move(*run), std::move(error), false);
            }
            return;
        }
        RunStatus status;
        std::shared_ptr<const RunHandlers> handlers;
        {
            LockGuard lock(mutex);
            const auto it = runs.find(id);
            if (it == runs.end()) {
                return; // Not reachable: only this thread removes runs that have no session.
            }
            Run& run = it->second;
            run.session = std::move(*started);
            run.status.processId = run.session->processId();
            run.status.commandLine = displayCommandLine(plan);
            run.status.phase = RunPhase::Running;
            if (run.stopRequested) {
                run.session->stop(); // stop() came while the program was being launched.
                run.status.phase = RunPhase::Stopping;
            }
            status = snapshot(run);
            handlers = run.handlers;
        }
        deliverPhase(status, handlers);
    }

    void output(RunId id, build::RunStream stream, const std::string& text) {
        std::shared_ptr<const RunHandlers> handlers;
        {
            LockGuard lock(mutex);
            const auto it = runs.find(id);
            if (it == runs.end()) {
                return;
            }
            handlers = it->second.handlers;
        }
        if (options.listener.onOutput) {
            options.listener.onOutput(id, stream, text);
        }
        if (handlers && handlers->onOutput) {
            handlers->onOutput(stream, text);
        }
    }

    void exited(RunId id, const build::RunExit& exit) {
        auto run = extract(id);
        if (!run) {
            return;
        }
        run->session.reset(); // Joins the session's waiter, which has delivered its last callback.
        RunResult result;
        result.id = id;
        result.configurationId = run->status.configuration.id;
        result.mode = run->status.mode;
        result.exitCode = exit.exitCode;
        result.stopped = exit.stopped;
        result.duration = exit.duration;
        result.cleanupFailed = exit.cleanupFailed;
        if (exit.waitFailed) {
            result.error = Error(ErrorCode::IoError, "the program's exit status could not be read");
        }
        result.plan = std::move(run->plan);
        deliverFinished(result, run->handlers);
    }
};

std::unique_ptr<RunManager> RunManager::create(io::IProcessLauncher& launcher, RunManagerOptions options) {
    auto state = std::make_shared<State>(launcher, std::move(options));
    State* raw = state.get();
    state->worker = std::thread([raw] { raw->workerLoop(); });
    return std::unique_ptr<RunManager>(new RunManager(std::move(state)));
}

RunManager::RunManager(std::shared_ptr<State> state) noexcept : m_state(std::move(state)) {}

RunManager::~RunManager() {
    State& state = *m_state;
    std::vector<std::stop_source> sources;
    std::vector<std::unique_ptr<build::RunSession>> sessions;
    {
        LockGuard lock(state.mutex);
        state.closing = true;
        for (auto& [id, run] : state.runs) {
            run.stopRequested = true;
            sources.push_back(run.stop);
            if (run.session) {
                run.session->stop();
                sessions.push_back(std::move(run.session));
            }
            if (run.taskSession) {
                run.taskSession->stop();
                sessions.push_back(std::move(run.taskSession));
            }
        }
    }
    for (std::stop_source& source : sources) {
        source.request_stop();
    }
    // Each session's destructor waits for its program to end; their callbacks only queue events.
    sessions.clear();
    {
        LockGuard lock(state.queueMutex);
        state.stopping = true;
    }
    state.queueChanged.notify_all();
    if (state.worker.joinable()) {
        state.worker.join();
    }
    // A launch that was under way when the stop began stored its session afterwards (and stopped it).
    {
        LockGuard lock(state.mutex);
        for (auto& [id, run] : state.runs) {
            if (run.session) {
                sessions.push_back(std::move(run.session));
            }
            if (run.taskSession) {
                sessions.push_back(std::move(run.taskSession));
            }
        }
    }
    sessions.clear();
}

Result<RunId> RunManager::start(const RunConfiguration& configuration, RunHandlers handlers, RunMode mode) {
    if (auto valid = validateRunConfiguration(configuration); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    if (configuration.type == RunType::Compound || configuration.type == RunType::Attach) {
        // Neither starts a program of its own: the Run toolbar starts a compound's members one by one,
        // and an attach configuration is the debugger's (RUN-5).
        return fail(ErrorCode::InvalidArgument,
                    std::format("run configuration '{}' is a {} configuration, which has no program to start",
                                configuration.name, toString(configuration.type)));
    }
    State& state = *m_state;
    const bool before = needsBeforeLaunch(configuration);
    if (before && !state.options.beforeLaunch) {
        return fail(ErrorCode::Unsupported,
                    std::format("run configuration '{}' needs a before-launch step to build it or resolve its "
                                "program, and none is available for {} configurations yet (RUN-2)",
                                configuration.name, toString(configuration.type)));
    }
    RunStatus status;
    RunId id = 0;
    {
        LockGuard lock(state.mutex);
        if (state.closing) {
            return fail(ErrorCode::Cancelled, "the run manager is shutting down");
        }
        for (const auto& [runId, run] : state.runs) {
            if (run.status.configuration.id == configuration.id) {
                return fail(ErrorCode::Busy, std::format("'{}' is already running", configuration.name));
            }
        }
        id = ++state.lastId;
        State::Run run;
        run.status.id = id;
        run.status.configuration = configuration;
        run.status.mode = mode;
        run.status.phase = RunPhase::BeforeLaunch;
        run.handlers = std::make_shared<const RunHandlers>(std::move(handlers));
        run.usedBeforeLaunch = before;
        status = run.status;
        state.runs.emplace(id, std::move(run));
        state.activeCount.fetch_add(1);
    }
    State* raw = &state;
    state.post([raw, status] {
        if (raw->options.listener.onStarted) {
            raw->options.listener.onStarted(status);
        }
        raw->beginRun(status.id);
    });
    return id;
}

void RunManager::stop(RunId run) {
    State& state = *m_state;
    std::optional<std::stop_source> source;
    RunStatus status;
    std::shared_ptr<const RunHandlers> handlers;
    {
        LockGuard lock(state.mutex);
        const auto it = state.runs.find(run);
        if (it == state.runs.end() || it->second.stopRequested) {
            return;
        }
        State::Run& entry = it->second;
        entry.stopRequested = true;
        if (entry.session) {
            entry.session->stop(); // Thread-safe and non-blocking; the session stays owned by `runs`.
            entry.status.phase = RunPhase::Stopping;
            status = State::snapshot(entry);
            handlers = entry.handlers;
        } else if (entry.taskSession) {
            entry.taskSession->stop(); // Its exit finishes the run as stopped (taskExited).
        } else if (entry.status.phase == RunPhase::BeforeLaunch && entry.usedBeforeLaunch) {
            source = entry.stop;
        }
    }
    // Outside the lock: the handler's stop callbacks run inside request_stop and may call back in.
    if (source) {
        source->request_stop();
    }
    if (handlers) {
        State* raw = &state;
        state.post([raw, status, handlers] { raw->deliverPhase(status, handlers); });
    }
}

void RunManager::stopAll() {
    std::vector<RunId> ids;
    {
        LockGuard lock(m_state->mutex);
        for (const auto& [id, run] : m_state->runs) {
            ids.push_back(id);
        }
    }
    for (const RunId id : ids) {
        stop(id);
    }
}

Result<void> RunManager::sendInput(RunId run, std::string bytes) {
    LockGuard lock(m_state->mutex);
    const auto it = m_state->runs.find(run);
    if (it == m_state->runs.end() || !it->second.session) {
        return fail(ErrorCode::NotFound, "the program is not running");
    }
    return it->second.session->writeInput(std::move(bytes));
}

void RunManager::closeInput(RunId run) {
    LockGuard lock(m_state->mutex);
    const auto it = m_state->runs.find(run);
    if (it != m_state->runs.end() && it->second.session) {
        it->second.session->closeInput();
    }
}

std::optional<RunStatus> RunManager::status(RunId run) const {
    LockGuard lock(m_state->mutex);
    const auto it = m_state->runs.find(run);
    if (it == m_state->runs.end()) {
        return std::nullopt;
    }
    return State::snapshot(it->second);
}

std::vector<RunStatus> RunManager::activeRuns() const {
    LockGuard lock(m_state->mutex);
    std::vector<RunStatus> out;
    out.reserve(m_state->runs.size());
    for (const auto& [id, run] : m_state->runs) {
        out.push_back(State::snapshot(run));
    }
    return out;
}

std::optional<RunId> RunManager::activeRun(std::string_view configurationId) const {
    LockGuard lock(m_state->mutex);
    for (const auto& [id, run] : m_state->runs) {
        if (run.status.configuration.id == configurationId) {
            return id;
        }
    }
    return std::nullopt;
}

bool RunManager::waitForIdle(IClock::Duration timeout) const {
    State& state = *m_state;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    UniqueLock lock(state.queueMutex);
    while (state.activeCount.load() != 0 || !state.tasks.empty() || state.taskRunning) {
        if (state.stopping) {
            return false;
        }
        if (state.queueChanged.wait_until(lock.native(), deadline) == std::cv_status::timeout) {
            return state.activeCount.load() == 0 && state.tasks.empty() && !state.taskRunning;
        }
    }
    return true;
}

} // namespace frost::run
