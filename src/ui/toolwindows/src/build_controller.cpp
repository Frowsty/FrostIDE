// SPDX-License-Identifier: Apache-2.0
//
// BuildController and BuildServiceBackend (frost/ui/toolwindows/build_controller.hpp): the relay
// that posts the build service's callbacks to the GUI thread, the cached build state, and the
// forwarding backend over frost::build::BuildService.

#include "frost/ui/toolwindows/build_controller.hpp"

#include "frost/core/mutex.hpp"

#include <QMetaObject>

#include <algorithm>
#include <functional>
#include <utility>

namespace frost::ui {

// ---- BuildServiceBackend: plain forwarding.

Result<void> BuildServiceBackend::openSolution(std::shared_ptr<const project::SolutionModel> solution) {
    return m_service.openSolution(std::move(solution));
}

Result<void> BuildServiceBackend::closeSolution() { return m_service.closeSolution(); }

std::shared_ptr<const project::SolutionModel> BuildServiceBackend::solution() const { return m_service.solution(); }

std::vector<project::SolutionConfiguration> BuildServiceBackend::configurations() const {
    return m_service.configurations();
}

std::optional<std::size_t> BuildServiceBackend::activeConfigurationIndex() const {
    return m_service.activeConfigurationIndex();
}

Result<void> BuildServiceBackend::setActiveConfiguration(std::size_t index) {
    return m_service.setActiveConfiguration(index);
}

void BuildServiceBackend::setSaveAllHook(build::SaveAllHook hook) { m_service.setSaveAllHook(std::move(hook)); }

Result<void> BuildServiceBackend::build(build::BuildAction action, std::optional<project::ProjectId> project) {
    return m_service.build(action, project);
}

void BuildServiceBackend::cancel() { m_service.cancel(); }

Result<void> BuildServiceBackend::trust(std::string_view root) { return m_service.trust(root); }

build::BuildState BuildServiceBackend::state() const { return m_service.state(); }

std::vector<std::string> BuildServiceBackend::untrustedRoots() const { return m_service.untrustedRoots(); }

std::vector<build::BuildDiagnostic> BuildServiceBackend::diagnostics() const { return m_service.diagnostics(); }

// ---- The relay (header comment, "Marshalling").

struct BuildController::Relay {
    Mutex mutex;
    BuildController* target FROST_GUARDED_BY(mutex) = nullptr;

    // Posts `task` to the controller's thread, to run there with the controller; false (nothing
    // posted) once the controller is gone. Holding the mutex while posting keeps the controller
    // alive until the event is queued; Qt drops it if the controller dies before it runs.
    bool post(std::function<void(BuildController&)> task) {
        LockGuard lock(mutex);
        if (target == nullptr) {
            return false;
        }
        BuildController* controller = target;
        static_cast<void>(QMetaObject::invokeMethod(
            controller, [controller, task = std::move(task)] { task(*controller); }, Qt::QueuedConnection));
        return true;
    }
};

BuildController::BuildController(QObject* parent) : QObject(parent), m_relay(std::make_shared<Relay>()) {
    LockGuard lock(m_relay->mutex);
    m_relay->target = this;
}

BuildController::~BuildController() {
    // The backend may already be gone (the header comment's wiring), so it is not touched here; the
    // Save-All hook it holds fails with Cancelled from now on.
    LockGuard lock(m_relay->mutex);
    m_relay->target = nullptr;
}

build::BuildServiceCallbacks BuildController::serviceCallbacks(std::function<bool()> active) const {
    std::shared_ptr<Relay> relay = m_relay;
    const auto post = [relay, active = std::move(active)](std::function<void(BuildController&)> task) {
        relay->post([active, task = std::move(task)](BuildController& controller) {
            if (!active || active())
                task(controller);
        });
    };
    build::BuildServiceCallbacks callbacks;
    callbacks.onStateChanged = [post](build::BuildState state) {
        post([state](BuildController& c) { c.applyState(state); });
    };
    callbacks.onNeedsTrust = [post](const std::vector<std::string>& roots) {
        post([roots](BuildController& c) { c.applyNeedsTrust(roots); });
    };
    callbacks.onOutput = [post](build::BuildOutputKind kind, std::string_view line) {
        post([kind, text = std::string(line)](BuildController& c) { c.applyOutput(kind, text); });
    };
    callbacks.onDiagnostic = [post](const build::BuildDiagnostic& diagnostic) {
        post([diagnostic](BuildController& c) { c.applyDiagnostic(diagnostic); });
    };
    callbacks.onDiagnosticsCleared = [post] { post([](BuildController& c) { c.applyDiagnosticsCleared(); }); };
    callbacks.onProgress = [post](const build::BuildProgress& progress) {
        post([progress](BuildController& c) { c.applyProgress(progress); });
    };
    callbacks.onFinished = [post](const build::BuildOutcome& outcome) {
        post([outcome](BuildController& c) { c.applyFinished(outcome); });
    };
    callbacks.onActiveConfigurationChanged = [post](const project::SolutionConfiguration& configuration) {
        post([configuration](BuildController& c) { c.applyActiveConfiguration(configuration); });
    };
    return callbacks;
}

void BuildController::setBackend(IBuildBackend* backend) {
    m_backend = backend;
    if (m_backend != nullptr) {
        std::shared_ptr<Relay> relay = m_relay;
        m_backend->setSaveAllHook([relay](build::SaveAllCompletion done) {
            if (!relay->post([done](BuildController& c) { c.runSaveAll(done); })) {
                done(fail(ErrorCode::Cancelled, "the build tool windows are closed"));
            }
        });
    }
    refreshFromBackend();
    emit backendChanged();
    emit diagnosticsCleared();
    for (const build::BuildDiagnostic& diagnostic : m_diagnostics) {
        emit diagnosticAdded(diagnostic);
    }
    emit solutionChanged();
    emit activeConfigurationChanged(m_active ? static_cast<int>(*m_active) : -1);
    emit stateChanged(m_state);
}

void BuildController::refreshFromBackend() {
    m_output.clear();
    m_progress = {};
    m_lastOutcome.reset();
    if (m_backend == nullptr) {
        m_solution.reset();
        m_hasProject = false;
        m_configurations.clear();
        m_active.reset();
        m_state = build::BuildState::Idle;
        m_untrusted.clear();
        m_diagnostics.clear();
        return;
    }
    m_solution = m_backend->solution();
    m_hasProject = m_backend->hasProject();
    m_configurations = m_backend->configurations();
    m_active = m_backend->activeConfigurationIndex();
    m_state = m_backend->state();
    m_untrusted = m_backend->untrustedRoots();
    m_diagnostics = m_backend->diagnostics();
}

void BuildController::reportFailure(const Error& error) {
    emit requestFailed(QString::fromUtf8(error.message().data(), static_cast<qsizetype>(error.message().size())));
}

Result<void> BuildController::openSolution(std::shared_ptr<const project::SolutionModel> solution) {
    if (m_backend == nullptr) {
        return fail(ErrorCode::InvalidArgument, "no build service is bound");
    }
    auto opened = m_backend->openSolution(std::move(solution));
    if (!opened) {
        reportFailure(opened.error());
        return opened;
    }
    refreshFromBackend();
    emit diagnosticsCleared();
    emit solutionChanged();
    emit activeConfigurationChanged(m_active ? static_cast<int>(*m_active) : -1);
    return {};
}

void BuildController::projectEditPublished() {
    if (!m_backend)
        return;
    refreshFromBackend();
    emit diagnosticsCleared();
    emit solutionChanged();
    emit activeConfigurationChanged(m_active ? static_cast<int>(*m_active) : -1);
}

Result<void> BuildController::closeSolution() {
    if (m_backend == nullptr) {
        return fail(ErrorCode::InvalidArgument, "no build service is bound");
    }
    auto closed = m_backend->closeSolution();
    if (!closed) {
        reportFailure(closed.error());
        return closed;
    }
    refreshFromBackend();
    emit diagnosticsCleared();
    emit solutionChanged();
    emit activeConfigurationChanged(-1);
    return {};
}

Result<void> BuildController::setActiveConfiguration(std::size_t index) {
    if (m_backend == nullptr) {
        return fail(ErrorCode::InvalidArgument, "no build service is bound");
    }
    auto switched = m_backend->setActiveConfiguration(index);
    // The service applies the choice even when persisting it fails, so the index is re-read either way.
    const std::optional<std::size_t> active = m_backend->activeConfigurationIndex();
    if (active != m_active) {
        m_active = active;
        emit activeConfigurationChanged(m_active ? static_cast<int>(*m_active) : -1);
    }
    if (!switched) {
        reportFailure(switched.error());
    }
    return switched;
}

Result<void> BuildController::build(build::BuildAction action, std::optional<project::ProjectId> project) {
    if (m_backend == nullptr) {
        return fail(ErrorCode::InvalidArgument, "no build service is bound");
    }
    auto started = m_backend->build(action, project);
    if (!started) {
        reportFailure(started.error());
    }
    return started;
}

void BuildController::cancel() {
    if (m_backend != nullptr) {
        m_backend->cancel();
    }
}

Result<void> BuildController::trust(std::string_view root) {
    if (m_backend == nullptr) {
        return fail(ErrorCode::InvalidArgument, "no build service is bound");
    }
    auto trusted = m_backend->trust(root);
    if (!trusted) {
        reportFailure(trusted.error());
    }
    return trusted;
}

void BuildController::runSaveAll(const build::SaveAllCompletion& done) {
    if (m_saveAll == nullptr) {
        done({});
        return;
    }
    m_saveAll->saveAll(done);
}

void BuildController::applyState(build::BuildState state) {
    const bool started = m_state == build::BuildState::Idle && state != build::BuildState::Idle;
    m_state = state;
    if (state != build::BuildState::NeedsTrust) {
        m_untrusted.clear();
    }
    if (started) {
        m_output.clear();
        m_progress = {};
        emit buildStarted();
    }
    emit stateChanged(state);
}

void BuildController::applyNeedsTrust(std::vector<std::string> roots) {
    m_untrusted = std::move(roots);
    QStringList list;
    list.reserve(static_cast<qsizetype>(m_untrusted.size()));
    for (const std::string& root : m_untrusted) {
        list.push_back(QString::fromUtf8(root.data(), static_cast<qsizetype>(root.size())));
    }
    emit needsTrust(list);
}

void BuildController::applyOutput(build::BuildOutputKind kind, const std::string& line) {
    BuildOutputEntry entry{kind, QString::fromUtf8(line.data(), static_cast<qsizetype>(line.size()))};
    m_output.push_back(entry);
    if (m_output.size() > kMaxReplayLines) {
        m_output.pop_front();
    }
    emit outputLine(entry.kind, entry.text);
}

void BuildController::applyDiagnostic(const build::BuildDiagnostic& diagnostic) {
    m_diagnostics.push_back(diagnostic);
    emit diagnosticAdded(diagnostic);
}

void BuildController::applyDiagnosticsCleared() {
    m_diagnostics.clear();
    emit diagnosticsCleared();
}

void BuildController::applyProgress(const build::BuildProgress& progress) {
    m_progress = progress;
    emit progressChanged(progress);
}

void BuildController::applyFinished(const build::BuildOutcome& outcome) {
    m_lastOutcome = outcome;
    emit buildFinished(outcome);
}

void BuildController::applyActiveConfiguration(const project::SolutionConfiguration& configuration) {
    const auto it = std::ranges::find(m_configurations, configuration);
    if (it == m_configurations.end()) {
        return;
    }
    const auto index = static_cast<std::size_t>(it - m_configurations.begin());
    if (m_active != index) {
        m_active = index;
        emit activeConfigurationChanged(static_cast<int>(index));
    }
}

QString configurationLabel(const project::SolutionConfiguration& configuration) {
    return QString::fromUtf8(configuration.configuration.data(),
                             static_cast<qsizetype>(configuration.configuration.size())) +
           QStringLiteral(" | ") +
           QString::fromUtf8(configuration.platform.data(), static_cast<qsizetype>(configuration.platform.size()));
}

} // namespace frost::ui
