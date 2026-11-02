// SPDX-License-Identifier: Apache-2.0
//
// RunService (see frost/build/run_service.hpp): the service thread and its task queue, the run
// pipeline (build, resolve, launch), the persisted startup projects, the property cache and the
// BuildService adapter.

#include "frost/build/run_service.hpp"

#include "frost/build/build_diagnostics.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <map>
#include <thread>
#include <utility>

namespace frost::build {

namespace {

constexpr std::string_view kStateHeaderPrefix = "# FrostIDE run settings v";
constexpr int kStateVersion = 1;
constexpr std::string_view kStateFileName = "run-settings.txt";
constexpr std::string_view kExternal = "external";
constexpr std::string_view kRunConsole = "run-console";
// MSBuild evaluates one project in about a second; a cold start of a big SDK can take much longer.
constexpr auto kQueryTimeout = std::chrono::seconds(120);

// One persisted line: a solution, its startup project file ("" if none was chosen) and the console choice.
struct StateEntry {
    std::string solution;
    std::string project;
    std::optional<bool> external;
};

std::vector<std::string_view> splitTabs(std::string_view line) {
    std::vector<std::string_view> fields;
    while (true) {
        const std::size_t tab = line.find('\t');
        fields.push_back(line.substr(0, tab));
        if (tab == std::string_view::npos) {
            return fields;
        }
        line.remove_prefix(tab + 1);
    }
}

// Reads the state file (run_service.hpp, "Startup project"); a missing file has no entries.
Result<std::vector<StateEntry>> readStateFile(io::IFileSystem& fs, const std::string& path) {
    auto content = fs.readFile(path);
    if (!content) {
        if (content.error().code() == ErrorCode::NotFound) {
            return std::vector<StateEntry>{};
        }
        return std::unexpected(std::move(content.error()));
    }
    std::string_view text = *content;
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    std::vector<StateEntry> entries;
    bool first = true;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (first) {
            first = false;
            if (!line.starts_with(kStateHeaderPrefix)) {
                return fail(ErrorCode::Corrupt, std::format("{}: the header line is missing", path));
            }
            const std::string_view number = line.substr(kStateHeaderPrefix.size());
            int version = 0;
            const auto [rest, error] = std::from_chars(number.data(), number.data() + number.size(), version);
            if (error != std::errc{} || rest != number.data() + number.size()) {
                return fail(ErrorCode::Corrupt, std::format("{}: the header line is malformed", path));
            }
            if (version > kStateVersion) {
                return fail(ErrorCode::Unsupported, std::format("{} has format version {}; this FrostIDE reads {}",
                                                                path, version, kStateVersion));
            }
            continue;
        }
        const std::vector<std::string_view> fields = splitTabs(line);
        if (fields.size() < 2 || fields[0].empty()) {
            continue; // Not "<solution>\t<project>[\t<console>]": skipped.
        }
        StateEntry entry{std::string(fields[0]), std::string(fields[1]), std::nullopt};
        if (fields.size() >= 3 && fields[2] == kExternal) {
            entry.external = true;
        } else if (fields.size() >= 3 && fields[2] == kRunConsole) {
            entry.external = false;
        }
        entries.push_back(std::move(entry));
    }
    if (first) {
        return fail(ErrorCode::Corrupt, std::format("{}: the header line is missing", path));
    }
    return entries;
}

std::string formatStateFile(const std::vector<StateEntry>& entries) {
    std::string text = std::format("{}{}\n", kStateHeaderPrefix, kStateVersion);
    for (const StateEntry& entry : entries) {
        text += std::format("{}\t{}", entry.solution, entry.project);
        if (entry.external) {
            text += std::format("\t{}", *entry.external ? kExternal : kRunConsole);
        }
        text += '\n';
    }
    return text;
}

bool isLoadedCpp(const project::ProjectInfo& info) noexcept {
    return info.nodeKind == project::NodeKind::Project && info.vcxproj.has_value() && !info.path.empty();
}

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return lower(x) == lower(y); });
}

// The ConfigurationType a project has in solution configuration `configuration` (its mapped
// project configuration's, else the project's first value).
std::string configurationTypeOf(const project::SolutionModel& solution, project::ProjectId id,
                                std::optional<std::size_t> configuration) {
    const project::ProjectInfo& info = solution.project(id);
    if (!info.vcxproj) {
        return {};
    }
    if (configuration && *configuration < solution.configurations().size()) {
        if (const auto& mapping = solution.projectConfiguration(id, *configuration)) {
            for (const project::ProjectConfiguration& config : info.vcxproj->configurations) {
                if (equalsIgnoringCase(config.configuration, mapping->configuration) &&
                    equalsIgnoringCase(config.platform, mapping->platform) && !config.configurationType.empty()) {
                    return config.configurationType;
                }
            }
        }
    }
    return info.vcxproj->configurationType;
}

// The default startup project (run_service.hpp, "Startup project").
std::optional<project::ProjectId> defaultStartupProject(const project::SolutionModel& solution,
                                                        std::optional<std::size_t> configuration) {
    const auto projects = solution.projects();
    for (std::uint32_t i = 0; i < projects.size(); ++i) {
        if (isLoadedCpp(projects[i]) &&
            equalsIgnoringCase(configurationTypeOf(solution, project::ProjectId{i}, configuration), "Application")) {
            return project::ProjectId{i};
        }
    }
    for (std::uint32_t i = 0; i < projects.size(); ++i) {
        if (isLoadedCpp(projects[i])) {
            return project::ProjectId{i};
        }
    }
    if (!projects.empty()) {
        return project::ProjectId{0};
    }
    return std::nullopt;
}

// What a file looked like when its properties were cached (missing files included).
struct FileStamp {
    bool exists = false;
    std::uint64_t size = 0;
    IClock::SystemTime mtime{};

    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

FileStamp stampOf(io::IFileSystem& fs, std::string_view path) {
    auto status = fs.status(path);
    if (!status) {
        return {};
    }
    return {true, status->size, status->mtime};
}

class BuildServiceRunBuilder final : public IRunBuilder {
public:
    explicit BuildServiceRunBuilder(BuildService& service) noexcept : m_service(service) {}

    std::optional<std::size_t> activeConfigurationIndex() const override {
        return m_service.activeConfigurationIndex();
    }
    bool building() const override { return m_service.state() != BuildState::Idle; }
    Result<void> buildForRun(project::ProjectId project, BuildCompletion done) override {
        return m_service.build(BuildAction::Build, project, std::move(done));
    }
    void cancelBuild() override { m_service.cancel(); }

private:
    BuildService& m_service;
};

} // namespace

std::unique_ptr<IRunBuilder> makeRunBuilder(BuildService& service) {
    return std::make_unique<BuildServiceRunBuilder>(service);
}

std::string_view toString(RunState state) noexcept {
    switch (state) {
    case RunState::Building: return "Building";
    case RunState::Resolving: return "Resolving";
    case RunState::Running: return "Running";
    case RunState::Stopping: return "Stopping";
    case RunState::Finished: return "Finished";
    }
    return "Building";
}

struct RunService::State : std::enable_shared_from_this<RunService::State> {
    State(io::IFileSystem& fileSystem, io::IProcessLauncher& processLauncher, project::TrustStore& store,
          IRunBuilder& runBuilder, RunServiceOptions serviceOptions, RunServiceCallbacks serviceCallbacks)
        : fs(fileSystem), launcher(processLauncher), trustStore(store), builder(runBuilder),
          options(std::move(serviceOptions)), callbacks(std::move(serviceCallbacks)) {}

    struct Run {
        RunInfo info{};
        std::size_t configurationIndex = 0;
        bool stopRequested = false;
        bool rerunPending = false;
        std::unique_ptr<RunSession> session{}; // While Running and Stopping.
    };

    struct CacheEntry {
        FileStamp project;
        FileStamp user;
        RunProperties properties;
    };

    io::IFileSystem& fs;
    io::IProcessLauncher& launcher;
    project::TrustStore& trustStore;
    IRunBuilder& builder;
    const RunServiceOptions options;
    const RunServiceCallbacks callbacks;

    mutable Mutex mutex;
    std::shared_ptr<const project::SolutionModel> solution FROST_GUARDED_BY(mutex);
    std::optional<project::ProjectId> startup FROST_GUARDED_BY(mutex);
    bool external FROST_GUARDED_BY(mutex) = true;
    std::map<RunId, Run> runs FROST_GUARDED_BY(mutex); // Unfinished runs, oldest first.
    RunId lastId FROST_GUARDED_BY(mutex) = 0;
    std::optional<RunOutcome> last FROST_GUARDED_BY(mutex);
    std::string msbuildPath FROST_GUARDED_BY(mutex);
    std::map<std::string, CacheEntry> cache FROST_GUARDED_BY(mutex);
    bool closing FROST_GUARDED_BY(mutex) = false;
    // runs.size() for waitForIdle, which must not take `mutex` (it holds queueMutex).
    std::atomic<std::size_t> activeCount{0};

    // Serializes the state file's read-modify-write cycles.
    Mutex persistMutex;

    // The service thread: runs the tasks (pipeline steps and callbacks) in order.
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
                    return; // The rest is dropped (run_service.hpp, "Lifetime").
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

    // ---- Events: each callback becomes a task, so they run in order on the service thread.

    void emitStarted(RunInfo info) {
        post([this, info = std::move(info)] {
            if (callbacks.onRunStarted) {
                callbacks.onRunStarted(info);
            }
        });
    }

    void emitStateChanged(RunInfo info) {
        post([this, info = std::move(info)] {
            if (callbacks.onRunStateChanged) {
                callbacks.onRunStateChanged(info);
            }
        });
    }

    void emitOutput(RunId id, RunOutputKind kind, std::string text) {
        post([this, id, kind, text = std::move(text)] {
            if (callbacks.onOutput) {
                callbacks.onOutput(id, kind, text);
            }
        });
    }

    void emitSystem(RunId id, std::string line) { emitOutput(id, RunOutputKind::System, std::move(line) + '\n'); }

    void emitStartup(std::optional<project::ProjectId> project) {
        post([this, project] {
            if (callbacks.onStartupProjectChanged) {
                callbacks.onStartupProjectChanged(project);
            }
        });
    }

    void emitExternal(bool value) {
        post([this, value] {
            if (callbacks.onExternalConsoleChanged) {
                callbacks.onExternalConsoleChanged(value);
            }
        });
    }

    Run* findLocked(RunId id) FROST_REQUIRES(mutex) {
        const auto it = runs.find(id);
        return it == runs.end() ? nullptr : &it->second;
    }

    void setStateLocked(Run& run, RunState state) FROST_REQUIRES(mutex) {
        run.info.state = state;
        emitStateChanged(run.info);
    }

    // Publishes how run `id` ended and forgets it; returns the project to run again, if a rerun waits.
    std::optional<project::ProjectId> finishLocked(RunId id, RunOutcome outcome) FROST_REQUIRES(mutex) {
        Run* run = findLocked(id);
        if (run == nullptr) {
            return std::nullopt;
        }
        outcome.run = run->info;
        outcome.run.state = RunState::Finished;
        outcome.run.acceptsInput = false;
        std::optional<project::ProjectId> again;
        if (run->rerunPending && !closing) {
            again = run->info.project;
        }
        post([this, outcome] {
            if (callbacks.onRunFinished) {
                callbacks.onRunFinished(outcome);
            }
        });
        last = std::move(outcome);
        runs.erase(id);
        activeCount.store(runs.size());
        return again;
    }

    // Ends run `id` before its program started.
    std::optional<project::ProjectId> failLocked(RunId id, Error error) FROST_REQUIRES(mutex) {
        Run* run = findLocked(id);
        if (run == nullptr) {
            return std::nullopt;
        }
        emitSystem(id, std::format("{} was not started: {}", run->info.projectName, error.message()));
        RunOutcome outcome;
        outcome.error = std::move(error);
        outcome.stopped = run->stopRequested;
        return finishLocked(id, std::move(outcome));
    }

    std::optional<project::ProjectId> stoppedLocked(RunId id) FROST_REQUIRES(mutex) {
        RunOutcome outcome;
        outcome.stopped = true;
        emitSystem(id, "Stopped before the program started.");
        return finishLocked(id, std::move(outcome));
    }

    // Starts the pending rerun of `project`, outside the lock.
    void startAgain(RunId previous, std::optional<project::ProjectId> project);

    // ---- The pipeline (service thread).

    void beginBuild(RunId id) {
        project::ProjectId project{};
        std::optional<project::ProjectId> again;
        {
            LockGuard lock(mutex);
            Run* run = findLocked(id);
            if (run == nullptr) {
                return;
            }
            if (run->stopRequested) {
                again = stoppedLocked(id);
            }
            project = run->info.project;
        }
        if (again) {
            startAgain(id, again);
            return;
        }
        auto started = builder.buildForRun(project, [weak = weak_from_this(), id](const BuildOutcome& outcome) {
            if (const auto self = weak.lock()) {
                State* raw = self.get();
                raw->post([raw, id, outcome] { raw->afterBuild(id, outcome); });
            }
        });
        if (started) {
            return;
        }
        if (started.error().code() == ErrorCode::Unsupported) {
            {
                LockGuard lock(mutex);
                if (Run* run = findLocked(id)) {
                    emitSystem(id, std::format("{} is not built in {}; running its last build.", run->info.projectName,
                                               run->info.configuration.name()));
                }
            }
            afterBuild(id, std::nullopt);
            return;
        }
        {
            LockGuard lock(mutex);
            again = failLocked(id, std::move(started.error()).withContext("while building before the run"));
        }
        startAgain(id, again);
    }

    // `outcome` is empty when the build was skipped.
    void afterBuild(RunId id, std::optional<BuildOutcome> outcome) {
        std::optional<project::ProjectId> again;
        {
            LockGuard lock(mutex);
            Run* run = findLocked(id);
            if (run == nullptr) {
                return;
            }
            if (outcome && !outcome->succeeded()) {
                const bool cancelled = outcome->result.cancelled;
                emitSystem(id, std::format("Build {}; {} was not started.", cancelled ? "cancelled" : "failed",
                                           run->info.projectName));
                RunOutcome result;
                result.buildFailed = true;
                result.stopped = run->stopRequested;
                result.error =
                    outcome->error
                        ? *outcome->error
                        : Error(cancelled ? ErrorCode::Cancelled : ErrorCode::IoError,
                                cancelled ? std::string("the build was cancelled")
                                          : std::format("the build failed with {} error{}", outcome->counts.errors,
                                                        outcome->counts.errors == 1 ? "" : "s"));
                again = finishLocked(id, std::move(result));
            } else if (run->stopRequested) {
                again = stoppedLocked(id);
            } else {
                setStateLocked(*run, RunState::Resolving);
            }
        }
        if (again) {
            startAgain(id, again);
            return;
        }
        resolveAndLaunch(id);
    }

    // The MSBuild path for the default property source (found once with vswhere).
    Result<std::string> msbuild() {
        {
            LockGuard lock(mutex);
            if (!options.msbuildPath.empty()) {
                return options.msbuildPath;
            }
            if (!msbuildPath.empty()) {
                return msbuildPath;
            }
        }
        auto found = findMsBuild(launcher, fs, {.vswherePath = {}, .instanceId = options.vsInstanceId});
        if (!found) {
            return std::unexpected(std::move(found.error()).withContext("while looking for MSBuild"));
        }
        LockGuard lock(mutex);
        msbuildPath = found->msbuildPath;
        return msbuildPath;
    }

    // The run properties of `request`, from the cache while the project files are unchanged.
    Result<RunProperties> properties(RunPropertyRequest request) {
        const std::string key =
            documentKey(request.projectPath) + '\n' + request.configuration + '|' + request.platform;
        const FileStamp projectStamp = stampOf(fs, request.projectPath);
        const FileStamp userStamp = stampOf(fs, request.projectPath + ".user");
        {
            LockGuard lock(mutex);
            if (const auto it = cache.find(key);
                it != cache.end() && it->second.project == projectStamp && it->second.user == userStamp) {
                return it->second.properties;
            }
        }
        Result<RunProperties> found = RunProperties{};
        if (options.properties) {
            request.msbuildPath = options.msbuildPath;
            found = options.properties(request);
        } else {
            auto path = msbuild();
            if (!path) {
                return std::unexpected(std::move(path.error()));
            }
            request.msbuildPath = *path;
            found = queryRunProperties(launcher, request, kQueryTimeout);
            if (!found && found.error().code() == ErrorCode::Unsupported) {
                const std::string why = found.error().message();
                found = staticRunProperties(fs, request, options.environment);
                if (found) {
                    found->warnings.insert(found->warnings.begin(), why);
                }
            }
        }
        if (!found) {
            return found;
        }
        LockGuard lock(mutex);
        cache[key] = CacheEntry{projectStamp, userStamp, *found};
        return found;
    }

    // Resolving: asks what to run, picks the console mode and starts the program.
    void resolveAndLaunch(RunId id) {
        std::shared_ptr<const project::SolutionModel> model;
        RunPropertyRequest request;
        bool externalConsole = true;
        {
            LockGuard lock(mutex);
            Run* run = findLocked(id);
            if (run == nullptr || !solution) {
                return;
            }
            model = solution;
            const project::ProjectInfo& info = model->project(run->info.project);
            request.projectPath = info.path;
            request.solutionPath = model->path();
            const auto& mapping = model->projectConfiguration(run->info.project, run->configurationIndex);
            request.configuration = mapping ? mapping->configuration : run->info.configuration.configuration;
            request.platform = mapping ? mapping->platform : run->info.configuration.platform;
            externalConsole = external;
        }
        // The query and the program run project code: the folder must be trusted (the build's
        // prompt asked already; a skipped build did not).
        if (auto trusted = project::checkBuildTrust(trustStore, *model); !trusted) {
            abortRun(id, Error(ErrorCode::Untrusted, std::format("{}; build the solution once to trust its folder",
                                                                 trusted.error().message())));
            return;
        }
        auto found = properties(request);
        if (!found) {
            abortRun(id, std::move(found.error()));
            return;
        }
        for (const std::string& warning : found->warnings) {
            emitSystem(id, "Warning: " + warning);
        }
        const RunMacros macros = runMacros(*found, request);
        auto command = resolveRunCommand(*found, macros, options.environment);
        if (!command) {
            abortRun(id, std::move(command.error()));
            return;
        }
        for (const std::string& warning : command->warnings) {
            emitSystem(id, "Warning: " + warning);
        }
        auto image = fs.readFile(command->executable);
        if (!image) {
            abortRun(id, image.error().code() == ErrorCode::NotFound
                             ? Error(ErrorCode::NotFound,
                                     std::format("{} does not exist; build the project first", command->executable))
                             : std::move(image.error()));
            return;
        }
        const Subsystem subsystem = peSubsystem(*image).value_or(Subsystem::Unknown);
        image->clear();
        image->shrink_to_fit();
        const ConsoleMode mode = subsystem == Subsystem::Gui ? ConsoleMode::NoConsole
                                 : externalConsole && options.msbuildRunSupported && io::kWindowsProcessRules
                                     ? ConsoleMode::ExternalConsole
                                     : ConsoleMode::RunConsole;
        std::string cmdPath = options.cmdPath;
        if (mode == ConsoleMode::ExternalConsole && cmdPath.empty()) {
            auto system = systemCommandProcessor();
            if (!system) {
                abortRun(id, std::move(system.error()));
                return;
            }
            cmdPath = std::move(*system);
        }
        auto spec = runProcessSpec(*command, mode, cmdPath);
        if (!spec) {
            abortRun(id, std::move(spec.error()));
            return;
        }
        {
            LockGuard lock(mutex);
            Run* run = findLocked(id);
            if (run == nullptr) {
                return;
            }
            if (run->stopRequested || closing) {
                startAgainLocked(id, stoppedLocked(id));
                return;
            }
        }
        emitSystem(id, command->displayCommandLine() +
                           (mode == ConsoleMode::ExternalConsole ? "   (in an external console window)" : ""));
        auto weak = weak_from_this();
        RunSessionCallbacks sessionCallbacks{
            .onOutput =
                [weak, id](RunStream stream, std::string_view text) {
                    if (const auto self = weak.lock()) {
                        self->emitOutput(id,
                                         stream == RunStream::Stdout ? RunOutputKind::Stdout : RunOutputKind::Stderr,
                                         std::string(text));
                    }
                },
            .onExited =
                [weak, id](const RunExit& exit) {
                    if (const auto self = weak.lock()) {
                        State* raw = self.get();
                        raw->post([raw, id, exit] { raw->programExited(id, exit); });
                    }
                },
        };
        auto session = RunSession::start(launcher, *spec, std::move(sessionCallbacks), systemClock(), options.session);
        std::unique_ptr<RunSession> stale;
        {
            LockGuard lock(mutex);
            Run* run = findLocked(id);
            if (!session) {
                if (run != nullptr) {
                    startAgainLocked(id, failLocked(id, std::move(session.error())));
                }
                return;
            }
            if (run == nullptr || closing) {
                stale = std::move(*session);
            } else {
                run->session = std::move(*session);
                run->info.mode = mode;
                run->info.commandLine = command->displayCommandLine();
                run->info.processId = run->session->processId();
                run->info.acceptsInput = run->session->acceptsInput();
                setStateLocked(*run, run->stopRequested ? RunState::Stopping : RunState::Running);
                if (run->stopRequested) {
                    run->session->stop(); // Stop came while it launched.
                }
            }
        }
        if (stale) {
            stale->stop();
            stale.reset();
        }
    }

    // Ends run `id` before its program started, outside the lock.
    void abortRun(RunId id, Error error) {
        std::optional<project::ProjectId> again;
        {
            LockGuard lock(mutex);
            again = failLocked(id, std::move(error));
        }
        startAgain(id, again);
    }

    // A rerun may only start once the lock is released (run() takes it): this posts it.
    void startAgainLocked(RunId previous, std::optional<project::ProjectId> project) FROST_REQUIRES(mutex) {
        if (project) {
            post([this, previous, project] { startAgain(previous, project); });
        }
    }

    // Service thread: the program has exited and every output chunk was posted before this.
    void programExited(RunId id, const RunExit& exit) {
        std::unique_ptr<RunSession> session;
        std::optional<project::ProjectId> again;
        {
            LockGuard lock(mutex);
            Run* run = findLocked(id);
            if (run == nullptr) {
                return;
            }
            session = std::move(run->session);
            RunOutcome outcome;
            outcome.stopped = exit.stopped;
            outcome.duration = exit.duration;
            if (exit.waitFailed) {
                outcome.error = Error(ErrorCode::IoError, "the program's exit status could not be read");
            } else {
                outcome.exitCode = exit.exitCode;
            }
            again = finishLocked(id, std::move(outcome));
        }
        session.reset(); // Joins the session's waiter, which has just returned from onExited.
        startAgain(id, again);
    }

    // Writes the startup project and console choice for `solutionPath`.
    Result<void> persist(const std::string& solutionPath, const std::string& projectPath, bool externalConsole) {
        if (options.stateFilePath.empty()) {
            return {};
        }
        LockGuard lock(persistMutex);
        auto entries = readStateFile(fs, options.stateFilePath);
        if (!entries) {
            return std::unexpected(std::move(entries.error()));
        }
        const std::string key = documentKey(solutionPath);
        bool replaced = false;
        for (StateEntry& entry : *entries) {
            if (documentKey(entry.solution) == key) {
                entry.project = projectPath;
                entry.external = externalConsole;
                replaced = true;
            }
        }
        if (!replaced) {
            entries->push_back({solutionPath, projectPath, externalConsole});
        }
        const std::string_view directory = io::parentPath(options.stateFilePath);
        if (!directory.empty()) {
            if (auto created = fs.createDirectories(directory); !created) {
                return std::unexpected(std::move(created.error()));
            }
        }
        return fs.writeFileAtomic(options.stateFilePath, formatStateFile(*entries));
    }

    std::optional<StateEntry> persisted(const std::string& solutionPath) {
        if (options.stateFilePath.empty()) {
            return std::nullopt;
        }
        LockGuard lock(persistMutex);
        auto entries = readStateFile(fs, options.stateFilePath);
        if (!entries) {
            return std::nullopt;
        }
        const std::string key = documentKey(solutionPath);
        for (StateEntry& entry : *entries) {
            if (documentKey(entry.solution) == key) {
                return std::move(entry);
            }
        }
        return std::nullopt;
    }

    // The checks of run() and the new run itself.
    Result<RunId> startRun(std::optional<project::ProjectId> project);
};

Result<RunId> RunService::State::startRun(std::optional<project::ProjectId> project) {
    LockGuard lock(mutex);
    if (!solution) {
        return fail(ErrorCode::InvalidArgument, "no solution is open");
    }
    if (!options.msbuildRunSupported) {
        return fail(ErrorCode::Unsupported,
                    "running MSBuild projects needs Windows; running CMake projects comes with a later milestone");
    }
    const std::optional<project::ProjectId> id = project ? project : startup;
    if (!id) {
        return fail(ErrorCode::InvalidArgument, std::format("{} has no project to run", solution->name()));
    }
    if (id->value() >= solution->projects().size()) {
        return fail(ErrorCode::InvalidArgument, std::format("project {} does not exist", id->value()));
    }
    const project::ProjectInfo& info = solution->project(*id);
    if (!isLoadedCpp(info)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{} cannot run: it is not a loaded C++ project", info.name));
    }
    for (const auto& [runId, run] : runs) {
        if (run.info.project == *id) {
            return fail(ErrorCode::Busy, std::format("{} is already running", info.name));
        }
    }
    const std::optional<std::size_t> configuration = builder.activeConfigurationIndex();
    if (!configuration || *configuration >= solution->configurations().size()) {
        return fail(ErrorCode::InvalidArgument, std::format("{} has no active configuration", solution->name()));
    }
    if (builder.building()) {
        return fail(ErrorCode::Busy, "a build is in progress; run again when it has finished");
    }
    Run run;
    run.info.id = ++lastId;
    run.info.project = *id;
    run.info.projectName = info.name;
    run.info.state = RunState::Building;
    run.info.configuration = solution->configurations()[*configuration];
    run.configurationIndex = *configuration;
    const RunId newId = run.info.id;
    emitStarted(run.info);
    runs.emplace(newId, std::move(run));
    activeCount.store(runs.size());
    post([this, newId] { beginBuild(newId); });
    return newId;
}

void RunService::State::startAgain(RunId previous, std::optional<project::ProjectId> project) {
    if (!project) {
        return;
    }
    if (auto started = startRun(project); !started) {
        emitSystem(previous, std::format("Rerun failed: {}", started.error().message()));
    }
}

std::unique_ptr<RunService> RunService::create(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                               project::TrustStore& trustStore, IRunBuilder& builder,
                                               RunServiceOptions options, RunServiceCallbacks callbacks) {
    auto state = std::make_shared<State>(fs, launcher, trustStore, builder, std::move(options), std::move(callbacks));
    {
        LockGuard lock(state->mutex);
        state->external = state->options.externalConsoleByDefault;
    }
    State* raw = state.get();
    state->worker = std::thread([raw] { raw->workerLoop(); });
    return std::unique_ptr<RunService>(new RunService(std::move(state)));
}

Result<std::string> RunService::defaultStatePath() {
    auto buildState = BuildService::defaultStatePath();
    if (!buildState) {
        return std::unexpected(std::move(buildState.error()));
    }
    return io::joinPath(io::parentPath(*buildState), kStateFileName);
}

RunService::RunService(std::shared_ptr<State> state) noexcept : m_state(std::move(state)) {}

RunService::~RunService() {
    std::vector<std::unique_ptr<RunSession>> sessions;
    {
        LockGuard lock(m_state->mutex);
        m_state->closing = true;
        for (auto& [id, run] : m_state->runs) {
            if (run.session) {
                sessions.push_back(std::move(run.session));
            }
        }
        m_state->runs.clear();
        m_state->activeCount.store(0);
    }
    for (std::unique_ptr<RunSession>& session : sessions) {
        session->stop();
        session.reset(); // Waits for the program's tree to end and the session's threads.
    }
    {
        LockGuard lock(m_state->queueMutex);
        m_state->stopping = true;
    }
    m_state->queueChanged.notify_all();
    if (m_state->worker.joinable()) {
        m_state->worker.join();
    }
    LockGuard lock(m_state->queueMutex);
    m_state->tasks.clear();
}

Result<void> RunService::openSolution(std::shared_ptr<const project::SolutionModel> solution) {
    if (!solution) {
        return fail(ErrorCode::InvalidArgument, "openSolution needs a solution");
    }
    const std::optional<StateEntry> saved = m_state->persisted(solution->path());
    const std::optional<std::size_t> configuration = m_state->builder.activeConfigurationIndex();
    LockGuard lock(m_state->mutex);
    if (!m_state->runs.empty()) {
        return fail(ErrorCode::Busy, "a program is running; stop it before opening another solution");
    }
    std::optional<project::ProjectId> startup;
    if (saved && !saved->project.empty()) {
        if (auto found = solution->findProjectByPath(saved->project); found && isLoadedCpp(solution->project(*found))) {
            startup = found;
        }
    }
    if (!startup) {
        startup = defaultStartupProject(*solution, configuration);
    }
    m_state->solution = std::move(solution);
    m_state->startup = startup;
    m_state->external = saved && saved->external ? *saved->external : m_state->options.externalConsoleByDefault;
    m_state->cache.clear();
    m_state->emitStartup(startup);
    m_state->emitExternal(m_state->external);
    return {};
}

Result<void> RunService::closeSolution() {
    LockGuard lock(m_state->mutex);
    if (!m_state->runs.empty()) {
        return fail(ErrorCode::Busy, "a program is running; stop it before closing the solution");
    }
    if (!m_state->solution) {
        return {};
    }
    m_state->solution.reset();
    m_state->startup.reset();
    m_state->cache.clear();
    m_state->emitStartup(std::nullopt);
    return {};
}

std::shared_ptr<const project::SolutionModel> RunService::solution() const {
    LockGuard lock(m_state->mutex);
    return m_state->solution;
}

std::vector<project::ProjectId> RunService::startupCandidates() const {
    LockGuard lock(m_state->mutex);
    std::vector<project::ProjectId> candidates;
    if (!m_state->solution) {
        return candidates;
    }
    const auto projects = m_state->solution->projects();
    for (std::uint32_t i = 0; i < projects.size(); ++i) {
        if (isLoadedCpp(projects[i])) {
            candidates.push_back(project::ProjectId{i});
        }
    }
    return candidates;
}

std::optional<project::ProjectId> RunService::startupProject() const {
    LockGuard lock(m_state->mutex);
    return m_state->startup;
}

Result<void> RunService::setStartupProject(project::ProjectId project) {
    std::string solutionPath;
    std::string projectPath;
    bool externalConsole = true;
    {
        LockGuard lock(m_state->mutex);
        if (!m_state->solution) {
            return fail(ErrorCode::InvalidArgument, "no solution is open");
        }
        if (project.value() >= m_state->solution->projects().size()) {
            return fail(ErrorCode::InvalidArgument, std::format("project {} does not exist", project.value()));
        }
        const project::ProjectInfo& info = m_state->solution->project(project);
        if (!isLoadedCpp(info)) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("{} cannot be the startup project: it is not a loaded C++ project", info.name));
        }
        if (m_state->startup == project) {
            return {};
        }
        m_state->startup = project;
        m_state->emitStartup(project);
        solutionPath = m_state->solution->path();
        projectPath = info.path;
        externalConsole = m_state->external;
    }
    return m_state->persist(solutionPath, projectPath, externalConsole);
}

bool RunService::useExternalConsole() const {
    LockGuard lock(m_state->mutex);
    return m_state->external;
}

Result<void> RunService::setUseExternalConsole(bool external) {
    std::string solutionPath;
    std::string projectPath;
    {
        LockGuard lock(m_state->mutex);
        if (m_state->external == external) {
            return {};
        }
        m_state->external = external;
        m_state->emitExternal(external);
        if (!m_state->solution) {
            return {};
        }
        solutionPath = m_state->solution->path();
        if (m_state->startup) {
            projectPath = m_state->solution->project(*m_state->startup).path;
        }
    }
    return m_state->persist(solutionPath, projectPath, external);
}

Result<RunId> RunService::run(std::optional<project::ProjectId> project) { return m_state->startRun(project); }

Result<RunId> RunService::rerun(std::optional<project::ProjectId> project) {
    std::optional<RunId> active;
    {
        LockGuard lock(m_state->mutex);
        const std::optional<project::ProjectId> id = project ? project : m_state->startup;
        if (id) {
            for (auto& [runId, run] : m_state->runs) {
                if (run.info.project == *id) {
                    run.rerunPending = true;
                    active = runId;
                }
            }
        }
    }
    if (!active) {
        return run(project);
    }
    stop(*active);
    return *active;
}

void RunService::stop(RunId id) {
    bool cancelBuild = false;
    {
        LockGuard lock(m_state->mutex);
        State::Run* run = m_state->findLocked(id);
        if (run == nullptr) {
            return;
        }
        run->stopRequested = true;
        switch (run->info.state) {
        case RunState::Building: cancelBuild = true; break;
        case RunState::Resolving: break; // Checked before the launch.
        case RunState::Running:
            if (run->session) {
                m_state->setStateLocked(*run, RunState::Stopping);
                run->session->stop();
            }
            break;
        case RunState::Stopping:
        case RunState::Finished: break;
        }
    }
    if (cancelBuild) {
        m_state->builder.cancelBuild(); // Its outcome arrives through afterBuild.
    }
}

void RunService::stopAll() {
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

Result<void> RunService::sendInput(RunId id, std::string bytes) {
    LockGuard lock(m_state->mutex);
    State::Run* run = m_state->findLocked(id);
    if (run == nullptr) {
        return fail(ErrorCode::NotFound, std::format("run {} has ended", id));
    }
    if (!run->session) {
        return fail(ErrorCode::NotFound, std::format("{} has not started yet", run->info.projectName));
    }
    return run->session->writeInput(std::move(bytes));
}

void RunService::closeInput(RunId id) {
    LockGuard lock(m_state->mutex);
    if (State::Run* run = m_state->findLocked(id); run != nullptr && run->session) {
        run->session->closeInput();
        run->info.acceptsInput = false;
    }
}

std::optional<RunInfo> RunService::runInfo(RunId id) const {
    LockGuard lock(m_state->mutex);
    if (const auto it = m_state->runs.find(id); it != m_state->runs.end()) {
        RunInfo info = it->second.info;
        info.acceptsInput = it->second.session && it->second.session->acceptsInput();
        return info;
    }
    if (m_state->last && m_state->last->run.id == id) {
        return m_state->last->run;
    }
    return std::nullopt;
}

std::vector<RunInfo> RunService::activeRuns() const {
    LockGuard lock(m_state->mutex);
    std::vector<RunInfo> infos;
    for (const auto& [id, run] : m_state->runs) {
        infos.push_back(run.info);
        infos.back().acceptsInput = run.session && run.session->acceptsInput();
    }
    return infos;
}

std::optional<RunId> RunService::activeRun(project::ProjectId project) const {
    LockGuard lock(m_state->mutex);
    for (const auto& [id, run] : m_state->runs) {
        if (run.info.project == project) {
            return id;
        }
    }
    return std::nullopt;
}

std::optional<RunOutcome> RunService::lastOutcome() const {
    LockGuard lock(m_state->mutex);
    return m_state->last;
}

void RunService::invalidateProperties() {
    LockGuard lock(m_state->mutex);
    m_state->cache.clear();
}

bool RunService::waitForIdle(IClock::Duration timeout) const {
    const State& s = *m_state;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    UniqueLock lock(s.queueMutex);
    for (;;) {
        if (s.tasks.empty() && !s.taskRunning && s.activeCount.load() == 0) {
            return true;
        }
        if (s.queueChanged.wait_until(lock.native(), deadline) == std::cv_status::timeout) {
            return s.tasks.empty() && !s.taskRunning && s.activeCount.load() == 0;
        }
    }
}

} // namespace frost::build
