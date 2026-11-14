// SPDX-License-Identifier: Apache-2.0
//
// RunController (see frost/ui/run_controller.hpp): the relay that moves the RunService's and the
// RunManager's events to the GUI thread, the configuration combo, the run of each configuration
// type into its Run tool window tab, and the Busy prompt.

#include "frost/ui/run_controller.hpp"

#include "frost/core/mutex.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_tool_window.hpp"

#include <QComboBox>
#include <QMetaObject>
#include <QSignalBlocker>

#include <algorithm>
#include <atomic>
#include <format>
#include <string>
#include <utility>

namespace frost::ui {

namespace {

// The combo's item data (Qt::UserRole): what an entry stands for. "edit" opens Run/Debug
// Configurations.
QString editEntry() { return QStringLiteral("edit"); }
QString configurationEntry(std::string_view id) {
    return QStringLiteral("config:") + QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
}
QString projectEntry(project::ProjectId id) { return QStringLiteral("project:%1").arg(id.value()); }

// The Run tool window keys (run_tool_window.hpp) of each runner.
QString configurationKey(std::string_view id) { return configurationEntry(id); }
QString projectKey(project::ProjectId id) { return projectEntry(id); }

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

} // namespace

// Shared with the service's and the RunManager's callbacks: posts to the controller while it lives.
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

RunController::RunController(RunToolWindow* window, RunControllerHooks hooks, QObject* parent)
    : QObject(parent), m_window(window), m_hooks(std::move(hooks)), m_relay(std::make_shared<Relay>()) {
    if (m_window) {
        // Rerun in the empty first tab runs the selected configuration, as Run does.
        connect(m_window, &RunToolWindow::rerunWithoutRunRequested, this, [this] {
            if (auto started = rerun(); !started)
                report(started.error());
        });
    }
    LockGuard lock(m_relay->mutex);
    m_relay->target = this;
}

RunController::~RunController() {
    {
        LockGuard lock(m_relay->mutex);
        m_relay->target = nullptr;
    }
    // A native run's tree must not outlive the controller that shows it.
    if (m_runs != nullptr) {
        for (const auto& [id, entry] : m_native) {
            m_runs->stop(entry.id);
        }
    }
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
    m_serviceConsoles.clear();
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
            if (m_fillingCombo || index < 0) {
                return;
            }
            const QString entry = m_combo->itemData(index).toString();
            if (entry == editEntry()) {
                fillCombo(); // Back to the selection the user had.
                if (m_hooks.editConfigurations) {
                    m_hooks.editConfigurations();
                }
                return;
            }
            if (entry.startsWith(QStringLiteral("config:"))) {
                const std::string id = entry.mid(7).toStdString();
                if (const run::RunConfiguration* configuration = m_set.find(id)) {
                    m_selectedId = id;
                    applySelection(*configuration);
                    emit runStateChanged(); // What Run and Debug would start changed (canRun).
                }
                return;
            }
            if (entry.startsWith(QStringLiteral("project:")) && m_service != nullptr) {
                const project::ProjectId project{entry.mid(8).toUInt()};
                if (auto set = m_service->setStartupProject(project); !set) {
                    report(set.error());
                }
            }
        });
    }
    refreshStartupCombo();
}

void RunController::setEditConfigurationsHandler(std::function<void()> handler) {
    m_hooks.editConfigurations = std::move(handler);
    refreshStartupCombo();
}

void RunController::setStoredConfigurations(std::vector<run::RunConfiguration> stored) {
    m_set.setStored(std::move(stored));
    refreshStartupCombo();
}

Result<void> RunController::replaceConfigurations(const std::vector<run::RunConfiguration>& edited) {
    m_set.replace(edited);
    Result<void> saved;
    if (m_hooks.saveConfigurations) {
        saved = m_hooks.saveConfigurations(m_set.stored());
    }
    refreshStartupCombo();
    return saved;
}

const run::RunConfiguration* RunController::selectedConfiguration() const noexcept {
    return m_selectedId.empty() ? nullptr : m_set.find(m_selectedId);
}

Result<void> RunController::selectConfiguration(const std::string& id, bool apply) {
    const run::RunConfiguration* configuration = m_set.find(id);
    if (configuration == nullptr) {
        return fail(ErrorCode::NotFound, std::format("no run configuration '{}'", id));
    }
    m_selectedId = id;
    if (apply) {
        applySelection(*configuration);
    }
    fillCombo();
    emit runStateChanged(); // What Run and Debug would start changed (canRun).
    return {};
}

void RunController::followCMakeTarget(const std::string& target) {
    // A native configuration the user chose stays selected; otherwise the combo follows the target.
    // So do compound and attach configurations: they belong to no target (RUN-5).
    if (const run::RunConfiguration* selected = selectedConfiguration();
        selected != nullptr && selected->type != run::RunType::CMakeTarget && selected->type != run::RunType::MSBuild) {
        return;
    }
    for (const run::RunConfiguration& configuration : m_set.all()) {
        if (configuration.type == run::RunType::CMakeTarget && configuration.target == target) {
            m_selectedId = configuration.id;
            break;
        }
    }
    fillCombo();
    emit runStateChanged();
}

void RunController::applySelection(const run::RunConfiguration& configuration) {
    switch (configuration.type) {
    case run::RunType::MSBuild:
        if (const auto project = projectOf(configuration); project && m_service != nullptr) {
            if (auto set = m_service->setStartupProject(*project); !set) {
                report(set.error());
            }
        }
        break;
    case run::RunType::CMakeTarget:
        if (m_hooks.selectCMakeTarget) {
            m_hooks.selectCMakeTarget(configuration.target);
        }
        break;
    case run::RunType::Native:
    case run::RunType::LuaScript:
    case run::RunType::LuaRepl:
    case run::RunType::FrostEditor:
    case run::RunType::FrostPlayer:
    case run::RunType::Compound:
    case run::RunType::Attach: break;
    }
    emit runStateChanged();
}

std::optional<project::ProjectId> RunController::projectOf(const run::RunConfiguration& configuration) const {
    const auto solution = m_service != nullptr ? m_service->solution() : nullptr;
    if (!solution || configuration.type != run::RunType::MSBuild) {
        return std::nullopt;
    }
    return solution->findProjectByPath(configuration.target);
}

void RunController::refreshStartupCombo() {
    m_set.setGenerated(m_hooks.generatedConfigurations ? m_hooks.generatedConfigurations()
                                                       : std::vector<run::RunConfiguration>{});
    if (!m_selectedId.empty() && m_set.find(m_selectedId) == nullptr) {
        m_selectedId.clear();
    }
    if (m_selectedId.empty()) {
        // The startup project's configuration, else the first one.
        const auto startup = m_service != nullptr ? m_service->startupProject() : std::nullopt;
        for (const run::RunConfiguration& configuration : m_set.all()) {
            if (startup && projectOf(configuration) == startup) {
                m_selectedId = configuration.id;
                break;
            }
        }
        if (m_selectedId.empty() && !m_set.all().empty()) {
            m_selectedId = m_set.all().front().id;
        }
    }
    fillCombo();
    emit configurationsChanged();
}

void RunController::fillCombo() {
    if (!m_combo) {
        return;
    }
    m_fillingCombo = true;
    m_combo->clear();
    const auto solution = m_service != nullptr ? m_service->solution() : nullptr;
    if (!m_set.all().empty()) {
        for (const run::RunConfiguration& configuration : m_set.all()) {
            m_combo->addItem(qs(configuration.name), configurationEntry(configuration.id));
            m_combo->setItemData(
                m_combo->count() - 1,
                QStringLiteral("%1 (%2)").arg(qs(configuration.name), qs(run::toString(configuration.type))),
                Qt::ToolTipRole);
        }
        m_combo->setCurrentIndex(m_combo->findData(configurationEntry(m_selectedId)));
    } else if (solution) {
        // No configurations: the startup candidates (header comment, "What runs").
        for (const project::ProjectId id : m_service->startupCandidates()) {
            m_combo->addItem(qs(solution->project(id).name), projectEntry(id));
        }
        if (const auto startup = m_service->startupProject()) {
            m_combo->setCurrentIndex(m_combo->findData(projectEntry(*startup)));
        }
    }
    if (m_combo->count() > 0 && m_hooks.editConfigurations) {
        m_combo->addItem(tr("Edit Configurations..."), editEntry());
    }
    m_combo->setEnabled(m_combo->count() > 0);
    m_fillingCombo = false;
}

bool RunController::running() const {
    if (m_hooks.alternateRunning && m_hooks.alternateRunning()) {
        return true;
    }
    if (!m_serviceConsoles.empty()) {
        return true;
    }
    return std::ranges::any_of(m_native, [](const auto& entry) { return entry.second.id != 0; });
}

bool RunController::configurationRunning(const std::string& id) const { return configurationRun(id).has_value(); }

std::optional<run::RunId> RunController::configurationRun(const std::string& id) const {
    const auto it = m_native.find(id);
    if (it == m_native.end() || it->second.id == 0) {
        return std::nullopt;
    }
    return it->second.id;
}

bool RunController::canRun() const {
    if (const run::RunConfiguration* selected = selectedConfiguration()) {
        switch (selected->type) {
        case run::RunType::Native:
        case run::RunType::LuaScript:
        case run::RunType::LuaRepl: return m_runs != nullptr;
        case run::RunType::FrostEditor:
        case run::RunType::FrostPlayer:
            return m_runs != nullptr && bool(m_hooks.prepareEngineConfiguration) &&
                   bool(m_hooks.engineLaunchAdmissionFor) && bool(m_hooks.engineLaunchPlanFor);
        case run::RunType::Attach: return false; // Debug-only (RUN-5).
        case run::RunType::Compound:
        case run::RunType::CMakeTarget:
        case run::RunType::MSBuild: return true;
        }
        return true;
    }
    return (m_hooks.alternateAvailable && m_hooks.alternateAvailable()) ||
           (m_service != nullptr && m_service->solution() != nullptr);
}

Result<build::RunId> RunController::run(std::optional<project::ProjectId> project) {
    if (m_hooks.mayRun && !m_hooks.mayRun())
        return fail(ErrorCode::Busy, "stop debugging first");
    if (project) {
        return runService(project);
    }
    if (const run::RunConfiguration* selected = selectedConfiguration()) {
        return runConfiguration(selected->id);
    }
    if (m_hooks.alternateAvailable && m_hooks.alternateAvailable() && m_hooks.alternateRun) {
        return m_hooks.alternateRun();
    }
    return runService(std::nullopt);
}

Result<run::RunId> RunController::runEngineConfiguration(const run::RunConfiguration& configuration) {
    if (!run::isFrostEngineRunType(configuration.type))
        return fail(ErrorCode::InvalidArgument, "Transient engine launch requires an Editor or Player configuration");
    // R7 explicitly adds an independent debugger session. The ordinary Run veto while a primary
    // debugger is live does not apply; runNative still enforces per-configuration project/trust
    // admission, preparation, before-task admission and exact engine reservation.
    if (configurationRunning(configuration.id))
        return fail(ErrorCode::Busy, "This engine configuration is already running");
    auto started = runNative(configuration);
    if (!started)
        return std::unexpected(started.error());
    return *started;
}

Result<build::RunId> RunController::runConfiguration(const std::string& id) {
    if (m_hooks.mayRun && !m_hooks.mayRun())
        return fail(ErrorCode::Busy, "stop debugging first");
    const run::RunConfiguration* found = m_set.find(id);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, std::format("no run configuration '{}'", id));
    }
    const run::RunConfiguration configuration = *found;
    switch (configuration.type) {
    case run::RunType::Native:
    case run::RunType::LuaScript:
    case run::RunType::LuaRepl:
    case run::RunType::FrostEditor:
    case run::RunType::FrostPlayer: return runNative(configuration);
    case run::RunType::CMakeTarget:
        if (!m_hooks.alternateRun) {
            return fail(ErrorCode::Unsupported, "no CMake project is open");
        }
        if (m_hooks.selectCMakeTarget) {
            m_hooks.selectCMakeTarget(configuration.target);
        }
        return m_hooks.alternateRun();
    case run::RunType::MSBuild: {
        const auto project = projectOf(configuration);
        if (!project) {
            return fail(ErrorCode::NotFound,
                        std::format("the project '{}' is not in the open solution", configuration.target));
        }
        return runService(project);
    }
    case run::RunType::Compound: return runCompound(configuration);
    case run::RunType::Attach:
        return fail(ErrorCode::Unsupported,
                    std::format("'{}' attaches the debugger to a running process; use Debug to attach (an attach "
                                "configuration has nothing to run)",
                                configuration.name));
    }
    return fail(ErrorCode::Unsupported, "unknown run configuration type");
}

Result<build::RunId> RunController::runCompound(const run::RunConfiguration& configuration) {
    if (auto valid = run::validateRunConfiguration(configuration); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    // Every member is checked before anything starts (header comment, "What runs").
    std::vector<std::string> members;
    std::size_t cmakeTargets = 0;
    for (const std::string& id : configuration.configurations) {
        const run::RunConfiguration* member = m_set.find(id);
        if (member == nullptr) {
            return fail(ErrorCode::NotFound,
                        std::format("'{}' starts the run configuration '{}', which does not exist (any more)",
                                    configuration.name, id));
        }
        if (member->type == run::RunType::Compound || member->type == run::RunType::Attach) {
            return fail(ErrorCode::Unsupported,
                        std::format("'{}' cannot start '{}': a compound run starts native, CMake and MSBuild "
                                    "configurations, not {} ones",
                                    configuration.name, member->name, run::toString(member->type)));
        }
        if (member->type == run::RunType::CMakeTarget && ++cmakeTargets > 1) {
            return fail(ErrorCode::Unsupported,
                        std::format("'{}' starts more than one CMake target; the CMake integration builds and runs "
                                    "one target at a time, so a compound run can hold one of them",
                                    configuration.name));
        }
        members.push_back(id);
    }
    // The workspace's trust gate once for the whole compound (one prompt, whose retry reruns it).
    if (m_hooks.mayRunConfiguration) {
        if (auto allowed = m_hooks.mayRunConfiguration(configuration); !allowed) {
            return std::unexpected(std::move(allowed.error()));
        }
    }
    std::optional<build::RunId> first;
    std::vector<Error> errors;
    for (const std::string& id : members) {
        auto started = runConfiguration(id);
        if (started) {
            first = first.value_or(*started);
            continue;
        }
        if (started.error().code() == ErrorCode::Cancelled) {
            continue; // The user declined to rerun a member that is still running.
        }
        Error error = started.error();
        static_cast<void>(error.withContext(std::format("starting '{}' of '{}'", id, configuration.name)));
        errors.push_back(std::move(error));
    }
    if (!first) {
        if (!errors.empty()) {
            return std::unexpected(std::move(errors.front()));
        }
        return fail(ErrorCode::Cancelled, std::format("no member of '{}' was started", configuration.name));
    }
    for (const Error& error : errors) {
        report(error); // Some members run: say which ones did not.
    }
    return *first;
}

Result<build::RunId> RunController::runService(std::optional<project::ProjectId> project) {
    if (m_service == nullptr) {
        return fail(ErrorCode::InvalidArgument, "no solution is open");
    }
    auto started = m_service->run(project);
    if (started) {
        // The tab opens now, so the Run tool window shows the run from its build on.
        if (const auto info = m_service->runInfo(*started)) {
            (void)openServiceTab(*info);
        }
        emit runStateChanged();
        return started;
    }
    if (started.error().code() != ErrorCode::Busy) {
        return started;
    }
    // Busy: the project's own run asks to stop and rerun; a build in progress just says so.
    const std::optional<project::ProjectId> id = project ? project : m_service->startupProject();
    if (!id || !m_service->activeRun(*id)) {
        return started;
    }
    const auto solution = m_service->solution();
    const QString name = solution ? qs(solution->project(*id).name) : QString();
    if (m_hooks.confirmRerun && !m_hooks.confirmRerun(name)) {
        return fail(ErrorCode::Cancelled, std::format("{} is still running", name.toStdString()));
    }
    return m_service->rerun(id);
}

void RunController::notifyExplicitEngineStop(run::RunId id) {
    if (!m_hooks.engineStopRequested || !m_runs)
        return;
    if (std::ranges::any_of(m_native, [id](const auto& entry) { return entry.second.id == id && entry.second.engine; }))
        m_hooks.engineStopRequested(id);
}

Result<build::RunId> RunController::runNative(const run::RunConfiguration& configuration) {
    if (configuration.type == run::RunType::LuaRepl && !m_window)
        return fail(ErrorCode::Unsupported, "Lua REPL requires the interactive Run console");
    if (m_runs == nullptr) {
        return fail(ErrorCode::Unsupported, "native run configurations need the RunManager");
    }
    if (auto valid = run::validateRunConfiguration(configuration); !valid) {
        return std::unexpected(std::move(valid.error()));
    }
    if (const auto active = configurationRun(configuration.id)) {
        if (m_hooks.confirmRerun && !m_hooks.confirmRerun(qs(configuration.name))) {
            return fail(ErrorCode::Cancelled, std::format("{} is still running", configuration.name));
        }
        // Rerun: the run ends (its whole tree), then nativeFinished starts it again.
        m_native[configuration.id].rerunPending = true;
        notifyExplicitEngineStop(*active);
        m_runs->stop(*active);
        return build::RunId{*active};
    }
    if (m_hooks.mayRunConfiguration) {
        if (auto allowed = m_hooks.mayRunConfiguration(configuration); !allowed) {
            return std::unexpected(std::move(allowed.error()));
        }
    }
    run::RunConfiguration launchConfiguration = configuration;
    const bool engine = run::isFrostEngineRunType(configuration.type);
    if (engine) {
        if (!m_hooks.prepareEngineConfiguration) {
            return fail(ErrorCode::Unsupported, "FrostEngine session integration is unavailable");
        }
        if (auto valid = run::validateRunConfiguration(configuration); !valid) {
            return std::unexpected(std::move(valid.error()));
        }
        auto prepared = m_hooks.prepareEngineConfiguration(configuration);
        if (!prepared) {
            return std::unexpected(std::move(prepared.error()));
        }
        launchConfiguration = std::move(*prepared);
        if (launchConfiguration.id != configuration.id || launchConfiguration.type != configuration.type ||
            launchConfiguration.program != configuration.program ||
            launchConfiguration.engineProjectPath != configuration.engineProjectPath ||
            launchConfiguration.engineDebugMode != configuration.engineDebugMode) {
            Error error(ErrorCode::InvalidArgument, "engine preparation changed the run identity, program or project");
            if (m_hooks.engineProcessFinished) {
                run::RunResult failed;
                failed.configurationId = configuration.id;
                failed.error = error;
                m_hooks.engineProcessFinished(failed);
            }
            return std::unexpected(std::move(error));
        }
    }
    const std::string id = configuration.id;
    RunConsole* console = nullptr;
    if (m_window) {
        RunTabHandlers handlers;
        handlers.stop = [this, id] {
            if (const auto active = configurationRun(id); active && m_runs != nullptr) {
                notifyExplicitEngineStop(*active);
                m_runs->stop(*active);
            }
        };
        handlers.rerun = [this, id] {
            if (const auto active = configurationRun(id); active && m_runs != nullptr) {
                m_native[id].rerunPending = true;
                notifyExplicitEngineStop(*active);
                m_runs->stop(*active);
                return;
            }
            if (auto again = runConfiguration(id); !again && again.error().code() != ErrorCode::Cancelled) {
                report(again.error());
            }
        };
        handlers.input = [this, id](const QString& line) {
            if (const auto active = configurationRun(id); active && m_runs != nullptr) {
                if (auto sent = m_runs->sendInput(*active, line.toStdString() + "\n"); !sent) {
                    report(sent.error());
                }
            }
        };
        console = m_window->openRun(configurationKey(id), qs(configuration.name), std::move(handlers));
    }
    const QPointer<RunConsole> target(console);
    const std::shared_ptr<Relay> relay = m_relay;
    run::RunHandlers handlers;
    if (run::isLuaRunType(configuration.type)) {
        if (m_hooks.launchAdmissionFor)
            handlers.launchAdmission = m_hooks.launchAdmissionFor(configuration);
        if (!handlers.launchAdmission)
            return fail(ErrorCode::Unsupported, "standalone Lua has no current project/trust admission");
    }
    // Fixed per-run identity: never resolve a configuration id to a later rerun in queued output.
    const auto engineRunId = std::make_shared<std::atomic<run::RunId>>(0);
    const auto engineStarted = std::make_shared<std::atomic<bool>>(false);
    if (engine) {
        if (m_hooks.engineLaunchAdmissionFor) {
            handlers.launchAdmission = m_hooks.engineLaunchAdmissionFor(launchConfiguration);
        }
        if (!handlers.launchAdmission) {
            handlers.launchAdmission = []() -> Result<void> {
                return fail(ErrorCode::PermissionDenied, "FrostEngine launch has no current trust/session admission");
            };
        }
        std::function<Result<run::LaunchPlan>(run::LaunchPlan)> resolve;
        if (m_hooks.engineLaunchPlanFor) {
            resolve = m_hooks.engineLaunchPlanFor(launchConfiguration);
        }
        handlers.transformLaunchPlan = [resolve = std::move(resolve)](run::LaunchPlan plan) -> Result<run::LaunchPlan> {
            if (!resolve) {
                return fail(ErrorCode::PermissionDenied, "FrostEngine launch has no physical project resolver");
            }
            const run::LaunchPlan original = plan;
            auto transformed = resolve(std::move(plan));
            if (!transformed) {
                return std::unexpected(std::move(transformed.error()));
            }
            if (original.arguments.size() < 2 || original.arguments[0] != "--project" ||
                transformed->arguments.size() != original.arguments.size() ||
                !io::normalizePath(transformed->arguments[1])) {
                return fail(ErrorCode::InvalidArgument,
                            "engine resolution did not preserve an absolute project argument");
            }
            // This transient seam pins the physical project; it cannot change the saved identity,
            // executable, environment, working directory or any caller-supplied engine options.
            run::LaunchPlan expected = original;
            expected.arguments[1] = transformed->arguments[1];
            if (*transformed != expected) {
                return fail(ErrorCode::InvalidArgument,
                            "engine resolution changed fields other than the project argument");
            }
            return transformed;
        };
        if (console) {
            console->appendOutput(build::RunOutputKind::System, "Engine output is shown in Engine Console.\n");
        }
    }
    handlers.onPhaseChanged = [relay, target, engine, engineStarted](const run::RunStatus& status) {
        if (engine && status.phase == run::RunPhase::Running) {
            engineStarted->store(true);
        }
        relay->post([target, status, engine](RunController* self) {
            if (engine && status.phase == run::RunPhase::Running && self->m_hooks.engineProcessStarted) {
                self->m_hooks.engineProcessStarted(status);
            }
            if (target) {
                if (!engine && status.phase == run::RunPhase::Running && !status.commandLine.empty()) {
                    target->appendOutput(build::RunOutputKind::System, status.commandLine + "\n");
                }
                if (status.phase == run::RunPhase::BeforeLaunch && !status.beforeLaunchTask.empty()) {
                    // RUN-5: which before-launch task runs now; its own output follows.
                    target->appendOutput(build::RunOutputKind::System,
                                         "Before launch: " + status.beforeLaunchTask + "\n");
                }
                target->setRunning(status.phase == run::RunPhase::Running, status.acceptsInput);
            }
            emit self->runStateChanged();
        });
    };
    handlers.onOutput = [relay, target, engineRunId, engine, engineStarted](run::OutputStream stream,
                                                                            std::string_view text) {
        if (engine && !engineStarted->load()) {
            return; // Before-launch task output is not an engine discovery record.
        }
        relay->post([target, stream, text = std::string(text), engineRunId, engine](RunController* self) {
            if (engine) {
                // The startup record carries credentials. Raw engine streams are private session
                // input, never RunConsole text; authenticated push logs feed Engine Console.
                if (self->m_hooks.engineProcessOutput) {
                    self->m_hooks.engineProcessOutput(engineRunId->load(), stream, text);
                }
                return;
            }
            if (target) {
                target->appendOutput(stream == run::OutputStream::Stdout ? build::RunOutputKind::Stdout
                                                                         : build::RunOutputKind::Stderr,
                                     text);
            }
        });
    };
    handlers.onFinished = [relay, id, engine](const run::RunResult& result) {
        relay->post([id, result, engine](RunController* self) {
            if (engine && self->m_hooks.engineProcessFinished) {
                self->m_hooks.engineProcessFinished(result);
            }
            self->nativeFinished(id, result);
        });
    };
    auto started = m_runs->start(launchConfiguration, std::move(handlers));
    if (!started) {
        if (engine && m_hooks.engineProcessFinished) {
            run::RunResult failed;
            failed.configurationId = configuration.id;
            failed.error = started.error();
            m_hooks.engineProcessFinished(failed);
        }
        if (console) {
            build::RunOutcome failed;
            failed.error = started.error();
            console->finish(failed);
        }
        return std::unexpected(std::move(started.error()));
    }
    engineRunId->store(*started);
    NativeRun& entry = m_native[id];
    entry.id = *started;
    entry.console = console;
    entry.rerunPending = false;
    entry.engine = engine;
    if (engine && m_hooks.engineRunAccepted) {
        m_hooks.engineRunAccepted(*started, launchConfiguration);
    }
    emit runStateChanged();
    return build::RunId{*started};
}

void RunController::nativeFinished(const std::string& id, const run::RunResult& result) {
    const auto it = m_native.find(id);
    if (it == m_native.end() || it->second.id != result.id) {
        return;
    }
    const bool rerun = it->second.rerunPending;
    if (it->second.console) {
        it->second.console->finish(runOutcomeFor(result));
    }
    m_native.erase(it);
    emit runStateChanged();
    if (rerun) {
        if (auto again = runConfiguration(id); !again && again.error().code() != ErrorCode::Cancelled) {
            report(again.error());
        }
    }
}

Result<build::RunId> RunController::rerun() {
    if (m_hooks.mayRun && !m_hooks.mayRun())
        return fail(ErrorCode::Busy, "stop debugging first");
    // The current tab's run: its configuration, its project, or the CMake target.
    const QString key = m_window ? m_window->keyOf(m_window->currentConsole()) : QString();
    if (key.startsWith(QStringLiteral("config:"))) {
        const std::string id = key.mid(7).toStdString();
        if (const auto active = configurationRun(id); active && m_runs != nullptr) {
            m_native[id].rerunPending = true;
            notifyExplicitEngineStop(*active);
            m_runs->stop(*active);
            return build::RunId{*active};
        }
        return runConfiguration(id);
    }
    if (key.startsWith(QStringLiteral("project:")) && m_service != nullptr) {
        return m_service->rerun(project::ProjectId{key.mid(8).toUInt()});
    }
    const run::RunConfiguration* selected = selectedConfiguration();
    const bool selectedIsTarget = selected == nullptr || selected->type == run::RunType::CMakeTarget;
    if (m_hooks.alternateAvailable && m_hooks.alternateAvailable() && m_hooks.alternateRerun &&
        (key.startsWith(QStringLiteral("cmake:")) || (key.isEmpty() && selectedIsTarget))) {
        return m_hooks.alternateRerun();
    }
    if (m_service != nullptr && m_service->solution() != nullptr && selectedConfiguration() == nullptr) {
        return m_service->rerun(m_currentProject);
    }
    return run();
}

void RunController::stop() {
    if (m_window && m_window->stopCurrent()) {
        return;
    }
    // Nothing runs in the current tab: Stop ends every run (Rider offers a list; all is the honest default).
    if (m_hooks.alternateRunning && m_hooks.alternateRunning() && m_hooks.alternateStop) {
        m_hooks.alternateStop();
    }
    if (m_service != nullptr) {
        for (const auto& [run, console] : m_serviceConsoles) {
            m_service->stop(run);
        }
    }
    if (m_runs != nullptr) {
        for (const auto& [id, entry] : m_native) {
            if (entry.id != 0) {
                notifyExplicitEngineStop(entry.id);
                m_runs->stop(entry.id);
            }
        }
    }
}

Result<void> RunController::setStartupProject(project::ProjectId project) {
    if (m_service == nullptr) {
        return fail(ErrorCode::InvalidArgument, "no solution is open");
    }
    return m_service->setStartupProject(project);
}

RunConsole* RunController::openServiceTab(const build::RunInfo& info) {
    if (const auto it = m_serviceConsoles.find(info.id); it != m_serviceConsoles.end()) {
        return it->second.data();
    }
    m_current = info.id;
    m_currentProject = info.project;
    m_currentActive = true;
    RunConsole* console = nullptr;
    if (m_window) {
        const project::ProjectId project = info.project;
        RunTabHandlers handlers;
        handlers.stop = [this, run = info.id] {
            if (m_service != nullptr) {
                m_service->stop(run);
            }
        };
        handlers.rerun = [this, project] {
            if (m_service == nullptr) {
                return;
            }
            if (auto again = m_service->rerun(project); !again) {
                report(again.error());
            }
        };
        handlers.input = [this, run = info.id](const QString& line) {
            if (m_service == nullptr) {
                return;
            }
            if (auto sent = m_service->sendInput(run, line.toStdString() + "\n"); !sent) {
                report(sent.error());
            }
        };
        const QString title = info.configuration.name().empty()
                                  ? qs(info.projectName)
                                  : QStringLiteral("%1 [%2]").arg(qs(info.projectName), qs(info.configuration.name()));
        console = m_window->openRun(projectKey(project), title, std::move(handlers));
    }
    m_serviceConsoles[info.id] = console;
    return console;
}

void RunController::onStarted(const build::RunInfo& info) {
    if (info.debug) {
        return; // The Debug tool window shows Debug runs (frost/ui/debug_controller.hpp).
    }
    (void)openServiceTab(info);
    emit runStateChanged();
}

void RunController::onStateChanged(const build::RunInfo& info) {
    const auto it = m_serviceConsoles.find(info.id);
    if (it == m_serviceConsoles.end()) {
        return;
    }
    const bool live = info.state == build::RunState::Running || info.state == build::RunState::Stopping;
    if (it->second && live) {
        it->second->setRunning(info.state == build::RunState::Running, info.acceptsInput);
    }
    emit runStateChanged();
}

void RunController::onOutput(build::RunId run, build::RunOutputKind kind, const std::string& text) {
    if (const auto it = m_serviceConsoles.find(run); it != m_serviceConsoles.end() && it->second) {
        it->second->appendOutput(kind, text);
    }
}

void RunController::onFinished(const build::RunOutcome& outcome) {
    const auto it = m_serviceConsoles.find(outcome.run.id);
    if (it == m_serviceConsoles.end()) {
        return;
    }
    if (it->second) {
        it->second->finish(outcome);
    }
    m_serviceConsoles.erase(it);
    if (outcome.run.id == m_current) {
        m_currentActive = false;
    }
    emit runStateChanged();
}

void RunController::onStartupChanged(std::optional<project::ProjectId> project) {
    if (project) {
        // The msbuild configuration of the new startup project, if it has one.
        for (const run::RunConfiguration& configuration : m_set.all()) {
            if (projectOf(configuration) == project) {
                m_selectedId = configuration.id;
                break;
            }
        }
    }
    if (!m_combo) {
        return;
    }
    if (!m_set.all().empty()) {
        fillCombo();
        return;
    }
    if (!project) {
        refreshStartupCombo();
        return;
    }
    const int index = m_combo->findData(projectEntry(*project));
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
