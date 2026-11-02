// SPDX-License-Identifier: Apache-2.0
//
// RunController (see frost/ui/run_controller.hpp): the relay that moves the service's events
// to the GUI thread, the startup combo and the Busy prompt.

#include "frost/ui/run_controller.hpp"

#include "frost/core/mutex.hpp"
#include "frost/ui/run_console.hpp"

#include <QComboBox>
#include <QMetaObject>
#include <QSignalBlocker>

#include <format>
#include <string>
#include <utility>

namespace frost::ui {

// Shared with the service's callbacks: posts to the controller while it lives.
struct RunController::Relay {
    Mutex mutex;
    RunController* target FROST_GUARDED_BY(mutex) = nullptr;

    // Calls function(controller) on the controller's thread, queued; ~QObject drops what is
    // still queued, so it never runs on a destroyed controller.
    template<typename F>
    void post(F&& function) {
        LockGuard lock(mutex);
        if (target != nullptr) {
            RunController* self = target;
            QMetaObject::invokeMethod(
                self, [self, call = std::forward<F>(function)]() mutable { call(self); }, Qt::QueuedConnection);
        }
    }
};

RunController::RunController(RunConsole* console, RunControllerHooks hooks, QObject* parent)
    : QObject(parent), m_console(console), m_hooks(std::move(hooks)), m_relay(std::make_shared<Relay>()) {
    {
        LockGuard lock(m_relay->mutex);
        m_relay->target = this;
    }
    if (m_console) {
        connect(m_console, &RunConsole::stopRequested, this, [this] { stop(); });
        connect(m_console, &RunConsole::rerunRequested, this, [this] {
            if (auto rerun = this->rerun(); !rerun) {
                report(rerun.error());
            }
        });
        connect(m_console, &RunConsole::inputSubmitted, this, [this](const QString& line) {
            if (m_hooks.alternateRunning && m_hooks.alternateRunning()) {
                if (m_hooks.alternateInput)
                    m_hooks.alternateInput(line);
                return;
            }
            if (m_service == nullptr || m_current == 0) {
                return;
            }
            if (auto sent = m_service->sendInput(m_current, line.toStdString() + "\n"); !sent) {
                report(sent.error());
            }
        });
    }
}

RunController::~RunController() {
    LockGuard lock(m_relay->mutex);
    m_relay->target = nullptr;
}

build::RunServiceCallbacks RunController::callbacks() {
    const std::shared_ptr<Relay> relay = m_relay;
    return {
        .onRunStarted =
            [relay](const build::RunInfo& info) {
                relay->post([info](RunController* self) { self->onStarted(info); });
            },
        .onRunStateChanged =
            [relay](const build::RunInfo& info) {
                relay->post([info](RunController* self) { self->onStateChanged(info); });
            },
        .onOutput =
            [relay](build::RunId run, build::RunOutputKind kind, std::string_view text) {
                relay->post(
                    [run, kind, text = std::string(text)](RunController* self) { self->onOutput(run, kind, text); });
            },
        .onRunFinished =
            [relay](const build::RunOutcome& outcome) {
                relay->post([outcome](RunController* self) { self->onFinished(outcome); });
            },
        .onStartupProjectChanged =
            [relay](std::optional<project::ProjectId> project) {
                relay->post([project](RunController* self) { self->onStartupChanged(project); });
            },
        .onExternalConsoleChanged = {},
    };
}

void RunController::setService(build::RunService* service) {
    m_service = service;
    m_current = 0;
    m_currentProject.reset();
    m_currentActive = false;
    refreshStartupCombo();
    emit runStateChanged();
}

void RunController::bindStartupCombo(QComboBox* combo) {
    if (m_combo) {
        disconnect(m_combo, nullptr, this, nullptr);
    }
    m_combo = combo;
    if (m_combo) {
        connect(m_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
            if (m_fillingCombo || m_service == nullptr || index < 0) {
                return;
            }
            const QVariant data = m_combo->itemData(index);
            if (!data.isValid()) {
                return;
            }
            const project::ProjectId project{data.toUInt()};
            if (auto set = m_service->setStartupProject(project); !set) {
                report(set.error());
            }
        });
    }
    refreshStartupCombo();
}

void RunController::refreshStartupCombo() {
    if (!m_combo) {
        return;
    }
    m_fillingCombo = true;
    m_combo->clear();
    const auto solution = m_service != nullptr ? m_service->solution() : nullptr;
    if (solution) {
        for (const project::ProjectId id : m_service->startupCandidates()) {
            m_combo->addItem(QString::fromStdString(solution->project(id).name), QVariant(id.value()));
        }
        if (const auto startup = m_service->startupProject()) {
            m_combo->setCurrentIndex(m_combo->findData(QVariant(startup->value())));
        }
    }
    m_combo->setEnabled(solution != nullptr && m_combo->count() > 0);
    m_fillingCombo = false;
}

Result<build::RunId> RunController::run(std::optional<project::ProjectId> project) {
    if (m_hooks.mayRun && !m_hooks.mayRun())
        return fail(ErrorCode::Busy, "stop debugging first");
    if (m_hooks.alternateAvailable && m_hooks.alternateAvailable() && m_hooks.alternateRun) {
        return m_hooks.alternateRun();
    }
    if (m_service == nullptr) {
        return fail(ErrorCode::InvalidArgument, "no solution is open");
    }
    auto started = m_service->run(project);
    if (started || started.error().code() != ErrorCode::Busy) {
        return started;
    }
    // Busy: the project's own run asks to stop and rerun; a build in progress just says so.
    const std::optional<project::ProjectId> id = project ? project : m_service->startupProject();
    if (!id || !m_service->activeRun(*id)) {
        return started;
    }
    const auto solution = m_service->solution();
    const QString name = solution ? QString::fromStdString(solution->project(*id).name) : QString();
    if (m_hooks.confirmRerun && !m_hooks.confirmRerun(name)) {
        return fail(ErrorCode::Cancelled, std::format("{} is still running", name.toStdString()));
    }
    return m_service->rerun(id);
}

Result<build::RunId> RunController::rerun() {
    if (m_hooks.mayRun && !m_hooks.mayRun())
        return fail(ErrorCode::Busy, "stop debugging first");
    if (m_hooks.alternateAvailable && m_hooks.alternateAvailable() && m_hooks.alternateRerun) {
        return m_hooks.alternateRerun();
    }
    if (m_service == nullptr) {
        return fail(ErrorCode::InvalidArgument, "no solution is open");
    }
    return m_service->rerun(m_currentProject);
}

void RunController::stop() {
    if (m_hooks.alternateRunning && m_hooks.alternateRunning()) {
        if (m_hooks.alternateStop)
            m_hooks.alternateStop();
        return;
    }
    if (m_service != nullptr && m_current != 0 && m_currentActive) {
        m_service->stop(m_current);
    }
}

Result<void> RunController::setStartupProject(project::ProjectId project) {
    if (m_service == nullptr) {
        return fail(ErrorCode::InvalidArgument, "no solution is open");
    }
    return m_service->setStartupProject(project);
}

void RunController::onStarted(const build::RunInfo& info) {
    if (info.debug) {
        return; // The Debug tool window shows Debug runs (frost/ui/debug_controller.hpp).
    }
    m_current = info.id;
    m_currentProject = info.project;
    m_currentActive = true;
    if (m_console) {
        m_console->beginRun(QStringLiteral("%1 [%2]").arg(QString::fromStdString(info.projectName),
                                                          QString::fromStdString(info.configuration.name())));
    }
    emit runStateChanged();
}

void RunController::onStateChanged(const build::RunInfo& info) {
    if (info.id != m_current) {
        return;
    }
    const bool live = info.state == build::RunState::Running || info.state == build::RunState::Stopping;
    if (m_console && live) {
        m_console->setRunning(info.state == build::RunState::Running, info.acceptsInput);
    }
    emit runStateChanged();
}

void RunController::onOutput(build::RunId run, build::RunOutputKind kind, const std::string& text) {
    if (run == m_current && m_console) {
        m_console->appendOutput(kind, text);
    }
}

void RunController::onFinished(const build::RunOutcome& outcome) {
    if (outcome.run.id != m_current) {
        return;
    }
    m_currentActive = false;
    if (m_console) {
        m_console->finish(outcome);
    }
    emit runStateChanged();
}

void RunController::onStartupChanged(std::optional<project::ProjectId> project) {
    if (!m_combo) {
        return;
    }
    if (!project) {
        refreshStartupCombo();
        return;
    }
    const int index = m_combo->findData(QVariant(project->value()));
    if (index < 0) {
        refreshStartupCombo(); // A solution was opened since the combo was filled.
        return;
    }
    const QSignalBlocker blocker(m_combo);
    m_combo->setCurrentIndex(index);
}

void RunController::report(const Error& error) {
    if (m_hooks.reportError) {
        m_hooks.reportError(QString::fromStdString(error.message()));
    }
}

} // namespace frost::ui
