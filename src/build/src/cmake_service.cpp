// SPDX-License-Identifier: Apache-2.0
//
// CMakeService (see frost/build/cmake_service.hpp): service construction/teardown, worker loop,
// state/progress/event reporting, build/configure/cancel/trust commands, and diagnostic queries.

#include "cmake_service_internal.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace frost::build {

std::string_view toString(CMakeStep step) noexcept {
    switch (step) {
    case CMakeStep::Configure: return "Configure";
    case CMakeStep::Clean: return "Clean";
    case CMakeStep::Build: return "Build";
    }
    return "Configure";
}

std::string_view toString(CMakeActivity activity) noexcept {
    switch (activity) {
    case CMakeActivity::Idle: return "Idle";
    case CMakeActivity::ReadingModel: return "Reading the CMake project model";
    case CMakeActivity::WaitingForTrust: return "Waiting for trust";
    case CMakeActivity::Preparing: return "Preparing";
    case CMakeActivity::Configuring: return "Configuring";
    case CMakeActivity::Generating: return "Generating";
    case CMakeActivity::Cleaning: return "Cleaning";
    case CMakeActivity::Building: return "Building";
    case CMakeActivity::Cancelling: return "Cancelling";
    }
    return "Idle";
}

std::vector<project::CMakeProfileRecord> cmakeProfileRecords(const settings::RecordList& records) {
    std::vector<project::CMakeProfileRecord> out;
    for (const settings::Record& record : records) {
        project::CMakeProfileRecord profile;
        profile.name = fieldString(record, "name");
        if (profile.name.empty()) {
            continue;
        }
        profile.buildType = fieldString(record, "buildType");
        profile.generator = fieldString(record, "generator");
        profile.buildDirectory = fieldString(record, "buildDirectory");
        profile.cacheVariables = fieldList(record, "cacheVariables");
        profile.environment = fieldList(record, "environment");
        profile.buildOptions = fieldList(record, "buildOptions");
        profile.root = fieldString(record, "root");
        if (const settings::FieldValue* jobs = settings::findField(record, "jobs");
            jobs != nullptr && std::holds_alternative<std::int64_t>(*jobs)) {
            profile.jobs = static_cast<int>(std::clamp<std::int64_t>(std::get<std::int64_t>(*jobs), 0, 1024));
        }
        out.push_back(std::move(profile));
    }
    return out;
}

std::vector<CMakeBuildWrapper> cmakeBuildWrappers(const settings::RecordList& records) {
    std::vector<CMakeBuildWrapper> out;
    for (const auto& record : records)
        out.push_back({.root = fieldString(record, "root"),
                       .profile = fieldString(record, "profile"),
                       .executable = fieldString(record, "executable"),
                       .defaultTarget = fieldString(record, "defaultTarget"),
                       .buildArguments = fieldList(record, "buildArguments"),
                       .cleanArguments = fieldList(record, "cleanArguments"),
                       .rebuildArguments = fieldList(record, "rebuildArguments")});
    return out;
}

void CMakeService::State::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            UniqueLock lock(queueMutex);
            for (;;) {
                if (stopping) {
                    return;
                }
                if (!tasks.empty()) {
                    task = std::move(tasks.front());
                    tasks.pop_front();
                    break;
                }
                if (reloadAt && std::chrono::steady_clock::now() >= *reloadAt) {
                    reloadAt.reset();
                    task = [this] { fireReload(); };
                    break;
                }
                if (reloadAt) {
                    queueChanged.wait_until(lock.native(), *reloadAt);
                } else {
                    queueChanged.wait(lock.native());
                }
            }
            taskRunning = true;
        }
        task();
        task = nullptr;
        {
            LockGuard lock(queueMutex);
            taskRunning = false;
        }
        queueChanged.notify_all();
    }
}

// ---- Events (each a task, so callbacks run in order on the service thread).

void CMakeService::State::emitState(BuildState value) {
    emit([this, value] {
        if (callbacks.onStateChanged) {
            callbacks.onStateChanged(value);
        }
    });
}

void CMakeService::State::emitOutput(bool configure, BuildOutputKind kind, std::string line) {
    emit([this, configure, kind, line = std::move(line)] {
        const auto& callback = configure ? callbacks.onConfigureOutput : callbacks.onOutput;
        if (callback) {
            callback(kind, line);
        }
    });
}

void CMakeService::State::emitModel(std::shared_ptr<const project::CMakeProject> value) {
    emit([this, value = std::move(value)] {
        if (callbacks.onModelChanged) {
            callbacks.onModelChanged(value);
        }
    });
}

void CMakeService::State::emitProfilesLocked() {
    emit([this, list = profiles, index = active] {
        if (callbacks.onProfilesChanged) {
            callbacks.onProfilesChanged(list, index);
        }
    });
}

void CMakeService::State::emitDiagnosticsCleared() {
    emit([this] {
        if (callbacks.onDiagnosticsCleared) {
            callbacks.onDiagnosticsCleared();
        }
    });
}

void CMakeService::State::setStateLocked(BuildState value) {
    state = value;
    publishedState.store(value);
    emitState(value);
    refreshProgressLocked();
}

/*
 * Recomputes the activity (CMakeProgress) from the state and the job, and reports a change:
 * posted as an event, or with `direct` returned for the caller, a task on the service thread,
 * to deliver itself once it has released the lock (an event posted by a task runs only after
 * the task, too late for "reading the reply" said by the task that reads it). Sequence
 * numbers let the receiver drop an older value that arrives after a newer one.
 */
std::optional<CMakeProgress> CMakeService::State::refreshProgressLocked(bool direct) {
    CMakeProgress next;
    switch (state) {
    case BuildState::Idle: next.activity = replyLoadRunning ? CMakeActivity::ReadingModel : CMakeActivity::Idle; break;
    case BuildState::NeedsTrust:
        next.activity = CMakeActivity::WaitingForTrust;
        next.cancellable = true;
        break;
    case BuildState::SavingDocuments:
    case BuildState::Starting:
        next.activity = CMakeActivity::Preparing;
        next.cancellable = true;
        break;
    case BuildState::Running:
        if (job && job->readingReply) {
            next.activity = CMakeActivity::ReadingModel;
        } else if (job && job->current == CMakeStep::Configure) {
            next.activity = job->generating ? CMakeActivity::Generating : CMakeActivity::Configuring;
            next.cancellable = true;
        } else {
            next.activity = job && job->current == CMakeStep::Clean ? CMakeActivity::Cleaning : CMakeActivity::Building;
            next.cancellable = true;
        }
        break;
    case BuildState::Cancelling: next.activity = CMakeActivity::Cancelling; break;
    }
    if (next.activity != CMakeActivity::Idle) {
        if (job) {
            next.profile = job->profile.name;
        } else if (auto profile = activeProfileLocked()) {
            next.profile = profile->name;
        }
    }
    if (next.activity == progressValue.activity && next.profile == progressValue.profile &&
        next.cancellable == progressValue.cancellable) {
        return std::nullopt;
    }
    next.since = std::chrono::steady_clock::now();
    next.sequence = progressValue.sequence + 1;
    progressValue = next;
    if (direct) {
        return next;
    }
    emit([this, next] { deliverProgress(next); });
    return std::nullopt;
}

void CMakeService::State::deliverProgress(const std::optional<CMakeProgress>& value) const {
    if (value && callbacks.onProgress) {
        callbacks.onProgress(*value);
    }
}

bool CMakeService::State::wrapperTrustCurrentLocked() const {
    if (!job || !job->wrapper)
        return true;
    const auto trust = trustStore.snapshot(job->sourceDir);
    return trust.trusted && trust.revision == job->trustRevision;
}

std::function<bool()> CMakeService::State::outputFenceLocked(std::uint64_t id) {
    const auto root = job->sourceDir;
    const auto revision = job->trustRevision;
    const auto generation = profileGeneration;
    std::weak_ptr<State> weak = weak_from_this();
    return [weak, id, root, revision, generation] {
        auto state = weak.lock();
        if (!state)
            return false;
        LockGuard lock(state->mutex);
        const auto trust = state->trustStore.snapshot(root);
        return state->lastJobId == id && state->sourceDir == root && state->profileGeneration == generation &&
               trust.trusted && trust.revision == revision;
    };
}

void CMakeService::State::emitWrapperOutputLocked(BuildOutputKind kind, std::string line) {
    auto fence = outputFenceLocked(job->id);
    emit([this, kind, line = std::move(line), fence = std::move(fence)] {
        if (!fence())
            return;
        if (callbacks.onOutputFenced)
            callbacks.onOutputFenced(kind, line, fence);
        else if (callbacks.onOutput)
            callbacks.onOutput(kind, line);
    });
}

std::unique_ptr<CMakeService> CMakeService::create(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                   project::TrustStore& trustStore, CMakeServiceOptions options,
                                                   CMakeServiceCallbacks callbacks) {
    auto state = std::make_shared<State>(fs, launcher, trustStore, std::move(options), std::move(callbacks));
    State* raw = state.get();
    state->worker = std::thread([raw] { raw->workerLoop(); });
    return std::unique_ptr<CMakeService>(new CMakeService(std::move(state)));
}

Result<std::string> CMakeService::defaultStatePath() {
    auto trustPath = project::TrustStore::defaultStorePath();
    if (!trustPath) {
        return std::unexpected(std::move(trustPath.error()));
    }
    return io::joinPath(io::parentPath(*trustPath), kStateFileName);
}

CMakeService::CMakeService(std::shared_ptr<State> state) noexcept : m_state(std::move(state)) {}

CMakeService::~CMakeService() {
    std::unique_ptr<BuildSession> session;
    std::shared_ptr<State::AdmissionLease> admission;
    {
        LockGuard lock(m_state->mutex);
        if (m_state->job) {
            admission = m_state->job->admission;
            session = std::move(m_state->job->session);
            m_state->job.reset();
        }
        m_state->state = BuildState::Idle;
        m_state->publishedState.store(BuildState::Idle);
    }
    if (session) {
        session->cancel();
        session.reset();
    }
    {
        LockGuard lock(m_state->queueMutex);
        m_state->stopping = true;
    }
    m_state->queueChanged.notify_all();
    if (m_state->worker.joinable()) {
        m_state->worker.join();
    }
    admission.reset();
    LockGuard lock(m_state->queueMutex);
    m_state->tasks.clear();
}

Result<void> CMakeService::configure() {
    LockGuard lock(m_state->mutex);
    return m_state->startJobLocked(State::JobKind::Configure, false, {}, {CMakeStep::Configure});
}

Result<void> CMakeService::configureOwned(std::uint64_t owner) {
    if (!owner)
        return fail(ErrorCode::InvalidArgument, "owned configure requires an owner");
    LockGuard lock(m_state->mutex);
    return m_state->startJobLocked(State::JobKind::Configure, false, {}, {CMakeStep::Configure}, {}, owner);
}

void CMakeService::admissionAvailable() {
    bool pending;
    {
        LockGuard lock(m_state->mutex);
        pending = m_state->reloadPending && m_state->state == BuildState::Idle;
    }
    if (pending) {
        auto* state = m_state.get();
        state->post([state] { state->fireReload(); });
    }
}

std::optional<CMakeConfigureOutcome> CMakeService::lastConfigure() const {
    LockGuard lock(m_state->mutex);
    return m_state->lastConfigure;
}

void CMakeService::setSaveAllHook(SaveAllHook hook) {
    LockGuard lock(m_state->mutex);
    m_state->saveAll = std::move(hook);
}

Result<void> CMakeService::setBuildWrappers(std::vector<CMakeBuildWrapper> wrappers) {
    LockGuard lock(m_state->mutex);
    if (m_state->state != BuildState::Idle)
        return fail(ErrorCode::Busy, "cannot change build wrappers while a CMake job is active");
    m_state->wrappers = std::move(wrappers);
    return {};
}

Result<void> CMakeService::build(BuildAction action, std::optional<std::size_t> target, CMakeBuildCompletion done,
                                 const std::shared_ptr<const project::CMakeProject>& expectedModel) {
    State& s = *m_state;
    LockGuard lock(s.mutex);
    if (expectedModel && expectedModel != s.model)
        return fail(ErrorCode::Busy, "CMake targets changed; wait for the project view to refresh");
    if (action == BuildAction::Compile) {
        // SLN-9's single-file compile is MSBuild's ClCompile target; CMake has no equivalent here yet.
        return fail(ErrorCode::Unsupported, "Compile (Ctrl+F7) is available for MSBuild solutions only (SLN-9)");
    }
    CMakeBuildRequest request{.action = action, .target = target, .targetName = {}, .profile = {}};
    if (target) {
        if (!s.model || *target >= s.model->targets.size()) {
            return fail(ErrorCode::InvalidArgument, std::format("target {} does not exist", *target));
        }
        request.targetName = s.model->targets[*target].name;
    }
    std::vector<CMakeStep> steps;
    if (!s.model || s.stale) {
        steps.push_back(CMakeStep::Configure);
    }
    if (action == BuildAction::Clean || action == BuildAction::Rebuild) {
        steps.push_back(CMakeStep::Clean);
    }
    if (action == BuildAction::Build || action == BuildAction::Rebuild) {
        steps.push_back(CMakeStep::Build);
    }
    return s.startJobLocked(State::JobKind::Build, false, std::move(request), std::move(steps), std::move(done));
}

Result<void> CMakeService::buildNamedTarget(std::string target) {
    if (target.empty() || target.front() == '-' || target.find_first_of("\r\n\0", 0, 3) != std::string::npos)
        return fail(ErrorCode::InvalidArgument, "a CMake target must be a literal nonempty name");
    State& s = *m_state;
    LockGuard lock(s.mutex);
    CMakeBuildRequest request{.action = BuildAction::Build, .targetName = std::move(target)};
    std::vector<CMakeStep> steps;
    if (!s.model || s.stale) steps.push_back(CMakeStep::Configure);
    steps.push_back(CMakeStep::Build);
    return s.startJobLocked(State::JobKind::Build, false, std::move(request), std::move(steps));
}

bool CMakeService::canCompileFile(std::string_view path) const {
    State& s = *m_state;
    LockGuard lock(s.mutex);
    if (!s.model || s.stale || !s.model->generator.starts_with("Ninja")) return false;
    for (const auto& target : s.model->targets)
        for (const auto& source : target.sources)
            if (source.compileGroup && documentKey(source.path) == documentKey(path)) return true;
    return false;
}

Result<void> CMakeService::compileFile(std::string_view path) {
    return buildFile(BuildAction::Compile, path);
}

Result<void> CMakeService::buildFile(BuildAction action, std::string_view path) {
    State& s = *m_state;
    LockGuard lock(s.mutex);
    if (!s.model || s.stale || !s.model->generator.starts_with("Ninja"))
        return fail(ErrorCode::Unsupported, "single-file CMake compilation requires a current Ninja model and compilation database");
    auto source = io::normalizePath(path);
    if (!source) return std::unexpected(source.error());
    CMakeBuildRequest request{.action = action, .file = std::move(*source)};
    std::vector<CMakeStep> steps;
    if (action == BuildAction::Clean || action == BuildAction::Rebuild) steps.push_back(CMakeStep::Clean);
    if (action != BuildAction::Clean) steps.push_back(CMakeStep::Build);
    return s.startJobLocked(State::JobKind::Build, false, std::move(request), std::move(steps));
}

void CMakeService::cancel() {
    State& s = *m_state;
    LockGuard lock(s.mutex);
    s.reloadPending = false;
    switch (s.state) {
    case BuildState::Idle:
    case BuildState::Cancelling: return;
    case BuildState::Running:
        if (s.job && s.job->session) {
            s.setStateLocked(BuildState::Cancelling);
            s.job->session->cancel(); // Its result arrives through finishStep.
        }
        return;
    case BuildState::NeedsTrust:
    case BuildState::SavingDocuments:
    case BuildState::Starting:
        if (s.job) {
            s.job->lastResult.cancelled = true;
            s.finishJobLocked(std::nullopt);
        }
        return;
    }
}

Result<void> CMakeService::trust(std::string_view root) {
    State& s = *m_state;
    if (auto trusted = s.trustStore.trust(root); !trusted) {
        return std::unexpected(std::move(trusted.error()));
    }
    LockGuard lock(s.mutex);
    if (s.state == BuildState::NeedsTrust && s.job) {
        if (s.folderTrustedLocked()) {
            s.afterTrustLocked();
        }
        return {};
    }
    if (s.state == BuildState::Idle && !s.sourceDir.empty() && s.options.configureOnOpen && !s.configuredOnce &&
        s.folderTrustedLocked()) {
        static_cast<void>(s.startJobLocked(State::JobKind::Configure, true, {}, {CMakeStep::Configure}));
    }
    return {};
}

void CMakeService::trustChanged() {
    State& s = *m_state;
    LockGuard lock(s.mutex);
    if (s.state == BuildState::NeedsTrust && s.job) {
        const bool all = std::ranges::all_of(s.job->untrusted,
                                             [&](const std::string& root) { return s.trustStore.isTrusted(root); });
        if (all && s.folderTrustedLocked()) {
            s.afterTrustLocked();
        }
        return;
    }
    if (s.state == BuildState::Idle && !s.sourceDir.empty() && s.options.configureOnOpen && !s.configuredOnce &&
        s.folderTrustedLocked()) {
        static_cast<void>(s.startJobLocked(State::JobKind::Configure, true, {}, {CMakeStep::Configure}));
    }
}

BuildState CMakeService::state() const {
    // Stored together with `state` under the lock (setStateLocked, the close path), read without it.
    return m_state->publishedState.load();
}

bool CMakeService::modelLoading() const {
    LockGuard lock(m_state->mutex);
    return m_state->replyLoadsPending > 0;
}

CMakeProgress CMakeService::progress() const {
    LockGuard lock(m_state->mutex);
    return m_state->progressValue;
}

std::optional<CMakeStep> CMakeService::currentStep() const {
    LockGuard lock(m_state->mutex);
    return m_state->job ? m_state->job->current : std::nullopt;
}

std::vector<std::string> CMakeService::untrustedRoots() const {
    LockGuard lock(m_state->mutex);
    if (m_state->state != BuildState::NeedsTrust || !m_state->job) {
        return {};
    }
    return m_state->job->untrusted;
}

std::optional<CMakeBuildOutcome> CMakeService::lastOutcome() const {
    LockGuard lock(m_state->mutex);
    return m_state->lastBuild;
}

std::vector<BuildDiagnostic> CMakeService::diagnostics() const {
    LockGuard lock(m_state->mutex);
    return m_state->diagnostics.all();
}

std::vector<DiagnosticFileGroup> CMakeService::diagnosticsByFile() const {
    LockGuard lock(m_state->mutex);
    return m_state->diagnostics.byFile();
}

DiagnosticCounts CMakeService::diagnosticCounts() const {
    LockGuard lock(m_state->mutex);
    return m_state->diagnostics.counts();
}

std::vector<BuildDiagnostic> CMakeService::diagnosticsForDocument(std::string_view path) const {
    LockGuard lock(m_state->mutex);
    return m_state->diagnostics.forDocument(path);
}

bool CMakeService::waitForIdle(IClock::Duration timeout) const {
    const State& s = *m_state;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    UniqueLock lock(s.queueMutex);
    for (;;) {
        if (s.tasks.empty() && !s.taskRunning && !s.reloadAt && s.publishedState.load() == BuildState::Idle) {
            return true;
        }
        if (s.queueChanged.wait_until(lock.native(), deadline) == std::cv_status::timeout) {
            return s.tasks.empty() && !s.taskRunning && !s.reloadAt && s.publishedState.load() == BuildState::Idle;
        }
    }
}

} // namespace frost::build
