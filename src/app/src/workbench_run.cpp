// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): Run and Debug, the run service, the RunManager and their
// controller, attaching Run to the window (the Run tool window v2 and the configuration combo,
// RUN-3), Lua debugging's availability, Attach to Process and the compound-launch refusal (split
// from workbench.cpp, FND-12a). The run configurations are workbench_run_configurations.cpp's.
// RUN-5: Debug runs a configuration's before-launch tasks first, attaches for an attach
// configuration and refuses a compound one (DBG-10).

#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_manager.hpp"
#include "frost/run/run_types.hpp"
#include "frost/run/vs_import.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/debug_actions.hpp"
#include "frost/ui/debug_retirement.hpp"
#include "frost/ui/debug_session_registry.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/find/find_feature.hpp"
#include "frost/ui/run_actions.hpp"
#include "frost/ui/run_configurations_dialog.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/run_tool_window.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/terminal/terminal_tool_window.hpp"
#include "frost/ui/toolwindows/build_actions.hpp"
#include "frost/ui/toolwindows/build_output.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"
#include "frost/ui/tr.hpp"
#include "frost/vcs/git_locator.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "managed_debug_preparation.hpp"
#include "multi_session_debug.hpp"
#include "project_properties_dialog.hpp"
#include "settings_dialog.hpp"
#include "unity_control.hpp"
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QScopeGuard>

#include <algorithm>
#include <utility>

namespace frost::app {

namespace key = settings::key;

namespace {

constexpr std::string_view kLog = "workbench";

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

} // namespace

void Workbench::createRunService() {
    // The Run tool window exists before the window (the actions drive it); attachRun() docks it.
    auto* runWindow = new ui::RunToolWindow();
    m_runWindow = runWindow;
    ui::RunControllerHooks hooks;
    hooks.confirmRerun = [this](const QString& project) { return m_services.prompts.confirmRerun(m_window, project); };
    hooks.reportError = [this](const QString& message) { notify(text("Run"), message, ui::NotificationKind::Error); };
    hooks.mayRun = [this] {
        return !m_buildService->projectEditInProgress() &&
               (!m_debug || (!m_debug->debugging() && !m_debug->rerunPending()));
    };
    hooks.alternateAvailable = [this] { return m_cmake != nullptr; };
    hooks.alternateRunning = [this] { return m_cmake && m_cmake->running(); };
    hooks.alternateRun = [this] { return m_cmake->run(); };
    hooks.alternateRerun = [this] { return m_cmake->rerun(); };
    hooks.alternateStop = [this] { m_cmake->stop(); };
    // RUN-2/RUN-3: the run configurations of the open workspace (workbench_run_configurations.cpp).
    hooks.selectCMakeTarget = [this](const std::string& target) {
        if (!m_cmake)
            return;
        m_cmake->selectRunTarget(target);
        // Not applied while the targets window shows an older model: the observer applies it later.
        m_pendingCMakeTarget = m_cmake && m_cmake->runTargetName() != target ? target : std::string();
    };
    hooks.mayRunConfiguration = [this](const run::RunConfiguration& configuration) {
        return mayRunConfiguration(configuration);
    };
    hooks.launchAdmissionFor = [this](const run::RunConfiguration& configuration) {
        return luaLaunchAdmissionFor(configuration);
    };
    hooks.prepareEngineConfiguration =
        [this](const run::RunConfiguration& configuration) -> Result<run::RunConfiguration> {
        if (!m_engine)
            return fail(ErrorCode::Unsupported, "FrostEngine integration is unavailable");
        return m_engine->prepareRun(configuration);
    };
    hooks.engineLaunchAdmissionFor =
        [this](const run::RunConfiguration& configuration) -> std::function<Result<void>()> {
        return m_engine ? m_engine->launchAdmissionFor(configuration) : std::function<Result<void>()>{};
    };
    hooks.engineLaunchPlanFor =
        [this](const run::RunConfiguration& configuration) -> std::function<Result<run::LaunchPlan>(run::LaunchPlan)> {
        return m_engine ? m_engine->launchPlanFor(configuration)
                        : std::function<Result<run::LaunchPlan>(run::LaunchPlan)>{};
    };
    hooks.engineRunAccepted = [this](run::RunId id, const run::RunConfiguration& configuration) {
        if (m_engine)
            m_engine->runAccepted(id, configuration);
    };
    hooks.engineProcessStarted = [this](const run::RunStatus& run) {
        if (m_engine)
            m_engine->processStarted(run);
    };
    hooks.engineProcessOutput = [this](run::RunId id, run::OutputStream stream, std::string_view bytes) {
        if (m_engine)
            m_engine->processOutput(id, stream, bytes);
    };
    hooks.engineProcessFinished = [this](const run::RunResult& result) {
        if (m_engine)
            m_engine->processFinished(result);
    };
    hooks.engineStopRequested = [this](run::RunId id) {
        if (m_engine)
            m_engine->explicitRunStop(id);
    };
    hooks.generatedConfigurations = [this] { return generatedRunConfigurations(); };
    hooks.saveConfigurations = [this](const std::vector<run::RunConfiguration>& stored) {
        return saveRunConfigurations(stored);
    };
    m_runController = std::make_unique<ui::RunController>(runWindow, std::move(hooks));
    /*
     * RUN-1: run configurations start through the RunManager: native ones from the controller, CMake
     * targets from the CMake integration once built. MSBuild projects run through RunService, which
     * builds them and asks MSBuild for TargetPath and the .vcxproj.user debugger settings.
     */
    m_runManager = run::RunManager::create(m_services.launcher);
    m_runController->setRunManager(m_runManager.get());
    connect(this, &Workbench::projectChanged, this, [this] { loadRunConfigurations(); });
    m_runBuilder = build::makeRunBuilder(*m_buildService);
    const auto snapshot = m_services.settings.snapshot();
    build::RunServiceOptions options;
    options.stateFilePath = m_options.runStatePath;
    options.msbuildPath = !m_options.msbuildPath.empty() ? m_options.msbuildPath : snapshot->get(key::toolsMsbuildPath);
    options.externalConsoleByDefault = m_options.runExternalConsole;
    m_runService = build::RunService::create(m_services.fs, m_services.launcher, m_services.trust, *m_runBuilder,
                                             std::move(options), m_runController->callbacks());
    m_runController->setService(m_runService.get());
    m_debugRetirements = std::make_shared<ui::DebugRetirementBatch>();
    ui::DebugControllerOptions debugOptions;
    debugOptions.retirements = m_debugRetirements;
    debugOptions.trust = &m_services.trust;
    debugOptions.fs = &m_services.fs;
    debugOptions.launcher = &m_services.launcher;
    debugOptions.natvisFiles = [this](const debug::DebugTarget& target) { return discoverProjectVisualizers(target); };
    debugOptions.dbgEngPathSetting = [this] {
        const auto configured = m_services.settings.snapshot()->get(key::toolsDbgEngPath);
        return configured.empty() ? QDir(QCoreApplication::applicationDirPath())
                                        .filePath("frost-dbgeng-dap.exe")
                                        .toUtf8()
                                        .toStdString()
                                  : configured;
    };
    debugOptions.adapterFactory = m_options.debugAdapterFactory;
    debugOptions.lldbDapSetting = [this] { return m_services.settings.snapshot()->get(key::toolsLldbDapPath); };
    debugOptions.nativeBackendSetting = [this] { return m_services.settings.snapshot()->get(key::debugNativeBackend); };
    debugOptions.gdbSetting = [this] { return m_services.settings.snapshot()->get(key::toolsGdbPath); };
    debugOptions.sourceMapSetting = [this] {
        const auto snapshot = m_services.settings.snapshot();
        return std::pair{snapshot->get(key::debugSourceMapFrom), snapshot->get(key::debugSourceMapTo)};
    };
    ui::DebugControllerHooks debugHooks;
    debugHooks.currentEditor = [this]() -> ui::EditorView* {
        auto* view = currentView();
        return view ? &view->editor() : nullptr;
    };
    debugHooks.openSource = [this](const QString& path, int line) -> ui::EditorView* {
        if (m_managedDebugSnapshot) {
            showManagedDebugSource(path.toStdString(), line);
            return nullptr;
        }
        if (!openPath(path.toStdString(), FileLocation{line, 1}))
            return nullptr;
        auto* view = currentView();
        return view ? &view->editor() : nullptr;
    };
    debugHooks.notify = [this](const QString& title, const QString& message, bool error) {
        notify(title, message, error ? ui::NotificationKind::Error : ui::NotificationKind::Warning);
    };
    debugHooks.showToolWindow = [this] {
        if (m_window)
            m_window->toolWindows().show(ui::kDebugToolWindowId, false);
    };
    // A native configuration debugs its program as configured; CMake targets are built first.
    const auto selectedType = [this]() -> std::optional<run::RunType> {
        const run::RunConfiguration* selected = m_runController ? m_runController->selectedConfiguration() : nullptr;
        return selected != nullptr ? std::optional<run::RunType>(selected->type) : std::nullopt;
    };
    const auto nativeSelected = [selectedType] { return selectedType() == run::RunType::Native; };
    debugHooks.alternateAvailable = [this, selectedType] {
        const auto type = selectedType();
        if (type && run::isFrostEngineRunType(*type))
            return m_engineRunDebug != nullptr;
        return m_cmake != nullptr || type == run::RunType::Native || (type && run::isLuaRunType(*type)) ||
               type == run::RunType::Attach || type == run::RunType::Compound;
    };
    /*
     * RUN-5: an attach configuration attaches to the one running process whose executable it names
     * (as Attach to Process does for a picked PID, with the same trust prompt). Compound and typed
     * engine configurations use the registry preparation path. Debug reports preparation errors.
     */
    debugHooks.startOverride = [this, selectedType]() -> std::optional<Result<void>> {
        const auto type = selectedType();
        if (type && run::isFrostEngineRunType(*type))
            return startSelectedDebugConfiguration();
        if (type && run::isLuaRunType(*type))
            return startStandaloneLuaDebug(*m_runController->selectedConfiguration());
        if (type == run::RunType::Compound) {
            return startCompoundDebug(*m_runController->selectedConfiguration());
        }
        if (type != run::RunType::Attach) {
            return std::nullopt;
        }
        const run::RunConfiguration configuration = *m_runController->selectedConfiguration();
        std::vector<RunningProcessInfo> matches;
        for (RunningProcessInfo& process : enumerateRunningProcesses()) {
            if (run::attachTargetMatches(configuration.target, process.executablePath))
                matches.push_back(std::move(process));
        }
        if (matches.size() != 1) {
            QString pids;
            for (const RunningProcessInfo& process : matches)
                pids += (pids.isEmpty() ? QString() : QStringLiteral(", ")) + QString::number(process.pid);
            const QString message =
                matches.empty()
                    ? text("No running process matches %1. Start it first, or use Attach to Process...")
                          .arg(qs(configuration.target))
                    : text("%1 running processes match %2 (PIDs %3); use Attach to Process... to choose one.")
                          .arg(matches.size())
                          .arg(qs(configuration.target), pids);
            notify(text("Debug"), message, ui::NotificationKind::Warning);
            return Result<void>(
                fail(matches.empty() ? ErrorCode::NotFound : ErrorCode::InvalidArgument, message.toStdString()));
        }
        attachToProcess(matches.front().pid, matches.front().executablePath, matches.front().name,
                        matches.front().isDotNet);
        return Result<void>{};
    };
    /*
     * RUN-5: a configuration's before-launch tasks run before the debugger starts its program, in
     * the RunManager's Prepare mode (the same tasks Run runs); a failing task ends the Debug with
     * its error. Without tasks the target goes to the debugger at once.
     *
     * The tasks are the workspace's programs, so they need its trust first, as Run's do
     * (mayRunConfiguration; the debugger's own trust gate only runs after them). In an untrusted
     * workspace nothing starts: the trust prompt asks, and once the user trusts the workspace it
     * starts this Debug again if the configuration is still the selected one.
     */
    const auto primaryPrepareRun = std::make_shared<std::optional<run::RunId>>();
    const auto primaryCMakePrepare = std::make_shared<bool>(false);
    const auto prepareThenDebug =
        [this, primaryPrepareRun](run::RunConfiguration prepared, debug::DebugTarget target,
                                  std::function<void(Result<debug::DebugTarget>)> done) -> Result<void> {
        if (prepared.beforeLaunchTasks.empty() || !m_runManager) {
            done(std::move(target));
            return {};
        }
        const auto retryDebug = [this, id = prepared.id] {
            const run::RunConfiguration* selected = m_runController->selectedConfiguration();
            if (!m_debug || selected == nullptr || selected->id != id)
                return;
            if (auto started = m_debug->debug(); !started)
                notify(text("Debug"), qs(started.error().message()), ui::NotificationKind::Error);
        };
        if (auto trusted = mayRunConfiguration(prepared, retryDebug); !trusted)
            return std::unexpected(std::move(trusted.error()));
        prepared.type = run::RunType::Native;
        prepared.target.clear();
        prepared.beforeLaunch = run::BeforeLaunch::None;
        prepared.program = target.program;
        prepared.workingDirectory = target.workingDirectory;
        const QPointer<Workbench> self(this);
        run::RunHandlers handlers;
        handlers.onFinished = [self, target, done = std::move(done), primaryPrepareRun](const run::RunResult& result) {
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, target, done, result, primaryPrepareRun] {
                    if (!self)
                        return;
                    if (*primaryPrepareRun == result.id)
                        primaryPrepareRun->reset();
                    if (result.error)
                        done(std::unexpected(*result.error));
                    else if (result.stopped)
                        done(fail(ErrorCode::Cancelled, "the before-launch tasks were stopped"));
                    else
                        done(target);
                },
                Qt::QueuedConnection);
        };
        auto started = m_runManager->start(prepared, std::move(handlers), run::RunMode::Prepare);
        if (!started)
            return std::unexpected(std::move(started.error()));
        *primaryPrepareRun = *started;
        return {};
    };
    debugHooks.mayStart = [this] {
        return !m_buildService->projectEditInProgress() && !m_runController->running() &&
               (!m_runManager || m_runManager->activeRuns().empty()) && m_runService->activeRuns().empty() &&
               (!m_managedDebugPreparation || !m_managedDebugPreparation->active()) && !m_managedSnapshotJob;
    };
    debugHooks.startAlternate = [this, nativeSelected, prepareThenDebug,
                                 primaryCMakePrepare](auto done) -> Result<void> {
        if (nativeSelected()) {
            const run::RunConfiguration configuration = *m_runController->selectedConfiguration();
            auto plan = run::launchPlanFor(configuration);
            if (!plan)
                return std::unexpected(std::move(plan.error()));
            debug::DebugTarget target;
            target.name = configuration.name;
            target.program = plan->program;
            target.arguments = plan->arguments;
            target.workingDirectory = plan->workingDirectory;
            // lldb-dap sets variables over the inherited environment; a removal cannot be passed on.
            for (const run::EnvironmentEntry& entry : plan->environment)
                if (entry.value)
                    target.environment.emplace_back(entry.name, *entry.value);
            // The debugger's own trust gate: the workspace the configuration belongs to.
            target.trustRoots = {m_projectRoot};
            return prepareThenDebug(configuration, std::move(target), std::move(done));
        }
        if (!m_cmake)
            return fail(ErrorCode::InvalidArgument, "no CMake project is open");
        // The target's configuration (with the user's edits) names its before-launch tasks (RUN-5).
        *primaryCMakePrepare = true;
        auto accepted = m_cmake->debug(
            [this, prepareThenDebug, primaryCMakePrepare, done = std::move(done)](Result<debug::DebugTarget> target) {
                *primaryCMakePrepare = false;
                const std::string id =
                    run::runConfigurationIdFor(run::kCMakeIdPrefix, m_cmake ? m_cmake->runTargetName() : std::string());
                const run::RunConfiguration* edited =
                    m_runController ? m_runController->configurations().find(id) : nullptr;
                if (!target || edited == nullptr || edited->beforeLaunchTasks.empty()) {
                    done(std::move(target));
                    return;
                }
                if (auto prepared = prepareThenDebug(*edited, std::move(*target), done); !prepared)
                    done(std::unexpected(std::move(prepared.error())));
            });
        if (!accepted)
            *primaryCMakePrepare = false;
        return accepted;
    };
    debugHooks.stopAlternate = [this, primaryPrepareRun, primaryCMakePrepare] {
        if (*primaryCMakePrepare && m_cmake) {
            *primaryCMakePrepare = false;
            m_cmake->stop();
        }
        // Cancel only the primary controller's own preparation, never a compound sibling's task.
        if (m_runManager && primaryPrepareRun->has_value()) {
            m_runManager->stop(**primaryPrepareRun);
            primaryPrepareRun->reset();
        }
    };
    ui::DebugControllerHooks childHooks;
    childHooks.currentEditor = debugHooks.currentEditor;
    childHooks.openSource = [this](const QString& path, int line) -> ui::EditorView* {
        if (!openPath(path.toStdString(), FileLocation{line, 1}))
            return nullptr;
        auto* view = currentView();
        return view ? &view->editor() : nullptr;
    };
    childHooks.notify = debugHooks.notify;
    childHooks.showToolWindow = debugHooks.showToolWindow;
    auto childOptions = debugOptions;
    childOptions.allowRerun = false;
    m_debug = std::make_unique<ui::DebugController>(m_breakpoints, std::move(debugOptions), std::move(debugHooks));
    m_debugSessions = std::make_unique<ui::DebugSessionRegistry>(
        m_breakpoints, *m_debug, [childOptions, childHooks](debug::BreakpointManager& breakpoints) {
            return std::make_unique<ui::DebugController>(breakpoints, childOptions, childHooks);
        });
    m_compoundDebug = std::make_unique<MultiSessionDebugCoordinator>(*m_debugSessions);
    connect(m_compoundDebug.get(), &MultiSessionDebugCoordinator::failed, this,
            [this](const QString& error) { notify(text("Debug"), error, ui::NotificationKind::Error); });
    const auto refreshDebug = [this] {
        if (m_window)
            m_window->refreshActions();
    };
    connect(m_debugSessions.get(), &ui::DebugSessionRegistry::sessionStateChanged, this, refreshDebug);
    connect(m_debugSessions.get(), &ui::DebugSessionRegistry::selectedChanged, this, refreshDebug);
    connect(m_compoundDebug.get(), &MultiSessionDebugCoordinator::stateChanged, this, refreshDebug);
    connect(m_debug.get(), &ui::DebugController::stateChanged, this, [this] {
        if (m_managedDebugSnapshot && !m_debug->debugging())
            invalidateManagedDebug();
    });
    m_managedDebugPreparation =
        std::make_unique<ManagedDebugPreparation>(m_services.launcher, m_services.trust, systemClock());
    m_debug->setRunService(m_runService.get());
    if (auto registered = ui::registerDebugActions(
            m_registry, *m_debugSessions, [this] { return startSelectedDebugConfiguration(); },
            [this] {
                return !m_shutDown && !m_projectRoot.empty() && !m_compoundDebug->busy() &&
                       !m_buildService->projectEditInProgress() && m_buildService->state() == build::BuildState::Idle &&
                       (!m_managedDebugPreparation || !m_managedDebugPreparation->active()) && !m_managedSnapshotJob;
            });
        !registered)
        log::warn(kLog, "debug actions: {}", registered.error().describe());
    input::ActionSpec managedDebug;
    managedDebug.id = "run.debugManaged";
    managedDebug.name = "Debug .NET Project";
    managedDebug.description = "Build and debug a trusted .NET console project";
    managedDebug.group = "Debug";
    managedDebug.executesProjectCode = true;
    managedDebug.enabled = [this](const input::ActionContext&) {
        return !m_projectRoot.empty() && !m_buildService->projectEditInProgress() && m_debug && !m_debug->debugging() &&
               m_managedDebugPreparation && !m_managedDebugPreparation->active() && !m_managedSnapshotJob;
    };
    managedDebug.handler = [this](const input::ActionContext&, const input::InvokeInfo&) { startManagedDebug(); };
    if (auto added = m_registry.add(std::move(managedDebug)); !added)
        log::warn(kLog, "managed debug action: {}", added.error().describe());
    input::ActionSpec attachProcess;
    attachProcess.id = "debug.attachToProcess";
    attachProcess.name = "Attach to Process...";
    attachProcess.description =
        "Attach the native C++ (lldb-dap) or .NET (netcoredbg) debugger to a running local process";
    attachProcess.group = "Debug";
    attachProcess.synonyms = {"attach", "pid", "debug process"};
    attachProcess.context.none = input::ContextKeys{input::ContextKey::DialogFocus};
    attachProcess.enabled = [this](const input::ActionContext&) {
        return m_debug && !m_debug->debugging() &&
               (!m_managedDebugPreparation || !m_managedDebugPreparation->active()) && !m_managedSnapshotJob;
    };
    attachProcess.handler = [this](const input::ActionContext&, const input::InvokeInfo&) { showAttachToProcess(); };
    if (auto added = m_registry.add(std::move(attachProcess)); !added)
        log::warn(kLog, "attach process action: {}", added.error().describe());
    ui::RunActionHooks actionHooks;
    actionHooks.debugging = [this] {
        return (m_debugSessions && (m_debugSessions->anyDebugging() || m_debugSessions->anyRerunPending() ||
                                    m_debugSessions->hasPendingWork())) ||
               (m_managedDebugPreparation && m_managedDebugPreparation->active()) || m_managedSnapshotJob != nullptr;
    };
    actionHooks.stopDebugging = [this] {
        if ((m_managedDebugPreparation && m_managedDebugPreparation->active()) || m_managedSnapshotJob)
            invalidateManagedDebug();
        if (m_compoundDebug && m_compoundDebug->busy())
            m_compoundDebug->cancel();
        else if (m_debugSessions)
            m_debugSessions->stopSelected();
    };
    actionHooks.selectedProject = [this]() -> std::optional<project::ProjectId> {
        return m_explorer ? m_explorer->currentProject() : std::nullopt;
    };
    actionHooks.reportError = [this](const Error& error) {
        notify(text("Run"), qs(error.message()), ui::NotificationKind::Error);
    };
    if (auto registered = ui::registerRunActions(m_registry, *m_runController, std::move(actionHooks)); !registered) {
        log::warn(kLog, "run actions: {}", registered.error().describe());
    }
    // Rider shows the Run tool window when a run starts. The toolbar's Run, Debug and Stop follow
    // the run state and the selected configuration at once (not only on the window's timer).
    connect(m_runController.get(), &ui::RunController::runStateChanged, this, [this] {
        if (m_window && m_runController->running()) {
            m_window->toolWindows().show(ui::tool_windows::kRun, false);
        }
        if (m_window)
            m_window->refreshActions();
    });
    connect(m_runController.get(), &ui::RunController::configurationsChanged, this, [this] {
        if (m_window)
            m_window->refreshActions();
    });
}

bool Workbench::canStartLuaDebug() const {
    return !m_shutDown && !m_projectRoot.empty() && m_buildService && !m_buildService->projectEditInProgress() &&
           m_buildService->state() == build::BuildState::Idle &&
           (!m_cmake || (!m_cmake->running() && m_cmake->service().state() == build::BuildState::Idle)) &&
           m_runController && !m_runController->running() && m_runService && m_runService->activeRuns().empty() &&
           (!m_runManager || m_runManager->activeRuns().empty()) && m_debug && !m_debug->debugging() &&
           !m_debug->rerunPending() && (!m_managedDebugPreparation || !m_managedDebugPreparation->active()) &&
           !m_managedSnapshotJob;
}

void Workbench::attachRun() {
    // The Run tool window (Alt+4) is the shell's "run" window; its content is the Run tool window v2
    // (RUN-3: one tab per run).
    m_window->toolWindows().setContent(ui::tool_windows::kRun, m_runWindow);
    m_runController->setEditConfigurationsHandler([this] { editRunConfigurations(); });
    m_runController->bindStartupCombo(m_window->mainToolbar().runConfigurations());
}

void Workbench::editRunConfigurations() {
    if (!m_window || !m_runController)
        return;
    // Run/Debug Configurations (RUN-3) over RUN-1's model; it replaces the R0 dialog.
    const run::RunConfiguration* selected = m_runController->selectedConfiguration();
    ui::RunConfigurationsDialog dialog(m_runController->configurations(), selected ? selected->id : std::string(),
                                       hasProject(), m_window);
    std::string luaScript;
    if (auto* view = currentView(); view && io::fileName(view->document().path()).ends_with(".lua"))
        luaScript = view->document().path();
    dialog.setLuaDefaults(defaultLuaInterpreterPath(), std::move(luaScript), m_projectRoot);
    dialog.setImportHandler([this](const std::string& path,
                                   std::span<const run::RunConfiguration> current) -> Result<run::RunImportReport> {
        auto bytes = m_services.fs.readFileSnapshot(path, 4U * 1024U * 1024U);
        if (!bytes)
            return std::unexpected(bytes.error());
        return run::importVisualStudioConfigurations(*bytes, io::fileName(path), m_projectRoot, current);
    });
    ui::installDialogChrome(&dialog, true);
    if (dialog.exec() != QDialog::Accepted || !m_runController)
        return;
    if (auto saved = m_runController->replaceConfigurations(dialog.edited()); !saved)
        notify(text("Run/Debug Configurations"), qs(saved.error().message()), ui::NotificationKind::Error);
    if (const std::string id = dialog.selectedId(); !id.empty())
        (void)m_runController->selectConfiguration(id);
}

void Workbench::showAttachToProcess() {
    QWidget* parentWin = activeTopWindow();
    if (!parentWin)
        return;
    showAttachToProcessDialog(parentWin, [this](const RunningProcessInfo& proc, bool asDotNet) {
        attachToProcess(proc.pid, proc.executablePath, proc.name, asDotNet);
    });
}

void Workbench::attachToProcess(std::uint32_t pid, const std::string& exePath, const std::string& exeName,
                                bool attachAsDotNet) {
    if (!m_debug || m_debug->debugging()) {
        notify(text("Attach to Process"), text("A debug session is already active; stop it first."),
               ui::NotificationKind::Warning);
        return;
    }
    debug::DebugTarget target;
    target.name = std::format("{} (PID {})", exeName.empty() ? "Process" : exeName, pid);
    // The program is the process's own image path as the OS reported it; nothing is guessed. A
    // process whose path cannot be read (another user's, protected) cannot be attached to here.
    if (exePath.empty() || !io::isAbsolutePath(exePath)) {
        notify(text("Attach to Process"),
               text("Cannot attach to PID %1: its executable path is unavailable (the process may belong to another "
                    "user or be protected).")
                   .arg(pid),
               ui::NotificationKind::Warning);
        return;
    }
    target.program = exePath;
    target.workingDirectory = !m_projectRoot.empty() ? m_projectRoot : std::string(io::parentPath(target.program));
    /*
     * The folder that must be trusted: the open project, else the program's own folder. Trust is
     * only ever granted by the user through the attach prompt (TrustPurpose::DebugAttach, which
     * never cancels a build waiting for trust), never silently here. With a project open the prompt
     * trusts the project as usual. With none open, the program's folder may be a system folder such
     * as C:\Windows\System32: the prompt then only confirms this attach, through a session grant
     * (TrustStore::trustForSession) that lasts while the debugger's trust gate runs and ends right
     * after the session started or failed, so the folder is never saved or trusted for anything else.
     */
    const bool oneOff = m_projectRoot.empty();
    const std::string trustRoot = oneOff ? std::string(io::parentPath(target.program)) : m_projectRoot;
    target.trustRoots = {trustRoot};
    if (!m_services.trust.isTrusted(trustRoot)) {
        if (!m_window || !m_build)
            return;
        auto* dialog = new ui::TrustDialog(*m_build, {qs(trustRoot)}, m_window, ui::TrustPurpose::DebugAttach);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        if (oneOff) {
            dialog->setSessionGrant(
                [this](std::string_view root) -> Result<void> { return m_services.trust.trustForSession(root); });
            // Cancelled after a grant (a failed retry, then Cancel): the grant ends with the prompt.
            connect(dialog, &QDialog::rejected, this,
                    [this, trustRoot] { m_services.trust.endSessionTrust(trustRoot); });
        }
        connect(dialog, &QDialog::accepted, this, [this, pid, exePath, exeName, attachAsDotNet, trustRoot, oneOff] {
            // The session grant ends with this handler, whatever the attach does.
            const auto endGrant = qScopeGuard([this, &trustRoot, oneOff] {
                if (oneOff)
                    m_services.trust.endSessionTrust(trustRoot);
            });
            // Retry only when the user actually trusted (or, one-off, confirmed) the folder and the
            // project did not change meanwhile; a cancelled prompt attaches nothing.
            if (!m_shutDown && oneOff == m_projectRoot.empty() && m_services.trust.isTrusted(trustRoot))
                attachToProcess(pid, exePath, exeName, attachAsDotNet);
        });
        dialog->open();
        return;
    }

    debug::AdapterFactory factory;
    rpc::Json attachArgs;
    if (m_options.debugAdapterFactory) {
        factory = m_options.debugAdapterFactory;
        attachArgs = rpc::Json::object()
                         .set("name", target.name)
                         .set("type", attachAsDotNet ? "coreclr" : "lldb-dap")
                         .set("request", "attach")
                         .set("pid", static_cast<std::int64_t>(pid))
                         .set("processId", static_cast<std::int64_t>(pid));
    } else if (attachAsDotNet) {
        const QString bundle = QCoreApplication::applicationDirPath() + QStringLiteral("/FrostManagedDebug");
#ifdef _WIN32
        const QString netcoredbgPath = bundle + QStringLiteral("/adapter/netcoredbg.exe");
#else
        const QString netcoredbgPath = bundle + QStringLiteral("/adapter/netcoredbg");
#endif
        if (!QFileInfo::exists(netcoredbgPath)) {
            notify(text("Attach to Process (.NET)"),
                   text("CoreCLR debug adapter not found at %1").arg(QDir::toNativeSeparators(netcoredbgPath)),
                   ui::NotificationKind::Error);
            return;
        }
        factory = debug::coreClrAdapterFactory(m_services.launcher, netcoredbgPath.toStdString());
        attachArgs = rpc::Json::object()
                         .set("name", target.name)
                         .set("type", "coreclr")
                         .set("request", "attach")
                         .set("processId", static_cast<std::int64_t>(pid))
                         .set("justMyCode", false);
    } else {
        // Default native adapter resolution occurs in the controller at session start, capturing
        // immutable pinned LLDB thread-ID provenance. Custom factories above stay unverified.
        attachArgs = rpc::Json::object()
                         .set("name", target.name)
                         .set("type", "lldb-dap")
                         .set("request", "attach")
                         .set("pid", static_cast<std::int64_t>(pid));
        if (!exePath.empty() && io::isAbsolutePath(exePath)) {
            attachArgs.set("program", exePath);
        }
    }

    if (auto attached = m_debug->attachTarget(target, std::move(factory), std::move(attachArgs)); !attached) {
        notify(text("Attach to Process"), qs(attached.error().message()), ui::NotificationKind::Error);
        return;
    }
    if (m_window) {
        m_window->toolWindows().show(ui::kDebugToolWindowId, false);
        m_window->refreshActions();
    }
}

/*
 * Compound launches are unavailable until RUN-5 (before-launch steps, compound run and attach
 * configurations). The previous implementation granted trust silently and started `dotnet` by bare
 * name through Qt's process class, outside IProcessLauncher; nothing is launched here now, and the user is
 * told why. The dialogs keep their compound section, which reaches this refusal.
 */
void Workbench::launchCompoundConfiguration(const std::vector<project::ProjectId>& targets) {
    if (targets.empty())
        return;
    notify(text("Compound Run unavailable"),
           text("Running several targets together requires compound run configurations (RUN-5). Nothing was "
                "started."),
           ui::NotificationKind::Warning);
}

} // namespace frost::app
