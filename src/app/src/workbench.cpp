// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): construction, the window wiring (the navigation workbench,
// tool windows, toolbar, actions, Welcome screen, Run, the language controller), the dialogs, the
// teardown and the shutdown. The document side is in workbench_documents.cpp, projects and clangd
// in workbench_project.cpp, the language features' adapters in workbench_language.cpp.

#include "frost/app/workbench.hpp"

#include "frost/app/welcome_screen.hpp"
#include "frost/core/assert.hpp"
#include "frost/core/log.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/simulated_keyboard.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/lane.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/debug_actions.hpp"
#include "frost/ui/find/find_feature.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/run_actions.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/navigation_bar.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/terminal/terminal_tool_window.hpp"
#include "frost/ui/toolwindows/build_actions.hpp"
#include "frost/ui/toolwindows/build_output.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"
#include "frost/ui/tr.hpp"
#include "frost/ui/vcs/git_actions.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "managed_debug_preparation.hpp"
#include "project_edit_process.hpp"
#include "settings_dialog.hpp"
#include "unity_control.hpp"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>
#include <QStyleHints>
#include <QTimer>

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace frost::app {

namespace key = settings::key;
namespace wa = workbench_actions;

namespace {

constexpr std::string_view kLog = "workbench";

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

// Whether `event` types text (printable, no Ctrl, Alt or Meta): the language features' textTyped.
QString typedText(const QKeyEvent& event) {
    if ((event.modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) != 0) {
        return {};
    }
    const QString typed = event.text();
    if (typed.isEmpty() || !std::ranges::all_of(typed, [](QChar c) { return c.isPrint(); })) {
        return {};
    }
    return typed;
}

} // namespace

// --- DialogWorkbenchPrompts ---

std::string DialogWorkbenchPrompts::chooseOpenPath(QWidget* parent, OpenKind kind) {
    const QString chosen =
        kind == OpenKind::Folder
            ? QFileDialog::getExistingDirectory(parent, text("Open Folder"))
            : QFileDialog::getOpenFileName(
                  parent, text("Open"), QString(),
                  text("Projects and files (*.sln *.slnx CMakeLists.txt *);;Solutions (*.sln *.slnx);;"
                       "CMake project (CMakeLists.txt);;"
                       "All files (*)"));
    return QDir::toNativeSeparators(chosen).toStdString();
}

bool DialogWorkbenchPrompts::confirmDiscard(QWidget* parent, const QString& file, const QString& error) {
    const auto answer =
        QMessageBox::warning(parent, text("Cannot Save File"),
                             text("%1 could not be saved:\n%2\n\nClose it and lose the changes?").arg(file, error),
                             QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    return answer == QMessageBox::Discard;
}

bool DialogWorkbenchPrompts::confirmRerun(QWidget* parent, const QString& project) {
    const auto answer = QMessageBox::question(parent, text("Process Is Running"),
                                              text("%1 is running. Stop and rerun it?").arg(project),
                                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
    return answer == QMessageBox::Yes;
}

bool DialogWorkbenchPrompts::confirmProjectRecovery(QWidget* parent, const QString& project) {
    return QMessageBox::question(
               parent, text("Recover Project Edit"),
               text("Restore the project definitions recorded before the interrupted edit of %1?").arg(project),
               QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
}

// --- Construction ---

Workbench::Workbench(WorkbenchServices services, WorkbenchOptions options, QObject* parent)
    : QObject(parent), m_services(services), m_options(std::move(options)), m_shellSettings(services.settings),
      m_keymapSettings(services.settings), m_appearanceSettings(services.settings) {
    m_pool = std::make_unique<sched::ThreadPool>(
        sched::ThreadPoolOptions{.workerCount = m_options.workers != 0 ? m_options.workers : 2U});
    m_commandsRetirement = std::make_shared<lang::GeneratedCommandsRetirement>(*m_pool);

    // The keymap on the given layout, else the thread's active one (en-US when it cannot be read).
    input::LayoutTable layout = [&] {
        if (m_options.layout) {
            return *m_options.layout;
        }
        Result<input::LayoutTable> active = input::readActiveLayout();
        return active ? std::move(*active) : input::simulatedLayout(input::SimulatedLayoutId::EnUs);
    }();
    const auto interval = std::chrono::milliseconds(QGuiApplication::styleHints()->mouseDoubleClickInterval());
    Result<input::KeymapService> keymap =
        input::KeymapService::create(m_registry, m_keymapSettings, input::currentKeymapPlatform(), layout, interval);
    // create() fails only when the embedded keymap does not parse, which its tests rule out.
    FROST_ASSERT(keymap.has_value());
    m_keymap.emplace(std::move(*keymap));
    if (auto bindings = ui::buildShortcutBindings(); bindings) {
        if (auto gitBindings = ui::gitShortcutBindings(); gitBindings)
            bindings->insert(bindings->end(), gitBindings->begin(), gitBindings->end());
        m_keymap->setModuleDefaults(std::move(*bindings));
    }

    // Hot exit: this process's own journal (offerRecovery() scans for the earlier sessions' ones).
    if (!m_options.journalDirectory.empty()) {
        if (auto made = m_services.fs.createDirectories(m_options.journalDirectory); !made) {
            log::warn(kLog, "no hot-exit journal directory: {}", made.error().describe());
        }
        if (auto journal = storage::HotExitJournal::create(m_options.journalDirectory, m_services.clock); journal) {
            m_journal.emplace(std::move(*journal));
        } else {
            log::warn(kLog, "no hot-exit journal: {}", journal.error().describe());
        }
    }
    if (!m_options.historyPath.empty()) {
        if (auto history = storage::LocalHistory::open(m_options.historyPath, m_services.clock); history) {
            m_history.emplace(std::move(*history));
        } else {
            log::warn(kLog, "no local history: {}", history.error().describe());
        }
    }
    m_documents = std::make_unique<document::SaveManager>(
        document::SaveManagerServices{&m_services.fs, &m_services.clock, m_journal ? &*m_journal : nullptr,
                                      m_history ? &*m_history : nullptr, &m_services.settings, nullptr});
    m_documents->applySettings(*m_services.settings.snapshot());
    m_autosave = std::make_unique<document::AutosaveController>(*m_documents, m_services.clock);
    if (!m_options.sessionDirectory.empty()) {
        m_sessions = std::make_unique<ui::nav::SessionStore>(m_services.fs, m_options.sessionDirectory);
    }
    if (m_options.watchFiles) {
        // One I/O reactor for the watcher and clangd's pipes (frost/io/reactor.hpp's exit order).
        if (Result<std::unique_ptr<io::Reactor>> reactor = io::Reactor::create(io::ReactorOptions{1, "frost-io"});
            reactor) {
            m_reactor = std::move(*reactor);
            if (auto watcher = io::RealFileWatcher::create(*m_reactor, *m_pool); watcher) {
                m_watcher = std::move(*watcher);
            } else {
                log::warn(kLog, "no file watcher: {}", watcher.error().describe());
            }
        } else {
            log::warn(kLog, "no file watcher: {}", reactor.error().describe());
        }
    }

    // The one document registry: the canonical path and the banner views of every tab.
    m_documentRegistry = std::make_unique<ui::nav::DocumentRegistry>(*m_documents, m_services.fs);
    m_registryListener = m_documentRegistry->subscribe([this](const std::string& path) { documentChanged(path); });
    m_editors = std::make_unique<DocumentEditorFactory>(*m_documents, static_cast<IDocumentEditorListener&>(*this));
    m_editors->setSettings(&m_services.settings, &m_services.fs);
    ui::EditorServices editorServices;
    editorServices.languages = &m_languages;
    editorServices.scheduler = m_pool.get();
    editorServices.clock = &m_services.clock;
    editorServices.actionHost = &m_actionHost;
    m_editors->setServices(editorServices);
    m_tabs = std::make_unique<ui::DocumentEditorFactory>(*m_editors, *m_documentRegistry);

    createBuildService();
    createRunService();
    registerActions();

    // The language features: one controller for the window, over whichever clangd runs.
    m_bridge = std::make_unique<LanguageBridge>(*this);
    m_language = std::make_unique<ui::lang::LanguageController>(
        ui::lang::LanguageServices{*m_bridge, *m_pool, *m_bridge, m_bridge->applier(), m_services.fs,
                                   [this](std::string_view path) -> std::vector<lang::LiveDiagnostic> {
                                       std::optional<lang::FileDiagnostics> set;
                                       if (isManagedSource(path) && m_managed)
                                           set = m_managed->diagnostics().forFile(path);
                                       else if (isLuaSource(path) && m_lua)
                                           set = m_lua->diagnostics().forFile(path);
                                       else if (isCFamilySource(path) && m_clangd)
                                           set = m_clangd->diagnostics().forFile(path);
                                       return set ? set->diagnostics : std::vector<lang::LiveDiagnostic>{};
                                   }});
    if (auto registered = m_language->registerActions(m_registry); !registered) {
        log::warn(kLog, "language actions: {}", registered.error().describe());
    }

    m_luaTrustWatch.setInterval(1000);
    connect(&m_luaTrustWatch, &QTimer::timeout, this, [this] {
        if (m_shutDown || m_projectRoot.empty())
            return;
        const bool trusted = m_services.trust.isTrusted(m_projectRoot);
        if (trusted != m_luaObservedTrust) {
            m_luaObservedTrust = trusted;
            if (trusted) {
                startLua(m_projectRoot);
                startManaged(m_projectRoot);
            } else {
                invalidateManagedDebug();
                stopLua();
                stopManaged();
            }
        } else if (trusted) {
            checkUnityProjectContext();
        }
    });
    m_luaTrustWatch.start();

    // Settings changes may be published on any thread; they are applied on this one.
    m_settingsSubscription = m_services.settings.subscribe([this](const settings::SettingsChange&) {
        QMetaObject::invokeMethod(this, [this] { onSettingsChanged(); }, Qt::QueuedConnection);
    });
}

Workbench::~Workbench() {
    shutdown();
    m_projectEdit.reset();
    m_settingsSubscription.reset();
    detach();
    for (auto& stopped : m_retiredClangd)
        if (stopped.valid())
            stopped.join();
    m_retiredClangd.clear();
    m_language.reset();
    // The prompter and the actions refer to the controller; the services' threads post to them.
    delete m_trustPrompter;
    m_trustPrompter = nullptr;
    delete m_buildActions;
    m_buildActions = nullptr;
    if (m_runController) {
        m_runController->setService(nullptr);
    }
    m_debug.reset();
    m_managedDebugPreparation.reset();
    m_runService.reset(); // Stops every run and joins its thread before the builder goes.
    m_runBuilder.reset();
    if (m_runConsole != nullptr && m_runConsole->parent() == nullptr) {
        delete m_runConsole.data(); // Never docked (no window was attached).
    }
    m_runController.reset();
    if (m_build) {
        m_build->setSaveAllHandler(nullptr);
        m_build->setBackend(nullptr);
    }
    m_buildService.reset(); // Joins its thread before the backend that forwards to it goes.
    m_buildBackend.reset();
    m_build.reset();
    if (m_documentRegistry) {
        m_documentRegistry->unsubscribe(m_registryListener);
    }
    for (auto& stopped : m_retiredLua)
        if (stopped.valid())
            stopped.join();
    m_retiredLua.clear();
    for (auto& stopped : m_retiredManaged)
        if (stopped.valid())
            stopped.join();
    m_retiredManaged.clear();
    // frost/io/reactor.hpp's exit order: shut the reactor down, drop the watcher, shut the pool
    // down, then destroy the reactor.
    if (m_reactor) {
        m_reactor->shutdown();
    }
    m_watcher.reset();
    m_commandsRetirement->close();
    if (m_pool) {
        m_pool->shutdown();
    }
    m_bridge.reset(); // In-flight insight requests use the bridge until the pool has stopped.
    m_reactor.reset();
}

void Workbench::createBuildService() {
    m_build = std::make_unique<ui::BuildController>();
    const auto snapshot = m_services.settings.snapshot();
    build::BuildServiceOptions options;
    options.stateFilePath = m_options.buildStatePath;
    options.msbuildPath = !m_options.msbuildPath.empty() ? m_options.msbuildPath : snapshot->get(key::toolsMsbuildPath);
    options.maxCpuCount = static_cast<unsigned>(std::max<std::int64_t>(0, snapshot->get(key::buildMsbuildMaxCpuCount)));
    options.startBuild = m_options.startBuild;
    m_buildService = build::BuildService::create(m_services.fs, m_services.launcher, m_services.trust, options,
                                                 m_build->serviceCallbacks([this] { return !m_cmake; }));
    m_buildBackend = std::make_unique<ui::BuildServiceBackend>(*m_buildService);
    m_build->setBackend(m_buildBackend.get());
    m_build->setSaveAllHandler(this);
    m_buildActions = new ui::BuildActions(*m_build, this);
    if (auto registered = m_buildActions->registerActions(m_registry); !registered) {
        log::warn(kLog, "build actions: {}", registered.error().describe());
    }
    // A new configuration builds other files with other flags: clangd follows (ADR 0041).
    connect(m_build.get(), &ui::BuildController::activeConfigurationChanged, this, [this](int) {
        if (m_solution && m_clangd) {
            restartClangd();
        }
    });
}

build::CMakeService* Workbench::cmakeService() noexcept { return m_cmake ? &m_cmake->service() : nullptr; }
bool Workbench::cmakeTargetsCurrent() const { return m_cmake && m_cmake->targetsCurrent(); }

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

void Workbench::registerActions() {
    // The shell registers editor.gotoLine itself (it opens its Go to Line popup).
    static constexpr std::array<std::string_view, 1> kShellOwned{ui::actions::kGoToLine};
    if (auto editor = ui::registerEditorActions(m_registry, m_actionHost, kShellOwned); !editor) {
        log::warn(kLog, "editor actions: {}", editor.error().describe());
    }
    const auto add = [this](std::string_view id, std::string name, std::function<void()> run,
                            std::function<bool()> enabled = {}) {
        input::ActionSpec spec;
        spec.id = std::string(id);
        spec.name = std::move(name);
        spec.group = "File";
        spec.context.none = input::ContextKeys{input::ContextKey::DialogFocus};
        spec.handler = [run = std::move(run)](const input::ActionContext&, const input::InvokeInfo&) { run(); };
        if (enabled) {
            spec.enabled = [enabled = std::move(enabled)](const input::ActionContext&) { return enabled(); };
        }
        if (auto added = m_registry.add(std::move(spec)); !added) {
            log::warn(kLog, "action {}: {}", id, added.error().describe());
        }
    };
    // Save All, Save, Reload All from Disk and Close Project are the navigation workbench's.
    add(wa::kOpenFolder, "Open Folder...", [this] { showOpenDialog(IWorkbenchPrompts::OpenKind::Folder); });
    add("file.settings", "Settings...", [this] { showSettings(); }, [this] { return m_window != nullptr; });
    add("window.structure", "Structure", [this] {
        if (m_window)
            m_window->toolWindows().activate("structure");
    });
    add(
        wa::kOpenRecent, "Open Recent...", [this] { showOpenRecent(); },
        [this] { return !recentProjects(*m_services.settings.snapshot()).empty(); });
}

Result<ui::InstallReport> Workbench::installAppearance(QApplication& app, std::string resourceDir) {
    if (m_appearance != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the appearance is already installed");
    }
    m_os = std::make_unique<ui::QtOsAppearanceSource>();
    auto manager = std::make_unique<ui::AppearanceManager>(
        ui::AppearanceEnvironment{m_services.fs, std::move(resourceDir), m_appearanceSettings, m_os.get()});
    Result<ui::InstallReport> report = manager->install(app);
    if (report) {
        m_appearance = std::move(manager);
        ui::EditorServices editorServices;
        editorServices.languages = &m_languages;
        editorServices.scheduler = m_pool.get();
        editorServices.clock = &m_services.clock;
        editorServices.actionHost = &m_actionHost;
        editorServices.appearance = m_appearance.get();
        m_editors->setServices(editorServices);
    }
    return report;
}

ui::ShellEnvironment Workbench::environment() {
    ui::ShellEnvironment environment{m_registry, *m_keymap, m_shellSettings, *m_tabs};
    environment.appearance = m_appearance.get();
    environment.appearanceSettings = m_appearance != nullptr ? &m_appearanceSettings : nullptr;
    environment.followActiveLayout = m_options.followActiveLayout;
    return environment;
}

Result<void> Workbench::announceUnity(const lang::Json& message) {
    if (m_shutDown || !m_unity)
        return fail(ErrorCode::Cancelled, "Unity controls unavailable");
    return m_unity->announce(message);
}

// --- The window ---

void Workbench::attach(ui::ShellWindow& window) {
    FROST_PRE(m_window == nullptr);
    m_window = &window;
    // A workbench destroyed first has detached already (the guard drops the call).
    window.addTeardownHook([self = QPointer<Workbench>(this)] {
        if (self) {
            self->detach();
        }
    });
    window.setOpenFileHandler([this] { showOpenDialog(IWorkbenchPrompts::OpenKind::SolutionOrFile); });

    // The navigation workbench owns the documents' UI: open, save, sessions, hot exit, disk changes.
    ui::WorkbenchServices nav{m_services.fs, *m_documents, *m_documentRegistry, *m_tabs, m_services.clock};
    nav.scheduler = m_pool.get();
    nav.watcher = m_watcher.get();
    nav.sessions = m_sessions.get();
    nav.autosave = m_autosave.get();
    nav.applicationSettingsFile = m_options.applicationSettingsFile;
    nav.closeProjectHook = [this] { static_cast<void>(closeProject()); };
    nav.openProjectHook = [this](const std::string& root) { return reopenRoot(root); };
    nav.filesChanged = [this](const io::FileChangeBatch& batch) {
        if (m_cmake)
            m_cmake->filesChanged(batch);
        if (m_git)
            m_git->filesChanged(batch);
    };
    m_nav = std::make_unique<ui::Workbench>(window, std::move(nav));

    attachToolWindows();
    attachNavigationBar();
    m_engine = std::make_unique<EngineIntegration>(*this);
    m_unity = std::make_unique<UnityControlIntegration>(*this);
    attachToolbar();
    attachRun();
    if (auto registered = ui::registerDebugToolWindow(m_window->toolWindows(), m_registry, *m_debug); !registered)
        log::warn(kLog, "debug tool window: {}", registered.error().describe());
    attachLanguage();
    connect(&window.tabs(), &ui::EditorTabs::currentEditorChanged, this, [this](ui::IEditorView*) {
        m_language->editorChanged();
        updateLanguageTarget();
    });
    attachTerminal();
    attachFind();
    if (m_options.git)
        m_git = std::make_unique<GitIntegration>(*this);
    m_trustPrompter = new ui::TrustPrompter(*m_build, &window, this);

    auto* welcome = new WelcomeScreen();
    m_welcome = welcome;
    window.tabs().setEmptyStateWidget(welcome);
    connect(welcome, &WelcomeScreen::openRequested, this,
            [this] { showOpenDialog(IWorkbenchPrompts::OpenKind::SolutionOrFile); });
    connect(welcome, &WelcomeScreen::openFolderRequested, this,
            [this] { showOpenDialog(IWorkbenchPrompts::OpenKind::Folder); });
    connect(welcome, &WelcomeScreen::openRecent, this, [this](const QString& path) {
        // Queued: opening replaces the Welcome screen whose click is being handled.
        QMetaObject::invokeMethod(
            this, [this, path] { (void)openProjectIdentity(path.toStdString()); }, Qt::QueuedConnection);
    });
    connect(welcome, &WelcomeScreen::removeRecent, this, [this](const QString& path) {
        if (auto removed = removeRecentProject(m_services.settings, path.toStdString()); !removed) {
            notify(text("Cannot update the recent projects"), qs(removed.error().message()),
                   ui::NotificationKind::Warning);
        }
        refreshRecent();
    });
    refreshRecent();

    // Build: auto-show rules (ADR 0044) and gutter markers.
    connect(m_build.get(), &ui::BuildController::buildStarted, this, [this] {
        if (m_window) {
            m_window->toolWindows().show(kBuildToolWindow, false);
        }
    });
    connect(m_build.get(), &ui::BuildController::buildFinished, this, [this](const build::BuildOutcome& outcome) {
        if (m_window && outcome.counts.errors > 0) {
            m_window->toolWindows().show(ui::tool_windows::kProblems, false);
        }
        refreshAllMarkers();
    });
    connect(m_build.get(), &ui::BuildController::diagnosticAdded, this, [this](const build::BuildDiagnostic& d) {
        if (!d.path.empty()) {
            for (DocumentEditorView* view : m_editors->views())
                if (lang::documentKey(view->path()) == lang::documentKey(d.path))
                    refreshMarkers(*view);
        }
    });
    connect(m_build.get(), &ui::BuildController::diagnosticsCleared, this, [this] { refreshAllMarkers(); });
    connect(m_build.get(), &ui::BuildController::requestFailed, this,
            [this](const QString& message) { notify(text("Build"), message, ui::NotificationKind::Error); });

    // The language features see keys before the shortcut dispatcher (installed with the shell, so
    // this later filter runs first).
    QCoreApplication::instance()->installEventFilter(this);
    window.refreshActions();
}

void Workbench::detach() {
    if (!m_window) {
        return;
    }
    delete m_settingsDialog.data();
    m_settingsDialog = nullptr;
    m_window->mainToolbar().setProjectMenuBuilder({});
    QCoreApplication::instance()->removeEventFilter(this);
    cancelFind();
    m_find.reset();
    m_git.reset();
    if (m_debug) {
        m_debug->shutdown();
        m_window->toolWindows().hide(ui::kDebugToolWindowId);
        m_window->toolWindows().setContent(ui::kDebugToolWindowId, new QWidget());
    }
    m_unity.reset();
    m_engine.reset();
    stopTerminal();
    m_terminal.reset();
    // The navigation workbench saves the session and stops its watcher while the tabs exist.
    disconnect(this, &Workbench::projectChanged, &m_window->navigationBar(), nullptr);
    m_window->navigationBar().clearModel();
    m_nav.reset();
    // Navigation saves tab state by directory; retain the exact project the user opened.
    if (m_sessions && !m_projectIdentity.empty()) {
        if (auto saved = m_sessions->setLastProject(m_projectIdentity); !saved)
            log::warn(kLog, "last project: {}", saved.error().describe());
    }
    if (m_cmake) {
        stopClangd();
        m_build->setBackend(m_buildBackend.get());
        m_cmake.reset();
    }
    // The controller owns its panel; the tool window manager deletes what it still holds.
    if (m_language) {
        ui::lang::FindUsagesPanel& panel = m_language->usagesPanel();
        panel.hide();
        panel.setParent(nullptr);
        auto& structure = m_language->structurePanel();
        structure.hide();
        structure.setParent(nullptr);
    }
    if (m_runController) {
        m_runController->bindStartupCombo(nullptr);
    }
    // The Run console outlives the window with its controller (~Workbench deletes it).
    if (m_runConsole != nullptr) {
        m_runConsole->hide();
        m_runConsole->setParent(nullptr);
    }
    m_window = nullptr;
}

void Workbench::attachToolWindows() {
    ui::ToolWindowManager& windows = m_window->toolWindows();

    auto* explorer = new ui::SolutionExplorer();
    m_projectEdit = std::make_unique<ProjectEditProcess>(m_services.launcher, m_services.clock);
    explorer->setController(m_build.get());
    explorer->setProcessLauncher(&m_services.launcher);
    explorer->setRemoveProjectFileHandler(
        [this](const QString& project, const QString& source) { removeProjectFile(project, source); });
    explorer->setAddProjectFileHandler([this](const QString& project) {
        const auto source = m_options.selectExistingProjectSource
                                ? m_options.selectExistingProjectSource(project)
                                : QFileDialog::getOpenFileName(m_window, QStringLiteral("Add Existing C/C++ Source"),
                                                               QFileInfo(project).absolutePath(),
                                                               QStringLiteral("C/C++ files (*.c *.cpp *.h *.hpp)"));
        if (!source.isEmpty())
            removeProjectFile(project, source, true);
    });
    explorer->setRecoverProjectHandler(
        [this](const QString&) {
            const auto journal = io::joinPath(m_projectRoot, ".frost-project-edit-journal");
            if (!journal)
                return false;
            const auto exists = m_services.fs.exists(*journal);
            return exists && *exists;
        },
        [this](const QString& project) { removeProjectFile(project, QString{}); });
    explorer->setStartupProjectHandler([this](project::ProjectId project) {
        if (auto set = m_runController->setStartupProject(project); !set) {
            notify(text("Set as Startup Project"), qs(set.error().message()), ui::NotificationKind::Error);
        }
    });
    windows.setContent(ui::tool_windows::kProject, explorer);
    m_explorer = explorer;
    connect(explorer, &ui::SolutionExplorer::openFile, this, [this](const QString& path) { openLocation(path, 0, 0); });
    connect(explorer, &ui::SolutionExplorer::commandFailed, this, [this](const QString& message) {
        notify(text("Solution Explorer"), message, ui::NotificationKind::Error);
    });

    auto* problems = new ui::ProblemsView();
    problems->setController(m_build.get());
    windows.setContent(ui::tool_windows::kProblems, problems);
    m_problems = problems;
    connect(problems, &ui::ProblemsView::openLocation, this, &Workbench::openLocation);

    auto* output = new ui::BuildOutputPane();
    output->setController(m_build.get());
    ui::ToolWindowSpec spec;
    spec.id = std::string(kBuildToolWindow);
    spec.title = text("Build");
    spec.side = ui::ToolWindowSide::Bottom;
    spec.iconId = std::string(ui::icons::kBuildBuild);
    if (auto added = windows.add(std::move(spec)); added) {
        windows.setContent(kBuildToolWindow, output);
    } else {
        log::warn(kLog, "build tool window: {}", added.error().describe());
        output->setParent(m_window);
        output->hide();
    }
    m_output = output;
    connect(output, &ui::BuildOutputPane::openLocation, this, &Workbench::openLocation);
}

void Workbench::attachToolbar() {
    ui::MainToolbar& toolbar = m_window->mainToolbar();
    toolbar.setProjectMenuBuilder([this](QMenu& menu) {
        auto* open = menu.addAction(text("Open..."));
        connect(open, &QAction::triggered, this, [this] {
            QTimer::singleShot(0, this, [this] {
                if (m_window && m_nav) {
                    const std::string chosen =
                        m_services.prompts.chooseOpenPath(m_window, IWorkbenchPrompts::OpenKind::SolutionOrFile);
                    if (!chosen.empty())
                        (void)openProjectIdentity(chosen);
                }
            });
        });
        auto* folder = menu.addAction(text("Open Folder..."));
        connect(folder, &QAction::triggered, this, [this] {
            QTimer::singleShot(0, this, [this] {
                if (m_window && m_nav)
                    showOpenDialog(IWorkbenchPrompts::OpenKind::Folder);
            });
        });
        const auto recent = recentProjects(*m_services.settings.snapshot());
        if (!recent.empty()) {
            menu.addSeparator();
        }
        for (const std::string& path : recent) {
            const QString native = QDir::toNativeSeparators(qs(path));
            const int maxWidth = menu.fontMetrics().horizontalAdvance(QString(64, QLatin1Char('n')));
            QString label = menu.fontMetrics().elidedText(native, Qt::ElideMiddle, maxWidth);
            label.replace(QLatin1Char('&'), QStringLiteral("&&"));
            QAction* action = menu.addAction(label);
            action->setData(qs(path));
            action->setToolTip(native);
            action->setCheckable(true);
            action->setChecked(path == m_projectIdentity);
            connect(action, &QAction::triggered, this, [this, path] {
                QTimer::singleShot(0, this, [this, path] {
                    if (m_window && m_nav)
                        (void)openProjectIdentity(path);
                });
            });
        }
    });
    toolbar.setSlotAction(ui::MainToolbar::kBuild, ui::build_actions::kBuildSolution);
    toolbar.setSlotAction(ui::MainToolbar::kDebug, "run.debug");
    auto* combo = new ui::ConfigurationCombo(*m_build, &toolbar);
    combo->setObjectName(QStringLiteral("solutionConfigurations"));
    QAction* before = nullptr;
    for (QAction* action : toolbar.actions()) {
        if (toolbar.widgetForAction(action) == toolbar.runConfigurations()) {
            before = action;
            break;
        }
    }
    toolbar.insertWidget(before, combo);
    m_configurations = combo;
}

void Workbench::attachRun() {
    // The Run tool window (Alt+4) is the shell's "run" window; its content is the Run console.
    m_window->toolWindows().setContent(ui::tool_windows::kRun, m_runConsole);
    m_runController->bindStartupCombo(m_window->mainToolbar().runConfigurations());
}

void Workbench::attachLanguage() {
    auto& windows = m_window->toolWindows();
    connect(&windows, &ui::ToolWindowManager::visibilityChanged, this, [this](const QString& id, bool visible) {
        if (id == QStringLiteral("structure"))
            m_language->setStructureVisible(visible);
    });
    ui::ToolWindowSpec structure;
    structure.id = "structure";
    structure.title = text("Structure");
    structure.side = ui::ToolWindowSide::Left;
    structure.iconId = std::string(ui::icons::kToolwindowStructure);
    const QPointer<QWidget> structureContent(&m_language->structurePanel());
    structure.createContent = [structureContent]() -> QWidget* { return structureContent.data(); };
    if (auto added = windows.add(std::move(structure)); !added)
        log::warn(kLog, "structure tool window: {}", added.error().describe());
    m_language->setStructureVisible(windows.isVisible("structure"));
    ui::lang::FindUsagesPanel* panel = &m_language->usagesPanel();
    ui::ToolWindowSpec spec;
    spec.id = std::string(kFindUsagesToolWindow);
    spec.title = text("Find Usages");
    spec.side = ui::ToolWindowSide::Bottom;
    spec.iconId = std::string(ui::icons::kActionsSearch);
    const QPointer<QWidget> content(panel);
    spec.createContent = [content]() -> QWidget* { return content.data(); };
    if (auto added = m_window->toolWindows().add(std::move(spec)); !added) {
        log::warn(kLog, "find usages tool window: {}", added.error().describe());
    }
    connect(m_language.get(), &ui::lang::LanguageController::usagesReady, this, [this] {
        if (m_window) {
            m_window->toolWindows().show(kFindUsagesToolWindow, false);
        }
    });
    connect(m_language.get(), &ui::lang::LanguageController::statusMessage, this, [this](const QString& message) {
        if (m_window) {
            m_window->statusBar()->showMessage(message, 5000);
        }
    });
}

void Workbench::notify(const QString& title, const QString& body, ui::NotificationKind kind) {
    if (m_window) {
        m_window->notifications().notify({title, body, kind, std::nullopt, {}});
    } else {
        log::warn(kLog, "{}: {}", title.toStdString(), body.toStdString());
    }
}

void Workbench::onSettingsChanged() {
    const auto snapshot = m_services.settings.snapshot();
    m_documents->applySettings(*snapshot);
    const auto luaPath = snapshot->get(settings::key::toolsLuaLanguageServerPath);
    const auto luaRuntime = snapshot->get(settings::key::luaRuntimeVersion);
    if (!m_projectRoot.empty() && (luaPath != m_luaConfiguredPath || luaRuntime != m_luaRuntimeVersion))
        startLua(m_projectRoot);
    if (!m_projectRoot.empty() && (snapshot->get(settings::key::toolsManagedDotnetPath) != m_managedDotnetPath ||
                                   snapshot->get(settings::key::toolsManagedServerPath) != m_managedServerPath))
        startManaged(m_projectRoot);
    m_appearanceSettings.storeChanged();
    refreshRecent();
}

bool Workbench::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::KeyPress || m_language == nullptr) {
        return QObject::eventFilter(watched, event);
    }
    DocumentEditorView* view = currentView();
    if (view == nullptr || watched != static_cast<QObject*>(&view->editor())) {
        return QObject::eventFilter(watched, event);
    }
    auto* key = static_cast<QKeyEvent*>(event);
    // An open lookup, parameter info or snippet takes its keys first (Up, Down, Enter, Tab, Esc).
    if (m_language->keyPress(key->key(), key->modifiers())) {
        return true;
    }
    if (const QString typed = typedText(*key); !typed.isEmpty()) {
        // editorTextChanged passes it on once the editor has inserted it (this key press only).
        m_typed = typed.toStdString();
        QTimer::singleShot(0, this, [this] { m_typed.clear(); });
    }
    return QObject::eventFilter(watched, event);
}

void Workbench::showOpenDialog(IWorkbenchPrompts::OpenKind kind) {
    const std::string chosen = m_services.prompts.chooseOpenPath(m_window, kind);
    if (!chosen.empty()) {
        // At the welcome screen a CMakeLists.txt is a project entry point. Once a project is
        // open, File > Open continues to edit CMake files, including nested lists.
        if (kind == IWorkbenchPrompts::OpenKind::SolutionOrFile && !hasProject() &&
            io::fileName(chosen) == "CMakeLists.txt") {
            (void)openCMakeProject(chosen);
            return;
        }
        (void)openPath(chosen);
    }
}

void Workbench::showOpenRecent() {
    if (!m_window) {
        return;
    }
    auto* popup = new ui::ListPopup(m_window, text("Open Recent"), true);
    popup->setObjectName(QStringLiteral("openRecentPopup"));
    std::vector<ui::ListPopupItem> items;
    for (const std::string& path : recentProjects(*m_services.settings.snapshot())) {
        const QString native = QDir::toNativeSeparators(qs(path));
        items.push_back(ui::ListPopupItem{QFileInfo(native).fileName(), native, QIcon(), qs(path)});
    }
    popup->setItems(std::move(items));
    connect(popup, &ui::ListPopup::itemAccepted, this, [this](int, const QVariant& data) {
        const std::string path = data.toString().toStdString();
        QMetaObject::invokeMethod(this, [this, path] { (void)openProjectIdentity(path); }, Qt::QueuedConnection);
    });
    popup->showCentered(m_window, 0.2);
}

void Workbench::showSettings() {
    if (!m_window)
        return;
    if (m_settingsDialog) {
        m_settingsDialog->raise();
        m_settingsDialog->activateWindow();
        return;
    }
    auto* dialog = new SettingsDialog(m_services.settings, m_window);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_settingsDialog = dialog;
    dialog->open();
}

void Workbench::refreshRecent() {
    if (m_welcome) {
        m_welcome->setRecentProjects(recentProjects(*m_services.settings.snapshot()));
    }
}

void Workbench::shutdown() {
    if (m_shutDown) {
        return;
    }
    m_shutDown = true;
    if (m_projectEdit)
        m_projectEdit->stop();
    if (m_language)
        m_language->setStructureVisible(false);
    invalidateManagedDebug();
    m_luaTrustWatch.stop();
    cancelFind();
    // Registered shell callbacks hold QPointers: destroying the feature makes queued actions inert.
    m_find.reset();
    m_git.reset();
    if (m_debug)
        m_debug->shutdown();
    if (m_engine)
        m_engine->shutdown();
    if (m_unity)
        m_unity->shutdown();
    stopTerminal();
    if (m_cmake)
        m_cmake->shutdown();
    if (m_nav) {
        if (auto saved = m_nav->saveSession(); !saved) {
            log::warn(kLog, "session: {}", saved.error().describe());
        }
    }
    if (m_sessions && !m_projectIdentity.empty()) {
        if (auto saved = m_sessions->setLastProject(m_projectIdentity); !saved)
            log::warn(kLog, "last project: {}", saved.error().describe());
    }
    if (m_runService) {
        m_runService->stopAll();
    }
    // Rider saves on exit; what cannot be saved stays in the hot-exit journal.
    if (m_documents->policies().autosave.onFrameDeactivation) {
        const document::SaveAllResult saved = m_documents->saveAll(document::SaveReason::Autosave);
        for (document::FileDocument* document : saved.saved) {
            m_documentRegistry->notifyChanged(document->path());
        }
    }
    if (auto closed = m_documents->closeJournal(); !closed) {
        log::warn(kLog, "hot-exit journal: {}", closed.error().describe());
    }
    stopLua();
    stopManaged();
    stopClangd();
}

} // namespace frost::app
