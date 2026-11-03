// SPDX-License-Identifier: Apache-2.0
//
// BuildService (see frost/build/build_service.hpp): the service thread and its task queue, the
// build pipeline (trust, Save All, MSBuild), the persisted active configurations and the
// diagnostics of the last build.

#include "frost/build/build_service.hpp"

#include "frost/build/msbuild_locator.hpp"
#include "frost/core/assert.hpp"
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
#include <set>
#include <span>
#include <thread>
#include <utility>

namespace frost::build {

namespace {

constexpr std::string_view kStateHeaderPrefix = "# FrostIDE active configurations v";
constexpr int kStateVersion = 1;
constexpr std::string_view kStateFileName = "active-configurations.txt";

constexpr char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool startsWithIgnoringCase(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (lowerAscii(text[i]) != lowerAscii(prefix[i])) {
            return false;
        }
    }
    return true;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = lowerAscii(c);
    }
    return out;
}

// One persisted choice: a solution path and its active configuration name ("Debug|x64").
struct StateEntry {
    std::string solution;
    std::string configuration;
};

// Reads the state file (build_service.hpp, "Active configuration"); a missing file has no entries.
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
        text.remove_prefix(3); // A BOM an editor added.
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
        const std::size_t tab = line.rfind('\t');
        if (tab == std::string_view::npos || tab == 0 || tab + 1 == line.size()) {
            continue; // Not "<solution>\t<configuration>": skipped.
        }
        entries.push_back({std::string(line.substr(0, tab)), std::string(line.substr(tab + 1))});
    }
    if (first) {
        return fail(ErrorCode::Corrupt, std::format("{}: the header line is missing", path));
    }
    return entries;
}

std::string formatStateFile(const std::vector<StateEntry>& entries) {
    std::string text = std::format("{}{}\n", kStateHeaderPrefix, kStateVersion);
    for (const StateEntry& entry : entries) {
        text += std::format("{}\t{}\n", entry.solution, entry.configuration);
    }
    return text;
}

// The default active configuration: the first named Debug..., else the first.
std::size_t defaultConfiguration(std::span<const project::SolutionConfiguration> configurations) noexcept {
    for (std::size_t i = 0; i < configurations.size(); ++i) {
        if (startsWithIgnoringCase(configurations[i].configuration, "Debug")) {
            return i;
        }
    }
    return 0;
}

bool buildsIn(const project::SolutionModel& solution, project::ProjectId project, std::size_t configuration) {
    const auto& mapping = solution.projectConfiguration(project, configuration);
    return mapping && mapping->build;
}

// How many projects a build of `project` (or of the solution) is expected to build.
std::size_t expectedProjects(const project::SolutionModel& solution, std::size_t configuration,
                             std::optional<project::ProjectId> project) {
    const auto projects = solution.projects();
    std::size_t count = 0;
    if (!project) {
        for (std::uint32_t i = 0; i < projects.size(); ++i) {
            count += buildsIn(solution, project::ProjectId{i}, configuration) ? 1 : 0;
        }
        return count;
    }
    // The project and everything it depends on, transitively.
    std::vector<bool> seen(projects.size(), false);
    std::vector<project::ProjectId> pending{*project};
    seen[project->value()] = true;
    while (!pending.empty()) {
        const project::ProjectId next = pending.back();
        pending.pop_back();
        count += buildsIn(solution, next, configuration) ? 1 : 0;
        for (const project::ProjectId dependency : solution.project(next).dependencies) {
            if (!seen[dependency.value()]) {
                seen[dependency.value()] = true;
                pending.push_back(dependency);
            }
        }
    }
    return count;
}

std::string plural(std::size_t count, std::string_view word) {
    return std::format("{} {}{}", count, word, count == 1 ? "" : "s");
}

std::string seconds(IClock::Duration duration) {
    return std::format("{:.1f} s", std::chrono::duration<double>(duration).count());
}

// "Build started: project Lib (Debug|x64) of solution Tiny" and the like.
std::string startLine(const BuildTarget& target, const project::SolutionModel& solution) {
    std::string what = target.project ? std::format("project {} of solution {}", target.projectName, solution.name())
                                      : std::format("solution {}", solution.name());
    std::string line =
        std::format("{} started: {}, configuration {}", toString(target.action), what, target.configuration.name());
    if (target.projectConfiguration) {
        line += std::format(" (project {}|{})", target.projectConfiguration->configuration,
                            target.projectConfiguration->platform);
    }
    return line;
}

// "Build succeeded: 0 errors, 1 warning (1.2 s)" and the like.
std::string summaryLine(const BuildOutcome& outcome) {
    const std::string_view action = toString(outcome.target.action);
    if (outcome.error) {
        return std::format("{} not started: {}", action, outcome.error->message());
    }
    if (outcome.result.cancelled) {
        return std::format("{} cancelled ({})", action, seconds(outcome.result.duration));
    }
    return std::format("{} {}: {}, {} ({})", action, outcome.result.success ? "succeeded" : "failed",
                       plural(outcome.counts.errors, "error"), plural(outcome.counts.warnings, "warning"),
                       seconds(outcome.result.duration));
}

} // namespace

std::string_view toString(BuildState state) noexcept {
    switch (state) {
    case BuildState::Idle: return "Idle";
    case BuildState::NeedsTrust: return "NeedsTrust";
    case BuildState::SavingDocuments: return "SavingDocuments";
    case BuildState::Starting: return "Starting";
    case BuildState::Running: return "Running";
    case BuildState::Cancelling: return "Cancelling";
    }
    return "Idle";
}

std::string solutionProjectName(const project::SolutionModel& solution, project::ProjectId id) {
    FROST_PRE(id.value() < solution.projects().size());
    const project::ProjectInfo& info = solution.project(id);
    std::string name = info.name;
    std::optional<project::NodeId> parent = solution.parent(info.node);
    while (parent && *parent != project::SolutionModel::root()) {
        const project::Node& folder = solution.node(*parent);
        if (folder.kind != project::NodeKind::SolutionFolder) {
            break;
        }
        name = folder.displayName + '\\' + name;
        parent = folder.parent;
    }
    return name;
}

struct BuildService::State : std::enable_shared_from_this<BuildService::State> {
    State(io::IFileSystem& fileSystem, io::IProcessLauncher& processLauncher, project::TrustStore& store,
          BuildServiceOptions serviceOptions, BuildServiceCallbacks serviceCallbacks)
        : fs(fileSystem), launcher(processLauncher), trustStore(store), options(std::move(serviceOptions)),
          callbacks(std::move(serviceCallbacks)) {
        if (!options.startBuild) {
            options.startBuild = [this](const MsBuildRequest& request, BuildCallbacks session) {
                return startMsBuild(launcher, request, std::move(session));
            };
        }
    }

    // One build, from build() to its outcome.
    struct Run {
        std::uint64_t id = 0;
        BuildTarget target{};
        std::shared_ptr<const project::SolutionModel> solution{};
        std::vector<std::string> untrusted{};    // While NeedsTrust.
        std::unique_ptr<BuildSession> session{}; // While Running and Cancelling.
        std::set<std::string> finished{};        // Lower-cased project files that reported their output.
        BuildCompletion onDone{};                // build()'s own completion, after onFinished.
    };

    io::IFileSystem& fs;
    io::IProcessLauncher& launcher;
    project::TrustStore& trustStore;
    BuildServiceOptions options; // Immutable after construction.
    const BuildServiceCallbacks callbacks;

    mutable Mutex mutex;
    std::shared_ptr<const project::SolutionModel> solution FROST_GUARDED_BY(mutex);
    std::optional<std::size_t> active FROST_GUARDED_BY(mutex);
    BuildState state FROST_GUARDED_BY(mutex) = BuildState::Idle;
    bool projectEditing FROST_GUARDED_BY(mutex) = false;
    std::size_t activeRuns FROST_GUARDED_BY(mutex) = 0;
    std::optional<Run> run FROST_GUARDED_BY(mutex);
    std::uint64_t lastRunId FROST_GUARDED_BY(mutex) = 0;
    DiagnosticIndex diagnostics FROST_GUARDED_BY(mutex);
    BuildProgress progress FROST_GUARDED_BY(mutex);
    std::optional<BuildOutcome> last FROST_GUARDED_BY(mutex);
    std::string msbuildPath FROST_GUARDED_BY(mutex);
    SaveAllHook saveAll FROST_GUARDED_BY(mutex);
    // `state` for waitForIdle, which must not take `mutex` (it holds queueMutex).
    std::atomic<BuildState> publishedState{BuildState::Idle};

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
                    return; // The rest is dropped (build_service.hpp, "Lifetime").
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

    void emitState(BuildState value) {
        post([this, value] {
            if (callbacks.onStateChanged) {
                callbacks.onStateChanged(value);
            }
        });
    }

    void emitOutput(BuildOutputKind kind, std::string line) {
        post([this, kind, line = std::move(line)] {
            if (callbacks.onOutput) {
                callbacks.onOutput(kind, line);
            }
        });
    }

    void emitNeedsTrust(std::vector<std::string> roots) {
        post([this, roots = std::move(roots)] {
            if (callbacks.onNeedsTrust) {
                callbacks.onNeedsTrust(roots);
            }
        });
    }

    void emitProgress(BuildProgress value) {
        post([this, value = std::move(value)] {
            if (callbacks.onProgress) {
                callbacks.onProgress(value);
            }
        });
    }

    void emitDiagnosticsCleared() {
        post([this] {
            if (callbacks.onDiagnosticsCleared) {
                callbacks.onDiagnosticsCleared();
            }
        });
    }

    void emitActiveConfiguration(project::SolutionConfiguration configuration) {
        post([this, configuration = std::move(configuration)] {
            if (callbacks.onActiveConfigurationChanged) {
                callbacks.onActiveConfigurationChanged(configuration);
            }
        });
    }

    void setStateLocked(BuildState value) FROST_REQUIRES(mutex) {
        state = value;
        publishedState.store(value);
        emitState(value);
    }

    // ---- The pipeline.

    // The folders of `model` the store does not trust; empty when checkBuildTrust passes.
    std::vector<std::string> untrustedRootsOf(const project::SolutionModel& model) const {
        if (project::checkBuildTrust(trustStore, model)) {
            return {};
        }
        std::vector<std::string> roots;
        for (std::string& root : project::buildTrustRoots(model)) {
            if (!trustStore.isTrusted(root)) {
                roots.push_back(std::move(root));
            }
        }
        if (roots.empty()) {
            roots.push_back(model.directory()); // The gate failed for a reason the list does not show.
        }
        return roots;
    }

    // Checks trust for the run: true when trusted; otherwise the run waits in NeedsTrust.
    bool passTrustGateLocked() FROST_REQUIRES(mutex) {
        std::vector<std::string> roots = untrustedRootsOf(*run->solution);
        if (roots.empty()) {
            return true;
        }
        run->untrusted = roots;
        setStateLocked(BuildState::NeedsTrust);
        emitNeedsTrust(std::move(roots));
        return false;
    }

    // After the trust gate: SavingDocuments, and the Save-All step on the service thread.
    void startSavingLocked() FROST_REQUIRES(mutex) {
        run->untrusted.clear();
        setStateLocked(BuildState::SavingDocuments);
        const std::uint64_t id = run->id;
        post([this, id] { saveDocuments(id); });
    }

    bool isCurrentLocked(std::uint64_t id) const FROST_REQUIRES(mutex) { return run && run->id == id; }

    // Publishes how the run ended and returns to Idle. The session (if any) must be gone or
    // moved out by the caller.
    void finishLocked(BuildOutcome outcome) FROST_REQUIRES(mutex) {
        emitOutput(BuildOutputKind::Status, summaryLine(outcome));
        post([this, outcome, onDone = std::move(run->onDone)] {
            if (callbacks.onFinished) {
                callbacks.onFinished(outcome);
            }
            if (onDone) {
                onDone(outcome);
            }
        });
        last = std::move(outcome);
        run.reset();
        setStateLocked(BuildState::Idle);
    }

    BuildOutcome outcomeLocked() const FROST_REQUIRES(mutex) {
        BuildOutcome outcome;
        outcome.target = run->target;
        outcome.counts = diagnostics.counts();
        return outcome;
    }

    void failLocked(Error error) FROST_REQUIRES(mutex) {
        BuildOutcome outcome = outcomeLocked();
        outcome.error = std::move(error);
        finishLocked(std::move(outcome));
    }

    // Service thread: calls the Save-All hook and waits for its completion.
    void saveDocuments(std::uint64_t id) {
        SaveAllHook hook;
        {
            LockGuard lock(mutex);
            if (!isCurrentLocked(id) || state != BuildState::SavingDocuments) {
                return;
            }
            hook = saveAll;
        }
        if (!hook) {
            launch(id, {});
            return;
        }
        auto called = std::make_shared<std::atomic<bool>>(false);
        hook([weak = weak_from_this(), id, called](Result<void> saved) {
            if (called->exchange(true)) {
                return; // Only the first completion counts.
            }
            if (const auto self = weak.lock()) {
                State* raw = self.get();
                raw->post([raw, id, saved = std::move(saved)]() mutable { raw->launch(id, std::move(saved)); });
            }
        });
    }

    // Service thread: after Save All, finds MSBuild and starts the build.
    void launch(std::uint64_t id, Result<void> saved) {
        std::string msbuild;
        {
            LockGuard lock(mutex);
            if (!isCurrentLocked(id) || state != BuildState::SavingDocuments) {
                return;
            }
            if (!saved) {
                failLocked(std::move(saved.error()).withContext("while saving the documents before the build"));
                return;
            }
            setStateLocked(BuildState::Starting);
            msbuild = msbuildPath.empty() ? options.msbuildPath : msbuildPath;
        }
        if (msbuild.empty()) {
            auto found = findMsBuild(launcher, fs, {.vswherePath = {}, .instanceId = options.vsInstanceId});
            LockGuard lock(mutex);
            if (!isCurrentLocked(id)) {
                return;
            }
            if (!found) {
                failLocked(std::move(found.error()).withContext("while looking for MSBuild"));
                return;
            }
            msbuildPath = found->msbuildPath;
            msbuild = msbuildPath;
        }

        MsBuildRequest request;
        BuildCallbacks sessionCallbacks;
        {
            LockGuard lock(mutex);
            if (!isCurrentLocked(id)) {
                return;
            }
            // The trust may have been taken back (another window) since build(): check again.
            if (!passTrustGateLocked()) {
                return;
            }
            const BuildTarget& target = run->target;
            request = MsBuildRequest{.msbuildPath = msbuild,
                                     .solutionPath = run->solution->path(),
                                     .projectName = target.projectName,
                                     .projectPath = {},
                                     .configuration = target.configuration.configuration,
                                     .platform = target.configuration.platform,
                                     .action = target.action,
                                     .trusted = true,
                                     .maxCpuCount = options.maxCpuCount,
                                     .verbosity = MsBuildVerbosity::Minimal,
                                     .properties = {},
                                     .uiLanguage = {}};
            emitOutput(BuildOutputKind::Status, startLine(target, *run->solution));
            sessionCallbacks = makeSessionCallbacks(id, run->solution->directory());
        }

        auto session = options.startBuild(request, std::move(sessionCallbacks));
        std::unique_ptr<BuildSession> stale;
        {
            LockGuard lock(mutex);
            if (!isCurrentLocked(id)) {
                if (session) {
                    stale = std::move(*session); // Cancelled while it started.
                }
            } else if (!session) {
                failLocked(std::move(session.error()));
            } else {
                run->session = std::move(*session);
                setStateLocked(BuildState::Running);
            }
        }
        // Outside the lock: the session's callbacks take it, and its destructor waits for them.
        if (stale) {
            stale->cancel();
            stale.reset();
        }
    }

    BuildCallbacks makeSessionCallbacks(std::uint64_t id, const std::string& solutionDirectory) {
        auto resolver = std::make_shared<DiagnosticResolver>(fs, solutionDirectory);
        return {
            .onLine = [this, id](OutputStream stream, std::string_view line) { onSessionLine(id, stream, line); },
            .onDiagnostic =
                [this, id, resolver](const Diagnostic& diagnostic) {
                    onSessionDiagnostic(id, resolver->resolve(diagnostic));
                },
            .onFinished = [this,
                           id](const BuildResult& result) { post([this, id, result] { finishBuild(id, result); }); },
        };
    }

    // Session threads (serialized by the session).
    void onSessionLine(std::uint64_t id, OutputStream stream, std::string_view line) {
        const std::optional<ProjectOutputLine> output = parseProjectOutputLine(line);
        LockGuard lock(mutex);
        if (!isCurrentLocked(id)) {
            return;
        }
        emitOutput(stream == OutputStream::Stdout ? BuildOutputKind::Stdout : BuildOutputKind::Stderr,
                   std::string(line));
        if (output && run->finished.insert(lowered(output->projectFile)).second) {
            ++progress.finishedProjects;
            progress.expectedProjects = std::max(progress.expectedProjects, progress.finishedProjects);
            const std::size_t dot = output->projectFile.rfind('.');
            progress.lastProject = output->projectFile.substr(0, dot);
            progress.lastOutput = output->output;
            emitProgress(progress);
        }
    }

    void onSessionDiagnostic(std::uint64_t id, BuildDiagnostic diagnostic) {
        LockGuard lock(mutex);
        if (!isCurrentLocked(id)) {
            return;
        }
        diagnostics.add(diagnostic);
        post([this, diagnostic = std::move(diagnostic)] {
            if (callbacks.onDiagnostic) {
                callbacks.onDiagnostic(diagnostic);
            }
        });
    }

    // Service thread: the session published its result.
    void finishBuild(std::uint64_t id, const BuildResult& result) {
        std::unique_ptr<BuildSession> session;
        {
            LockGuard lock(mutex);
            if (!isCurrentLocked(id)) {
                return;
            }
            session = std::move(run->session);
            BuildOutcome outcome = outcomeLocked();
            outcome.result = result;
            finishLocked(std::move(outcome));
        }
        session.reset(); // Joins the session's waiter, which has just returned from onFinished.
    }

    // Writes `configuration` as the persisted choice for `solutionPath`.
    Result<void> persist(const std::string& solutionPath, const std::string& configuration) {
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
                entry.configuration = configuration;
                replaced = true;
            }
        }
        if (!replaced) {
            entries->push_back({solutionPath, configuration});
        }
        const std::string_view directory = io::parentPath(options.stateFilePath);
        if (!directory.empty()) {
            if (auto created = fs.createDirectories(directory); !created) {
                return std::unexpected(std::move(created.error()));
            }
        }
        return fs.writeFileAtomic(options.stateFilePath, formatStateFile(*entries));
    }

    // The persisted configuration name for `solutionPath`, if any (errors leave it unset).
    std::optional<std::string> persistedConfiguration(const std::string& solutionPath) {
        if (options.stateFilePath.empty()) {
            return std::nullopt;
        }
        LockGuard lock(persistMutex);
        auto entries = readStateFile(fs, options.stateFilePath);
        if (!entries) {
            return std::nullopt;
        }
        const std::string key = documentKey(solutionPath);
        for (const StateEntry& entry : *entries) {
            if (documentKey(entry.solution) == key) {
                return entry.configuration;
            }
        }
        return std::nullopt;
    }
};

std::unique_ptr<BuildService> BuildService::create(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                   project::TrustStore& trustStore, BuildServiceOptions options,
                                                   BuildServiceCallbacks callbacks) {
    auto state = std::make_shared<State>(fs, launcher, trustStore, std::move(options), std::move(callbacks));
    State* raw = state.get();
    state->worker = std::thread([raw] { raw->workerLoop(); });
    return std::unique_ptr<BuildService>(new BuildService(std::move(state)));
}

Result<std::string> BuildService::defaultStatePath() {
    auto trustPath = project::TrustStore::defaultStorePath();
    if (!trustPath) {
        return std::unexpected(std::move(trustPath.error()));
    }
    return io::joinPath(io::parentPath(*trustPath), kStateFileName);
}

BuildService::BuildService(std::shared_ptr<State> state) noexcept : m_state(std::move(state)) {}

BuildService::~BuildService() {
    std::unique_ptr<BuildSession> session;
    {
        LockGuard lock(m_state->mutex);
        if (m_state->run) {
            session = std::move(m_state->run->session);
            m_state->run.reset();
        }
        m_state->state = BuildState::Idle;
        m_state->publishedState.store(BuildState::Idle);
    }
    if (session) {
        session->cancel();
        session.reset(); // Waits for MSBuild's tree to end and the session's threads.
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

Result<void> BuildService::beginProjectEdit() {
    LockGuard lock(m_state->mutex);
    if (m_state->projectEditing || m_state->state != BuildState::Idle || m_state->activeRuns != 0)
        return fail(ErrorCode::Busy, "a build or project edit is in progress");
    if (!m_state->solution)
        return fail(ErrorCode::InvalidArgument, "no solution is open");
    m_state->projectEditing = true;
    return {};
}

Result<void> BuildService::reserveRun() {
    LockGuard lock(m_state->mutex);
    if (m_state->projectEditing || m_state->state != BuildState::Idle)
        return fail(ErrorCode::Busy, "a build or project edit is in progress");
    ++m_state->activeRuns;
    return {};
}

void BuildService::releaseRun() {
    LockGuard lock(m_state->mutex);
    if (m_state->activeRuns != 0)
        --m_state->activeRuns;
}

Result<void> BuildService::finishProjectEdit(std::shared_ptr<const project::SolutionModel> solution) {
    LockGuard lock(m_state->mutex);
    if (!m_state->projectEditing || !solution || !m_state->solution || solution->path() != m_state->solution->path())
        return fail(ErrorCode::InvalidArgument, "the edited solution changed");
    std::optional<std::string> activeName;
    if (m_state->active && *m_state->active < m_state->solution->configurations().size())
        activeName = m_state->solution->configurations()[*m_state->active].name();
    m_state->solution = std::move(solution);
    m_state->active = activeName ? m_state->solution->findConfiguration(*activeName) : std::nullopt;
    if (!m_state->active && !m_state->solution->configurations().empty())
        m_state->active = defaultConfiguration(m_state->solution->configurations());
    m_state->diagnostics.clear();
    m_state->progress = {};
    m_state->last.reset();
    m_state->emitDiagnosticsCleared();
    if (m_state->active)
        m_state->emitActiveConfiguration(m_state->solution->configurations()[*m_state->active]);
    m_state->projectEditing = false;
    return {};
}

void BuildService::cancelProjectEdit() {
    LockGuard lock(m_state->mutex);
    m_state->projectEditing = false;
}

bool BuildService::projectEditInProgress() const {
    LockGuard lock(m_state->mutex);
    return m_state->projectEditing;
}

Result<void> BuildService::openSolution(std::shared_ptr<const project::SolutionModel> solution) {
    if (!solution) {
        return fail(ErrorCode::InvalidArgument, "openSolution needs a solution");
    }
    const std::optional<std::string> persisted = m_state->persistedConfiguration(solution->path());
    LockGuard lock(m_state->mutex);
    if (m_state->projectEditing)
        return fail(ErrorCode::Busy, "a project edit is in progress");
    if (m_state->state != BuildState::Idle) {
        return fail(ErrorCode::Busy, "a build is in progress; cancel it before opening another solution");
    }
    const auto configurations = solution->configurations();
    std::optional<std::size_t> active;
    if (persisted) {
        active = solution->findConfiguration(*persisted);
    }
    if (!active && !configurations.empty()) {
        active = defaultConfiguration(configurations);
    }
    m_state->solution = std::move(solution);
    m_state->active = active;
    m_state->diagnostics.clear();
    m_state->progress = {};
    m_state->last.reset();
    m_state->emitDiagnosticsCleared();
    if (active) {
        m_state->emitActiveConfiguration(configurations[*active]);
    }
    return {};
}

Result<void> BuildService::closeSolution() {
    LockGuard lock(m_state->mutex);
    if (m_state->projectEditing)
        return fail(ErrorCode::Busy, "a project edit is in progress");
    if (m_state->state != BuildState::Idle) {
        return fail(ErrorCode::Busy, "a build is in progress; cancel it before closing the solution");
    }
    if (!m_state->solution) {
        return {};
    }
    m_state->solution.reset();
    m_state->active.reset();
    m_state->diagnostics.clear();
    m_state->progress = {};
    m_state->last.reset();
    m_state->emitDiagnosticsCleared();
    return {};
}

std::shared_ptr<const project::SolutionModel> BuildService::solution() const {
    LockGuard lock(m_state->mutex);
    return m_state->solution;
}

std::vector<project::SolutionConfiguration> BuildService::configurations() const {
    LockGuard lock(m_state->mutex);
    if (!m_state->solution) {
        return {};
    }
    const auto configurations = m_state->solution->configurations();
    return {configurations.begin(), configurations.end()};
}

std::optional<std::size_t> BuildService::activeConfigurationIndex() const {
    LockGuard lock(m_state->mutex);
    return m_state->active;
}

std::optional<project::SolutionConfiguration> BuildService::activeConfiguration() const {
    LockGuard lock(m_state->mutex);
    if (!m_state->solution || !m_state->active) {
        return std::nullopt;
    }
    return m_state->solution->configurations()[*m_state->active];
}

Result<void> BuildService::setActiveConfiguration(std::size_t index) {
    std::string solutionPath;
    std::string name;
    {
        LockGuard lock(m_state->mutex);
        if (m_state->projectEditing)
            return fail(ErrorCode::Busy, "a project edit is in progress");
        if (!m_state->solution) {
            return fail(ErrorCode::InvalidArgument, "no solution is open");
        }
        if (m_state->state != BuildState::Idle) {
            return fail(ErrorCode::Busy, "the configuration cannot change while a build is in progress");
        }
        const auto configurations = m_state->solution->configurations();
        if (index >= configurations.size()) {
            return fail(ErrorCode::OutOfRange,
                        std::format("configuration {} of {} does not exist", index, configurations.size()));
        }
        if (m_state->active == index) {
            return {};
        }
        m_state->active = index;
        name = configurations[index].name();
        solutionPath = m_state->solution->path();
        m_state->emitActiveConfiguration(configurations[index]);
    }
    return m_state->persist(solutionPath, name);
}

Result<void> BuildService::setActiveConfiguration(std::string_view name) {
    std::optional<std::size_t> index;
    {
        LockGuard lock(m_state->mutex);
        if (!m_state->solution) {
            return fail(ErrorCode::InvalidArgument, "no solution is open");
        }
        index = m_state->solution->findConfiguration(name);
    }
    if (!index) {
        return fail(ErrorCode::NotFound, std::format("the solution has no configuration '{}'", name));
    }
    return setActiveConfiguration(*index);
}

void BuildService::setSaveAllHook(SaveAllHook hook) {
    LockGuard lock(m_state->mutex);
    m_state->saveAll = std::move(hook);
}

Result<void> BuildService::build(BuildAction action, std::optional<project::ProjectId> project,
                                 BuildCompletion onDone) {
    State& s = *m_state;
    LockGuard lock(s.mutex);
    if (s.projectEditing)
        return fail(ErrorCode::Busy, "a project edit is in progress");
    if (s.state != BuildState::Idle) {
        return fail(ErrorCode::Busy, std::format("a build is already in progress ({})", toString(s.state)));
    }
    if (!s.solution) {
        return fail(ErrorCode::InvalidArgument, "no solution is open");
    }
    if (!s.active) {
        return fail(ErrorCode::InvalidArgument, std::format("{} has no solution configurations", s.solution->name()));
    }
    const project::SolutionModel& solution = *s.solution;
    BuildTarget target{.action = action,
                       .project = std::nullopt,
                       .projectName = {},
                       .configuration = solution.configurations()[*s.active],
                       .projectConfiguration = std::nullopt};
    if (project) {
        if (project->value() >= solution.projects().size()) {
            return fail(ErrorCode::InvalidArgument, std::format("project {} does not exist", project->value()));
        }
        const project::ProjectInfo& info = solution.project(*project);
        if (info.path.empty() || info.nodeKind == project::NodeKind::UnavailableProject) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("{} cannot be built: its project file is missing or unreadable", info.name));
        }
        const auto& mapping = solution.projectConfiguration(*project, *s.active);
        if (!mapping || !mapping->build) {
            return fail(ErrorCode::Unsupported,
                        std::format("{} is not built in {}", info.name, target.configuration.name()));
        }
        target.project = project;
        target.projectName = solutionProjectName(solution, *project);
        target.projectConfiguration = mapping;
    }
    s.diagnostics.clear();
    s.progress = BuildProgress{.finishedProjects = 0,
                               .expectedProjects = expectedProjects(solution, *s.active, project),
                               .lastProject = {},
                               .lastOutput = {}};
    s.run = State::Run{.id = ++s.lastRunId,
                       .target = std::move(target),
                       .solution = s.solution,
                       .untrusted = {},
                       .session = nullptr,
                       .finished = {},
                       .onDone = std::move(onDone)};
    s.emitDiagnosticsCleared();
    s.emitProgress(s.progress);
    if (s.passTrustGateLocked()) {
        s.startSavingLocked();
    }
    return {};
}

void BuildService::cancel() {
    State& s = *m_state;
    LockGuard lock(s.mutex);
    switch (s.state) {
    case BuildState::Idle:
    case BuildState::Cancelling: return;
    case BuildState::Running:
        if (s.run && s.run->session) {
            s.setStateLocked(BuildState::Cancelling);
            s.run->session->cancel(); // Its result arrives through finishBuild.
        }
        return;
    case BuildState::NeedsTrust:
    case BuildState::SavingDocuments:
    case BuildState::Starting: {
        // Nothing runs yet: the build ends here, and a launch in flight is cancelled when it returns.
        BuildOutcome outcome = s.outcomeLocked();
        outcome.result.cancelled = true;
        s.finishLocked(std::move(outcome));
        return;
    }
    }
}

Result<void> BuildService::trust(std::string_view root) {
    State& s = *m_state;
    if (auto trusted = s.trustStore.trust(root); !trusted) {
        return std::unexpected(std::move(trusted.error()));
    }
    LockGuard lock(s.mutex);
    if (s.state != BuildState::NeedsTrust || !s.run) {
        return {};
    }
    if (s.passTrustGateLocked()) {
        s.startSavingLocked();
    }
    return {};
}

BuildState BuildService::state() const {
    LockGuard lock(m_state->mutex);
    return m_state->state;
}

std::vector<std::string> BuildService::untrustedRoots() const {
    LockGuard lock(m_state->mutex);
    if (m_state->state != BuildState::NeedsTrust || !m_state->run) {
        return {};
    }
    return m_state->run->untrusted;
}

std::optional<BuildTarget> BuildService::currentBuild() const {
    LockGuard lock(m_state->mutex);
    if (!m_state->run) {
        return std::nullopt;
    }
    return m_state->run->target;
}

BuildProgress BuildService::progress() const {
    LockGuard lock(m_state->mutex);
    return m_state->progress;
}

std::optional<BuildOutcome> BuildService::lastOutcome() const {
    LockGuard lock(m_state->mutex);
    return m_state->last;
}

std::vector<BuildDiagnostic> BuildService::diagnostics() const {
    LockGuard lock(m_state->mutex);
    return m_state->diagnostics.all();
}

std::vector<DiagnosticFileGroup> BuildService::diagnosticsByFile() const {
    LockGuard lock(m_state->mutex);
    return m_state->diagnostics.byFile();
}

DiagnosticCounts BuildService::diagnosticCounts() const {
    LockGuard lock(m_state->mutex);
    return m_state->diagnostics.counts();
}

std::vector<BuildDiagnostic> BuildService::diagnosticsForDocument(std::string_view path) const {
    LockGuard lock(m_state->mutex);
    return m_state->diagnostics.forDocument(path);
}

bool BuildService::waitForIdle(IClock::Duration timeout) const {
    const State& s = *m_state;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    UniqueLock lock(s.queueMutex);
    for (;;) {
        if (s.tasks.empty() && !s.taskRunning && s.publishedState.load() == BuildState::Idle) {
            return true;
        }
        if (s.queueChanged.wait_until(lock.native(), deadline) == std::cv_status::timeout) {
            return s.tasks.empty() && !s.taskRunning && s.publishedState.load() == BuildState::Idle;
        }
    }
}

} // namespace frost::build
