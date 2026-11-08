// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): Run and Debug, the run service and its controller, attaching
// Run to the window, Lua debugging's availability, Attach to Process and the compound-launch
// refusal (split from workbench.cpp, FND-12a).

#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/debug_actions.hpp"
#include "frost/ui/find/find_feature.hpp"
#include "frost/ui/run_actions.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"
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
#include "project_properties_dialog.hpp"
#include "settings_dialog.hpp"
#include "unity_control.hpp"
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>

#include <algorithm>
#include <utility>

namespace frost::app {

namespace key = settings::key;
namespace wa = workbench_actions;

namespace {

constexpr std::string_view kLog = "workbench";

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

} // namespace

void Workbench::createRunService() {
    // The Run console exists before the window (the actions drive it); attachRun() docks it.
    auto* console = new ui::RunConsole();
    console->setObjectName(QStringLiteral("runConsole"));
    m_runConsole = console;
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
    hooks.alternateInput = [this](const QString& line) { m_cmake->input(line); };
    m_runController = std::make_unique<ui::RunController>(console, std::move(hooks));
    // RUN-1: run configurations start through the RunManager (the CMake target run does today; the
    // MSBuild startup project still runs through RunService until RUN-2 moves it).
    m_runManager = run::RunManager::create(m_services.launcher);
    m_runBuilder = build::makeRunBuilder(*m_buildService);
    const auto snapshot = m_services.settings.snapshot();
    build::RunServiceOptions options;
    options.stateFilePath = m_options.runStatePath;
    options.msbuildPath = !m_options.msbuildPath.empty() ? m_options.msbuildPath : snapshot->get(key::toolsMsbuildPath);
    options.externalConsoleByDefault = m_options.runExternalConsole;
    m_runService = build::RunService::create(m_services.fs, m_services.launcher, m_services.trust, *m_runBuilder,
                                             std::move(options), m_runController->callbacks());
    m_runController->setService(m_runService.get());
    ui::DebugControllerOptions debugOptions;
    debugOptions.trust = &m_services.trust;
    debugOptions.fs = &m_services.fs;
    debugOptions.launcher = &m_services.launcher;
    debugOptions.adapterFactory = m_options.debugAdapterFactory;
    debugOptions.lldbDapSetting = [this] { return m_services.settings.snapshot()->get(key::toolsLldbDapPath); };
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
    debugHooks.alternateAvailable = [this] { return m_cmake != nullptr; };
    debugHooks.mayStart = [this] {
        return !m_buildService->projectEditInProgress() && !m_runController->running() &&
               m_runService->activeRuns().empty() &&
               (!m_managedDebugPreparation || !m_managedDebugPreparation->active()) && !m_managedSnapshotJob;
    };
    debugHooks.startAlternate = [this](auto done) { return m_cmake->debug(std::move(done)); };
    debugHooks.stopAlternate = [this] {
        if (m_cmake)
            m_cmake->stop();
    };
    m_debug = std::make_unique<ui::DebugController>(m_breakpoints, std::move(debugOptions), std::move(debugHooks));
    connect(m_debug.get(), &ui::DebugController::stateChanged, this, [this] {
        if (m_managedDebugSnapshot && !m_debug->debugging())
            invalidateManagedDebug();
    });
    m_managedDebugPreparation =
        std::make_unique<ManagedDebugPreparation>(m_services.launcher, m_services.trust, systemClock());
    m_debug->setRunService(m_runService.get());
    if (auto registered = ui::registerDebugActions(m_registry, *m_debug); !registered)
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
        return (m_debug && (m_debug->debugging() || m_debug->rerunPending())) ||
               (m_managedDebugPreparation && m_managedDebugPreparation->active()) || m_managedSnapshotJob != nullptr;
    };
    actionHooks.stopDebugging = [this] {
        if ((m_managedDebugPreparation && m_managedDebugPreparation->active()) || m_managedSnapshotJob)
            invalidateManagedDebug();
        if (m_debug)
            m_debug->stop();
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
    // Rider shows the Run tool window when a run starts.
    connect(m_runController.get(), &ui::RunController::runStateChanged, this, [this] {
        if (m_window && m_runController->running()) {
            m_window->toolWindows().show(ui::tool_windows::kRun, false);
        }
    });
}

bool Workbench::canStartLuaDebug() const {
    return !m_shutDown && !m_projectRoot.empty() && m_buildService && !m_buildService->projectEditInProgress() &&
           m_buildService->state() == build::BuildState::Idle &&
           (!m_cmake || (!m_cmake->running() && m_cmake->service().state() == build::BuildState::Idle)) &&
           m_runController && !m_runController->running() && m_runService && m_runService->activeRuns().empty() &&
           m_debug && !m_debug->debugging() && !m_debug->rerunPending() &&
           (!m_managedDebugPreparation || !m_managedDebugPreparation->active()) && !m_managedSnapshotJob;
}

void Workbench::attachRun() {
    // The Run tool window (Alt+4) is the shell's "run" window; its content is the Run console.
    m_window->toolWindows().setContent(ui::tool_windows::kRun, m_runConsole);
    m_runController->setEditConfigurationsHandler([this] {
        if (!m_window || !m_solution || !m_runService)
            return;
        std::string projPath;
        if (const auto startupId = m_runService->startupProject();
            startupId && startupId->value() < m_solution->projects().size()) {
            projPath = m_solution->project(*startupId).path;
        }
        if (projPath.empty() && !m_solution->projects().empty()) {
            projPath = m_solution->projects().front().path;
        }
        std::string activeCfg = "Debug|x64";
        if (const auto idx = m_build->activeConfigurationIndex(); idx && *idx < m_build->configurations().size())
            activeCfg = m_build->configurations()[*idx].name();
        showEditRunConfigurationsDialog(
            m_window, m_services.fs, m_solution.get(), projPath, activeCfg, m_runService->useExternalConsole(),
            [this](bool ext) {
                if (m_runService)
                    (void)m_runService->setUseExternalConsole(ext);
            },
            [this] { refreshSolutionExplorerForFsChange(); },
            [this](const std::vector<project::ProjectId>& targets) { launchCompoundConfiguration(targets); });
    });
    m_runController->bindStartupCombo(m_window->mainToolbar().runConfigurations());
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
        auto lldbPath = debug::findLldbDap(
            m_services.fs, debug::defaultLldbDapSearch(m_services.settings.snapshot()->get(key::toolsLldbDapPath)));
        if (!lldbPath) {
            notify(text("Attach to Process (Native C++)"), qs(lldbPath.error().message()), ui::NotificationKind::Error);
            return;
        }
        factory = debug::lldbDapAdapterFactory(m_services.launcher, *lldbPath);
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
