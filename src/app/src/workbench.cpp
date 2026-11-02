// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): construction, the window wiring (tool windows, toolbar,
// actions, Welcome screen, autosave and activation hooks), the dialogs and the shutdown. The
// document side is in workbench_documents.cpp, projects and clangd in workbench_project.cpp.

#include "frost/app/workbench.hpp"

#include "frost/app/welcome_screen.hpp"
#include "frost/core/assert.hpp"
#include "frost/core/log.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/simulated_keyboard.hpp"
#include "frost/io/path.hpp"
#include "frost/sched/lane.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/main_toolbar.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_actions.hpp"
#include "frost/ui/toolwindows/build_output.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"
#include "frost/ui/tr.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMessageBox>
#include <QStyleHints>

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

} // namespace

// --- DialogWorkbenchPrompts ---

std::string DialogWorkbenchPrompts::chooseOpenPath(QWidget* parent, OpenKind kind) {
    const QString chosen =
        kind == OpenKind::Folder
            ? QFileDialog::getExistingDirectory(parent, text("Open Folder"))
            : QFileDialog::getOpenFileName(parent, text("Open"), QString(),
                                           text("Solutions and files (*.sln *.slnx *);;Solutions (*.sln *.slnx);;"
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

bool DialogWorkbenchPrompts::confirmRestore(QWidget* parent, int count) {
    const auto answer = QMessageBox::question(
        parent, text("Restore Unsaved Files"),
        text("FrostIDE found %1 unsaved file(s) from an earlier session. Restore them?\n\nNo discards them.")
            .arg(count),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    return answer == QMessageBox::Yes;
}

// --- Construction ---

Workbench::Workbench(WorkbenchServices services, WorkbenchOptions options, QObject* parent)
    : QObject(parent), m_services(services), m_options(std::move(options)), m_shellSettings(services.settings),
      m_keymapSettings(services.settings), m_appearanceSettings(services.settings) {
    m_pool = std::make_unique<sched::ThreadPool>(
        sched::ThreadPoolOptions{.workerCount = m_options.workers != 0 ? m_options.workers : 2U});

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
    m_autosaveTimer.setSingleShot(true);
    connect(&m_autosaveTimer, &QTimer::timeout, this, [this] { pollAutosave(); });

    m_editors = std::make_unique<DocumentEditorFactory>(*m_documents, static_cast<IDocumentEditorListener&>(*this));
    m_editors->setSettings(&m_services.settings, &m_services.fs);
    ui::EditorServices editorServices;
    editorServices.languages = &m_languages;
    editorServices.scheduler = m_pool.get();
    editorServices.clock = &m_services.clock;
    editorServices.actionHost = &m_actionHost;
    m_editors->setServices(editorServices);

    createBuildService();
    registerActions();

    // Settings changes may be published on any thread; they are applied on this one.
    m_settingsSubscription = m_services.settings.subscribe([this](const settings::SettingsChange&) {
        QMetaObject::invokeMethod(this, [this] { onSettingsChanged(); }, Qt::QueuedConnection);
    });
}

Workbench::~Workbench() {
    shutdown();
    m_settingsSubscription.reset();
    // The prompter and the actions refer to the controller; the service's thread posts to it.
    delete m_trustPrompter;
    m_trustPrompter = nullptr;
    delete m_buildActions;
    m_buildActions = nullptr;
    if (m_build) {
        m_build->setSaveAllHandler(nullptr);
        m_build->setBackend(nullptr);
    }
    m_buildService.reset(); // Joins its thread before the backend that forwards to it goes.
    m_buildBackend.reset();
    m_build.reset();
    if (m_reactor) {
        m_reactor->shutdown();
    }
    if (m_pool) {
        m_pool->shutdown();
    }
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
                                                 m_build->serviceCallbacks());
    m_buildBackend = std::make_unique<ui::BuildServiceBackend>(*m_buildService);
    m_build->setBackend(m_buildBackend.get());
    m_build->setSaveAllHandler(this);
    m_buildActions = new ui::BuildActions(*m_build, this);
    if (auto registered = m_buildActions->registerActions(m_registry); !registered) {
        log::warn(kLog, "build actions: {}", registered.error().describe());
    }
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
    add(wa::kSaveAll, "Save All", [this] { (void)saveAllDocuments(document::SaveReason::SaveAll); });
    add(
        wa::kSaveDocument, "Save",
        [this] {
            if (DocumentEditorView* view = currentView(); view != nullptr) {
                (void)save(*view);
            }
        },
        [this] { return currentView() != nullptr; });
    add(wa::kReloadFromDisk, "Reload All from Disk", [this] { checkExternalChanges(); });
    add(wa::kOpenFolder, "Open Folder...", [this] { showOpenDialog(IWorkbenchPrompts::OpenKind::Folder); });
    add(
        wa::kOpenRecent, "Open Recent...", [this] { showOpenRecent(); },
        [this] { return !recentProjects(*m_services.settings.snapshot()).empty(); });
    add(wa::kCloseProject, "Close Project", [this] { closeProject(); }, [this] { return hasProject(); });
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
    ui::ShellEnvironment environment{m_registry, *m_keymap, m_shellSettings, *m_editors};
    environment.appearance = m_appearance.get();
    environment.appearanceSettings = m_appearance != nullptr ? &m_appearanceSettings : nullptr;
    environment.followActiveLayout = m_options.followActiveLayout;
    return environment;
}

// --- The window ---

void Workbench::attach(ui::ShellWindow& window) {
    FROST_PRE(m_window == nullptr);
    m_window = &window;
    m_files = std::make_unique<OpenFileService>(window);
    window.setOpenFileHandler([this] { showOpenDialog(IWorkbenchPrompts::OpenKind::SolutionOrFile); });
    attachToolWindows();
    attachToolbar();
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
        QMetaObject::invokeMethod(this, [this, path] { (void)openPath(path.toStdString()); }, Qt::QueuedConnection);
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
        if (DocumentEditorView* view = d.path.empty() ? nullptr : m_editors->find(d.path); view != nullptr) {
            refreshMarkers(*view);
        }
    });
    connect(m_build.get(), &ui::BuildController::diagnosticsCleared, this, [this] { refreshAllMarkers(); });
    connect(m_build.get(), &ui::BuildController::requestFailed, this,
            [this](const QString& message) { notify(text("Build"), message, ui::NotificationKind::Error); });

    // Autosave: input activity restarts the idle period; the frame's activation drives the rest.
    QCoreApplication::instance()->installEventFilter(this);
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {
            frameActivated();
        } else if (state == Qt::ApplicationInactive) {
            frameDeactivated();
        }
    });
    scheduleAutosave();
    window.refreshActions();
}

void Workbench::attachToolWindows() {
    ui::ToolWindowManager& windows = m_window->toolWindows();

    auto* explorer = new ui::SolutionExplorer();
    explorer->setController(m_build.get());
    explorer->setProcessLauncher(&m_services.launcher);
    windows.setContent(ui::tool_windows::kProject, explorer);
    m_explorer = explorer;
    connect(explorer, &ui::SolutionExplorer::openFile, this,
            [this](const QString& path) { (void)m_files->open(path.toStdString()); });
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
    toolbar.setSlotAction(ui::MainToolbar::kBuild, ui::build_actions::kBuildSolution);
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

DocumentEditorView* Workbench::currentView() const {
    if (!m_window) {
        return nullptr;
    }
    return dynamic_cast<DocumentEditorView*>(m_window->tabs().currentEditor());
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
    m_appearanceSettings.storeChanged();
    refreshRecent();
    scheduleAutosave();
}

bool Workbench::eventFilter(QObject* watched, QEvent* event) {
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::Wheel:
    case QEvent::InputMethod:
        if (!m_shutDown) {
            m_autosave->noteActivity();
            scheduleAutosave();
        }
        break;
    default: break;
    }
    return QObject::eventFilter(watched, event);
}

void Workbench::showOpenDialog(IWorkbenchPrompts::OpenKind kind) {
    const std::string chosen = m_services.prompts.chooseOpenPath(m_window, kind);
    if (!chosen.empty()) {
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
        QMetaObject::invokeMethod(this, [this, path] { (void)openPath(path); }, Qt::QueuedConnection);
    });
    popup->showCentered(m_window, 0.2);
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
    m_autosaveTimer.stop();
    // Rider saves on exit; what cannot be saved stays in the hot-exit journal.
    if (m_documents->policies().autosave.onFrameDeactivation) {
        const document::SaveAllResult saved = m_documents->saveAll(document::SaveReason::Autosave);
        for (document::FileDocument* document : saved.saved) {
            afterSaved(*document);
        }
    }
    if (auto closed = m_documents->closeJournal(); !closed) {
        log::warn(kLog, "hot-exit journal: {}", closed.error().describe());
    }
    stopClangd();
}

} // namespace frost::app
