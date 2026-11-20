// SPDX-License-Identifier: Apache-2.0
// Real dotnet builds feed existing BuildController output and Problems; no synthetic outcomes.
#include "managed_build_backend.hpp"

#include "frost/core/mutex.hpp"
#include "frost/io/file_system.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/trust_store.hpp"

#include <QMetaObject>

#include <atomic>
namespace frost::app {
struct ManagedBuildJob {
    Mutex mutex;
    ManagedBuildBackend* target FROST_GUARDED_BY(mutex) = nullptr;
    build::BuildSession* session FROST_GUARDED_BY(mutex) = nullptr;
    std::stop_source stop;
    build::BuildService* admission = nullptr;
    std::atomic<bool> reserved = false, started = false;
    void release() {
        if (reserved.exchange(false) && admission)
            admission->releaseRun();
    }
    void post(std::function<void(ManagedBuildBackend&)> action) {
        LockGuard lock(mutex);
        if (!target)
            return;
        auto* receiver = target;
        QMetaObject::invokeMethod(
            receiver, [receiver, action = std::move(action)] { action(*receiver); }, Qt::QueuedConnection);
    }
};
namespace {
sched::Task<void> buildManaged(std::shared_ptr<ManagedBuildJob> job,
                               std::shared_ptr<const build::ManagedProjectEvaluation> model, std::string root,
                               std::uint64_t revision, io::IFileSystem* fs, io::IProcessLauncher* launcher,
                               project::TrustStore* trust, build::BuildAction action,
                               build::BuildServiceCallbacks callbacks) {
    build::BuildOutcome outcome;
    outcome.target.action = action;
    outcome.target.projectName = model->assemblyName;
    outcome.target.configuration = {model->request.configuration, model->request.targetFramework};
    const auto current = [&]() -> Result<void> {
        const auto snapshot = trust->snapshot(root);
        if (job->stop.stop_requested())
            return fail(ErrorCode::Cancelled, "Managed build cancelled");
        if (!snapshot.trusted || snapshot.revision != revision)
            return fail(ErrorCode::Untrusted, "Managed project trust changed");
        for (const auto& path : {root, model->request.project}) {
            auto physical = fs->physicalPath(path);
            if (!physical || lang::documentKey(*physical) != lang::documentKey(path))
                return fail(ErrorCode::PermissionDenied, "Managed project physical identity changed");
        }
        if (model->inputFiles.empty())
            return fail(ErrorCode::PermissionDenied, "Refresh managed project input receipts before execution");
        for (const auto& [path, bytes] : model->inputFiles) {
            auto actual = fs->readFileSnapshot(path, 2U * 1024U * 1024U);
            if (!actual || *actual != bytes)
                return fail(ErrorCode::Busy, "Managed project/import changed; refresh .NET Project Context");
        }
        return {};
    };
    auto execute = [&]() -> Result<build::BuildResult> {
        if (auto valid = current(); !valid)
            return std::unexpected(valid.error());
        auto spec = build::managedProjectBuildSpec(model->request);
        if (!spec)
            return std::unexpected(spec.error());
        spec->arguments.push_back("--no-restore");
        if (action == build::BuildAction::Clean)
            spec->arguments[0] = "clean";
        else if (action == build::BuildAction::Rebuild)
            spec->arguments.push_back("-target:Rebuild");
        build::DiagnosticResolver resolver(*fs, root);
        build::BuildCallbacks events;
        events.onLine = [callbacks](auto stream, std::string_view text) {
            if (callbacks.onOutput)
                callbacks.onOutput(stream == build::OutputStream::Stdout ? build::BuildOutputKind::Stdout
                                                                         : build::BuildOutputKind::Stderr,
                                   text);
        };
        events.onDiagnostic = [&](const build::Diagnostic& diagnostic) {
            auto resolved = resolver.resolve(diagnostic);
            job->post(
                [resolved = std::move(resolved)](auto& backend) mutable { backend.diagnostic(std::move(resolved)); });
        };
        auto session = build::BuildSession::start(*launcher, *spec, true, std::move(events));
        if (!session)
            return std::unexpected(session.error());
        {
            LockGuard lock(job->mutex);
            job->session = session->get();
            if (job->stop.stop_requested())
                (*session)->cancel();
        }
        std::stop_callback cancelled(job->stop.get_token(), [job] {
            LockGuard lock(job->mutex);
            if (job->session)
                job->session->cancel();
        });
        auto result = (*session)->wait();
        {
            LockGuard lock(job->mutex);
            job->session = nullptr;
        }
        if (auto valid = current(); !valid)
            return std::unexpected(valid.error());
        return result;
    }();
    if (execute)
        outcome.result = std::move(*execute);
    else {
        outcome.error = execute.error();
        outcome.result.cancelled = job->stop.stop_requested();
    }
    outcome.counts.errors = outcome.result.errorCount;
    outcome.counts.warnings = outcome.result.warningCount;
    job->release();
    job->post([outcome = std::move(outcome)](auto& backend) mutable { backend.finish(std::move(outcome)); });
    co_return;
}
} // namespace
ManagedBuildBackend::ManagedBuildBackend(std::shared_ptr<const build::ManagedProjectEvaluation> model, std::string root,
                                         std::uint64_t revision, io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                         project::TrustStore& trust, build::BuildService& admission,
                                         sched::ThreadPool& pool, std::vector<sched::JoinHandle<void>>& tasks,
                                         build::BuildServiceCallbacks callbacks)
    : m_model(std::move(model)), m_root(std::move(root)), m_trustRevision(revision), m_fs(fs), m_launcher(launcher),
      m_trust(trust), m_admission(admission), m_pool(pool), m_tasks(tasks), m_callbacks(std::move(callbacks)) {}
ManagedBuildBackend::~ManagedBuildBackend() {
    cancel();
    if (m_job) {
        LockGuard lock(m_job->mutex);
        m_job->target = nullptr;
    }
    if (m_job && !m_job->started.load())
        m_job->release();
}
Result<void> ManagedBuildBackend::closeSolution() {
    if (m_state != build::BuildState::Idle)
        return fail(ErrorCode::Busy, "Stop the managed build first");
    return {};
}
std::vector<project::SolutionConfiguration> ManagedBuildBackend::configurations() const {
    return {{m_model->request.configuration, m_model->request.targetFramework}};
}
Result<void> ManagedBuildBackend::setActiveConfiguration(std::size_t index) {
    if (index)
        return fail(ErrorCode::OutOfRange, "Select another .NET Project Context to change TFM/configuration");
    return {};
}
void ManagedBuildBackend::setState(build::BuildState state) {
    m_state = state;
    if (m_callbacks.onStateChanged)
        m_callbacks.onStateChanged(state);
}
Result<void> ManagedBuildBackend::build(build::BuildAction action, std::optional<project::ProjectId> project) {
    if (project || action == build::BuildAction::Compile)
        return fail(ErrorCode::Unsupported, "Managed builds use the selected evaluated SDK project");
    if (m_state != build::BuildState::Idle)
        return fail(ErrorCode::Busy, "A managed build is already active");
    auto reserved = m_admission.reserveRun();
    if (!reserved)
        return reserved;
    ++m_generation;
    m_diagnostics.clear();
    if (m_callbacks.onDiagnosticsCleared)
        m_callbacks.onDiagnosticsCleared();
    m_job = std::make_shared<ManagedBuildJob>();
    m_job->target = this;
    m_job->admission = &m_admission;
    m_job->reserved = true;
    setState(build::BuildState::SavingDocuments);
    const auto generation = m_generation;
    auto saved = [job = m_job, action, generation](Result<void> result) {
        job->post([result = std::move(result), action, generation](auto& self) mutable {
            if (self.m_generation != generation || self.m_state != build::BuildState::SavingDocuments)
                return;
            if (!result) {
                build::BuildOutcome outcome;
                outcome.error = result.error();
                self.finish(std::move(outcome));
            } else
                self.start(action, generation);
        });
    };
    if (m_save)
        m_save(std::move(saved));
    else
        saved({});
    return {};
}
void ManagedBuildBackend::start(build::BuildAction action, std::uint64_t generation) {
    if (generation != m_generation)
        return;
    setState(build::BuildState::Running);
    m_job->started = true;
    m_tasks.push_back(m_pool.spawn(sched::Lane::Background, buildManaged(m_job, m_model, m_root, m_trustRevision, &m_fs,
                                                                         &m_launcher, &m_trust, action, m_callbacks)));
}
void ManagedBuildBackend::finish(build::BuildOutcome outcome) {
    if (m_job)
        m_job->release();
    if (m_callbacks.onFinished)
        m_callbacks.onFinished(outcome);
    setState(build::BuildState::Idle);
}
void ManagedBuildBackend::diagnostic(build::BuildDiagnostic value) {
    m_diagnostics.push_back(value);
    if (m_callbacks.onDiagnostic)
        m_callbacks.onDiagnostic(value);
}
void ManagedBuildBackend::cancel() {
    if (m_state == build::BuildState::Idle)
        return;
    if (m_job)
        m_job->stop.request_stop();
    if (m_state == build::BuildState::SavingDocuments) {
        ++m_generation;
        build::BuildOutcome outcome;
        outcome.result.cancelled = true;
        finish(std::move(outcome));
    } else
        setState(build::BuildState::Cancelling);
}
Result<void> ManagedBuildBackend::trust(std::string_view root) { return m_trust.trust(root); }
std::vector<std::string> ManagedBuildBackend::untrustedRoots() const {
    return m_trust.isTrusted(m_root) ? std::vector<std::string>{} : std::vector<std::string>{m_root};
}
} // namespace frost::app
