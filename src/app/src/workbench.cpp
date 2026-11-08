// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): construction, the build service, the appearance, the
// launcher and Welcome windows, notifications, settings changes, the key event filter, the open and
// settings dialogs, the recent list, the teardown and the shutdown. The other concerns live beside
// it (FND-12a): the prompts in workbench_prompts.cpp, the actions in workbench_actions.cpp, window
// attachment in workbench_attach.cpp and workbench_tool_windows.cpp, Run and Attach in
// workbench_run.cpp, updates in workbench_update.cpp, git clone in workbench_clone.cpp, the
// document side in workbench_documents.cpp, projects in workbench_project.cpp (and its siblings),
// clangd in workbench_clangd.cpp and the language features' adapters in workbench_language.cpp.

#include "frost/app/workbench.hpp"

#include "frost/app/welcome_screen.hpp"
#include "frost/core/assert.hpp"
#include "frost/core/build_info.hpp"
#include "frost/core/log.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/simulated_keyboard.hpp"
#include "frost/io/path.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/debug_actions.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/find/find_feature.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/run_actions.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/run_controller.hpp"
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
#include "frost/ui/workbench/file_system_view.hpp"
#include "frost/vcs/git_locator.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "git_clone.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "managed_debug_preparation.hpp"
#include "project_edit_process.hpp"
#include "settings_dialog.hpp"
#include "unity_control.hpp"
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QStyleHints>
#include <QTemporaryFile>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <thread>
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
        if (auto attachSeq = input::parseSequence("Ctrl+Alt+P", input::SyntaxMode::Strict))
            bindings->push_back(input::Binding{"debug.attachToProcess", {*attachSeq}});
        if (auto testsSeq = input::parseSequence("Alt+Digit8", input::SyntaxMode::Strict))
            bindings->push_back(input::Binding{"window.unitTests", {*testsSeq}});
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
    m_documents->setSaveTransform(
        [this](const document::FileDocument& document, document::SaveReason) { return saveTransformFor(document); });
    m_autosave = std::make_unique<document::AutosaveController>(*m_documents, m_services.clock);
    if (!m_options.sessionDirectory.empty()) {
        m_sessions = std::make_unique<ui::nav::SessionStore>(m_services.fs, m_options.sessionDirectory);
    }
    if (!m_options.directoryRolesPath.empty()) {
        if (auto store = project::DirectoryRoleStore::open(m_services.fs, m_options.directoryRolesPath); store)
            m_directoryRoleStore = std::move(*store);
        else
            log::warn(kLog, "directory roles: {}", store.error().describe());
    }
    if (!m_directoryRoleStore)
        m_directoryRoleStore = project::DirectoryRoleStore::inMemory(m_services.fs);
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
                                   },
                                   [this]() -> std::vector<std::string> {
                                       return m_nav ? m_nav->projectFiles().candidates() : std::vector<std::string>{};
                                   },
                                   // Metadata sources go to the profile's cache only (LTR-1); "" names LTR-6.
                                   m_options.metadataCacheDirectory,
                                   // Empty answers say why: no server, fallback flags, indexing (R0 follow-up 4).
                                   [this](std::string_view path) { return languageReadiness(path); }});
    if (auto registered = m_language->registerActions(m_registry); !registered) {
        log::warn(kLog, "language actions: {}", registered.error().describe());
    }

    m_luaTrustWatch.setInterval(1000);
    connect(&m_luaTrustWatch, &QTimer::timeout, this, [this] {
        if (m_shutDown || m_projectRoot.empty())
            return;
        const auto trust = m_services.trust.snapshot(m_projectRoot);
        const bool trusted = trust.trusted;
        if (trusted && m_projectTrustDialog) {
            const bool allTrusted = std::ranges::all_of(m_projectTrustDialog->roots(), [this](const QString& r) {
                return m_services.trust.isTrusted(r.toUtf8().toStdString());
            });
            if (allTrusted) {
                delete m_projectTrustDialog.data();
                m_projectTrustDialog.clear();
            }
        }
        if (m_solution && trust.revision != m_projectTrustRevision) {
            startProjectEvaluation(m_projectRoot);
            if (m_clangd)
                restartClangd();
        }
        /* Trust granted straight to the store (Settings, another instance) gets the same
         * configure-on-open as the trust prompt (R0 follow-up 15): CMakeService::trustChanged
         * configures a folder that was not configured yet in this session. It writes nothing:
         * trust granted to a parent stays the parent's (revoking it untrusts the project), and a
         * revocation between the snapshot and the call is never undone. Asked before the clangd
         * check, so the configure it starts makes that check wait for its publication. */
        if (!m_solution && m_cmake && trusted && !m_luaObservedTrust)
            m_cmake->service().trustChanged();
        /* A CMake or plain-folder clangd follows trust granted or revoked by any route (Settings,
         * the Attach prompt, revocation); its launch flags depend on it (FND-3). While a CMake
         * configure runs, its model publication compares the trust too and restarts clangd once
         * with the database it writes, so a grant waits (FND-9a: no second restart); if the
         * configure fails, the next idle tick restarts it. A revocation restarts clangd restricted
         * at once, even during a long build: .clangd and --query-driver must stop applying now (R0
         * follow-up 15); a configure that then writes a first database restarts it once more.
         * Once the service is idle, the watch also restarts a clangd whose compile commands moved
         * without a publication saying so (a profile's configure that failed, R0 follow-up 10). A
         * job held in NeedsTrust runs nothing until the user answers, so it does not count as busy.
         * A queued reply read (open, profile switch) does: it publishes the model, which runs the
         * same check, and it may start a configure (R0 follow-up 10). Read before the state, since
         * the read may start that configure as it ends. */
        const bool cmakeLoading = m_cmake && m_cmake->service().modelLoading();
        const build::BuildState cmakeState = m_cmake ? m_cmake->service().state() : build::BuildState::Idle;
        const bool cmakeBusy =
            cmakeLoading || (cmakeState != build::BuildState::Idle && cmakeState != build::BuildState::NeedsTrust);
        if (!m_solution && m_clangd) {
            if (trusted != m_clangdTrusted && (!trusted || !cmakeBusy)) {
                if (m_cmake)
                    restartClangdIfCMakeStateChanged();
                else
                    restartClangd();
            } else if (m_cmake && !cmakeBusy && trusted == m_clangdTrusted &&
                       clangdCMakeStateNow() != m_clangdCMakeState) {
                restartClangdIfCMakeStateChanged();
            }
        }
        if (m_solution && m_explorer) {
            auto model = m_solution;
            if (trust.trusted && trust.revision == m_projectTrustRevision && m_streamedProjects) {
                if (auto evaluated = m_streamedProjects->modelSnapshot())
                    model = std::move(evaluated);
            }
            const auto current = m_services.trust.snapshot(m_projectRoot);
            if (!current.trusted || current.revision != m_projectTrustRevision)
                model = m_solution;
            m_explorer->setSolution(std::move(model), true);
        }
        if (trusted != m_luaObservedTrust) {
            m_luaObservedTrust = trusted;
            if (trusted) {
                startLua(m_projectRoot);
                startManaged(m_projectRoot);
            } else {
                invalidateManagedDebug();
                if (m_engine)
                    m_engine->trustChanged();
                stopLua();
                stopManaged();
            }
        } else if (trusted) {
            checkUnityProjectContext();
        }
    });
    m_luaTrustWatch.start();

    // Settings changes may be published on any thread; they are applied on this one.
    m_settingsSubscription = m_services.settings.subscribe([this](const settings::SettingsChange& change) {
        const bool keymapChanged = std::ranges::any_of(change.keys, [](const std::string& key) {
            return key == settings::key::keymapId.name || key == settings::key::keymapOverrides.name ||
                   key == settings::key::keymapDoubleTapIntervalMs.name;
        });
        QMetaObject::invokeMethod(
            this,
            [this, keymapChanged] {
                if (keymapChanged) {
                    m_keymap->reloadPreferences();
                    if (m_window)
                        m_window->refreshActions();
                }
                onSettingsChanged();
            },
            Qt::QueuedConnection);
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
    for (auto& stopped : m_retiredProjectEvaluations)
        if (stopped.valid())
            stopped.join();
    m_retiredProjectEvaluations.clear();
    m_searchSymbolsSlot.cancel();
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
        if (m_solution && !m_closingSolution) {
            startProjectEvaluation(m_projectRoot);
            if (m_clangd)
                restartClangd();
        }
    });
}

build::CMakeService* Workbench::cmakeService() noexcept { return m_cmake ? &m_cmake->service() : nullptr; }
bool Workbench::cmakeTargetsCurrent() const { return m_cmake && m_cmake->targetsCurrent(); }

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

bool Workbench::interactiveReady() const {
    const bool anyVisible = (m_window && m_window->isVisible()) || (m_launcherWindow && m_launcherWindow->isVisible());
    if (!m_interactiveStartupComplete || !anyVisible || !m_nav)
        return false;
    if (m_projectRoot.empty())
        return m_nav->projectRoot().empty();
    return m_nav->projectRoot() == m_projectRoot && m_nav->initialScanReady() && !m_nav->scanPending();
}

QWidget* Workbench::activeTopWindow() const noexcept {
    if (m_launcherWindow && m_launcherWindow->isVisible())
        return m_launcherWindow.data();
    return m_window.data();
}

void Workbench::showLauncherWindow() {
    m_launcherModeEnabled = true;
    if (m_window && m_window->isVisible()) {
        m_window->hide();
    }
    if (!m_launcherWindow) {
        return;
    }
    refreshRecent();
    if (!m_launcherWindow->isVisible()) {
        if (const QScreen* screen = QGuiApplication::primaryScreen()) {
            const QRect avail = screen->availableGeometry();
            m_launcherWindow->move(avail.center() -
                                   QPoint(m_launcherWindow->width() / 2, m_launcherWindow->height() / 2));
        }
    }
    m_launcherWindow->show();
    m_launcherWindow->raise();
    m_launcherWindow->activateWindow();
}

bool Workbench::showStartupWindow() {
    // general.reopenLastProject: the last project with its tabs (restoreLastProject checks the
    // setting and that the project was not closed explicitly); otherwise the launcher.
    if (!hasProject() && restoreLastProject() && m_window) {
        if (m_launcherWindow && m_launcherWindow->isVisible())
            m_launcherWindow->hide();
        m_window->show();
        m_window->raise();
        m_window->activateWindow();
        return true;
    }
    showLauncherWindow();
    return false;
}

void Workbench::transitionLauncherToMainWindow() {
    if (!m_launcherWindow || !m_launcherWindow->isVisible()) {
        return;
    }
    m_launcherWindow->setTransitioningToIde(true);
    m_launcherWindow->hide();
    m_launcherWindow->setTransitioningToIde(false);
    if (m_window && !m_window->isVisible()) {
        if (const QScreen* screen = QGuiApplication::primaryScreen()) {
            const QRect avail = screen->availableGeometry();
            m_window->move(avail.center() - QPoint(m_window->width() / 2, m_window->height() / 2));
        }
        m_window->show();
        m_window->raise();
        m_window->activateWindow();
    }
}

void Workbench::wireWelcomeScreen(WelcomeScreen* welcome) {
    if (!welcome)
        return;
    connect(welcome, &WelcomeScreen::newProjectRequested, this, [this] { showNewProjectDialog(); });
    connect(welcome, &WelcomeScreen::openRequested, this,
            [this] { showOpenDialog(IWorkbenchPrompts::OpenKind::SolutionOrFile); });
    connect(welcome, &WelcomeScreen::cloneRepositoryRequested, this, [this] {
        QWidget* parentWin = activeTopWindow();
        if (!parentWin)
            return;
        QDialog dialog(parentWin);
        dialog.setObjectName(QStringLiteral("cloneRepositoryDialog"));
        dialog.setWindowTitle(text("Get from Version Control — Clone Repository"));
        dialog.resize(580, 220);
        auto* root = new QVBoxLayout(&dialog);
        auto* form = new QFormLayout();
        auto* urlEdit = new QLineEdit(&dialog);
        urlEdit->setObjectName(QStringLiteral("cloneRepoUrlEdit"));
        urlEdit->setPlaceholderText(QStringLiteral("https://github.com/owner/repository.git"));
        auto* dirRow = new QWidget(&dialog);
        auto* dirLayout = new QHBoxLayout(dirRow);
        dirLayout->setContentsMargins(0, 0, 0, 0);
        auto* dirEdit = new QLineEdit(QDir::toNativeSeparators(QDir::homePath()), dirRow);
        dirEdit->setObjectName(QStringLiteral("cloneRepoDirEdit"));
        auto* browseBtn = new QPushButton(text("Browse..."), dirRow);
        QObject::connect(browseBtn, &QPushButton::clicked, &dialog, [&dialog, dirEdit] {
            const QString chosen =
                QFileDialog::getExistingDirectory(&dialog, text("Select Clone Directory"), dirEdit->text());
            if (!chosen.isEmpty())
                dirEdit->setText(QDir::toNativeSeparators(chosen));
        });
        dirLayout->addWidget(dirEdit, 1);
        dirLayout->addWidget(browseBtn);
        form->addRow(text("Repository URL:"), urlEdit);
        form->addRow(text("Directory:"), dirRow);
        root->addLayout(form);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText(text("Clone"));
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        root->addWidget(buttons);
        ui::installDialogChrome(&dialog, false);
        if (dialog.exec() == QDialog::Accepted) {
            const QString url = urlEdit->text().trimmed();
            const QString targetDir = QDir::toNativeSeparators(dirEdit->text().trimmed());
            if (!url.isEmpty() && !targetDir.isEmpty()) {
                // Runs off the GUI thread with progress and Cancel (cloneRepository).
                if (auto started = cloneRepository(url.toStdString(), targetDir.toStdString()); !started)
                    notify(text("Git Clone"), qs(started.error().message()), ui::NotificationKind::Warning);
            }
        }
    });
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
    connect(welcome, &WelcomeScreen::quickSwitchThemeRequested, this, [this] {
        if (m_window) {
            (void)m_window->showQuickSwitchTheme();
        }
    });
    connect(welcome, &WelcomeScreen::settingsRequested, this, [this] { showSettings(); });
    connect(welcome, &WelcomeScreen::checkForUpdatesRequested, this, [this] { showUpdateCheck(); });
    connect(welcome, &WelcomeScreen::switchVersionRequested, this, [this] { showVersionSwitcher(); });
    {
        const auto snap = m_services.settings.snapshot();
        const QString ch = qs(snap->get(key::updateChannel));
        const bool offline = snap->get(key::networkOffline);
        welcome->setUpdateStatus(
            offline ? text("FrostIDE %1 • Offline (%2)").arg(qs(build_info::kVersion), ch)
                    : text("FrostIDE %1 • Channel: %2 (Up to date)").arg(qs(build_info::kVersion), ch));
    }
}

void Workbench::markInteractiveStartupComplete() {
    if (m_interactiveStartupComplete)
        return;
    m_interactiveStartupComplete = true;
    auto* timer = new QTimer(this);
    timer->setInterval(100);
    connect(timer, &QTimer::timeout, this, [this] {
        if (m_window)
            m_window->setAvailabilityLevel(interactiveReady() ? input::AvailabilityLevel::L1
                                                              : input::AvailabilityLevel::L0);
    });
    timer->start();
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
    // general.confirmExit: closing the IDE window (title bar, Alt+F4, File > Exit) asks first. This
    // application-wide filter runs before the navigation workbench's own close handling. Closes the
    // IDE starts itself (update restart, version switch) set m_exitConfirmed and are not asked.
    if (event->type() == QEvent::Close && m_window && watched == m_window.data() && !m_exitConfirmed && !m_shutDown &&
        m_services.settings.snapshot()->get(key::generalConfirmExit) && !m_services.prompts.confirmExit(m_window)) {
        event->ignore();
        return true;
    }
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
    QWidget* parentWin = activeTopWindow();
    if (!parentWin)
        return;
    if (m_settingsDialog) {
        m_settingsDialog->raise();
        m_settingsDialog->activateWindow();
        return;
    }
    auto* dialog = new SettingsDialog(m_services.settings, m_registry, *m_keymap, parentWin);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_settingsDialog = dialog;
    dialog->open();
}

void Workbench::refreshRecent() {
    const auto recent = recentProjects(*m_services.settings.snapshot());
    if (m_welcome) {
        m_welcome->setRecentProjects(recent);
    }
    if (m_launcherWindow && m_launcherWindow->welcomeScreen()) {
        m_launcherWindow->welcomeScreen()->setRecentProjects(recent);
    }
}

std::string Workbench::defaultNewItemDirectory() const {
    const bool filesVisible = m_window && m_window->toolWindows().isVisible(ui::kFileSystemToolWindow);
    const bool projectVisible = m_window && m_window->toolWindows().isVisible(ui::tool_windows::kProject);
    if (m_solution && m_explorer && (projectVisible || !filesVisible)) {
        const QString explorerDir = m_explorer->currentDirectory();
        if (!explorerDir.isEmpty())
            return explorerDir.toStdString();
    }
    if (m_nav && m_nav->fileSystemView() && !m_nav->fileSystemView()->selectedPath().empty()) {
        const std::string selectedDir = m_nav->fileSystemView()->selectedDirectory();
        if (!selectedDir.empty())
            return selectedDir;
    }
    if (m_explorer) {
        const QString explorerDir = m_explorer->currentDirectory();
        if (!explorerDir.isEmpty())
            return explorerDir.toStdString();
    }
    if (const DocumentEditorView* active = currentView(); active != nullptr && !active->path().empty()) {
        const std::string parent(io::parentPath(active->path()));
        if (m_projectRoot.empty() || parent == m_projectRoot ||
            (parent.starts_with(m_projectRoot) && parent.size() > m_projectRoot.size() &&
             (parent[m_projectRoot.size()] == '/' || parent[m_projectRoot.size()] == '\\'))) {
            return parent;
        }
    }
    return m_projectRoot;
}

void Workbench::shutdown() {
    if (m_shutDown) {
        return;
    }
    m_shutDown = true;
    if (m_clone) {
        // Ends git's process tree and waits for the worker; its queued result is then ignored.
        m_clone->cancelled = true;
        m_clone->worker.request_stop();
        if (m_clone->worker.joinable())
            m_clone->worker.join();
        if (m_clone->dialog) {
            m_clone->dialog->blockSignals(true);
            m_clone->dialog->close();
        }
        m_clone.reset();
    }
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
    if (m_runManager) {
        m_runManager->stopAll(); // RUN-1: every run configuration's process tree ends.
    }
    // Rider saves on exit; what cannot be saved stays in the hot-exit journal.
    if (m_documents->policies().autosave.onFrameDeactivation) {
        const document::SaveAllResult saved = m_documents->saveAll(document::SaveReason::Autosave);
        for (document::FileDocument* document : saved.saved) {
            m_documentRegistry->notifyChanged(document->path());
        }
        for (const document::FailedSave& warning : saved.warnings) {
            log::warn(kLog, "after saving {} during shutdown: {}", warning.document->path(), warning.error.describe());
            notify(text("Saved %1 with a warning").arg(qs(warning.document->path())), qs(warning.error.describe()),
                   ui::NotificationKind::Warning);
        }
    }
    if (auto closed = m_documents->closeJournal(); !closed) {
        log::warn(kLog, "hot-exit journal: {}", closed.error().describe());
    }
    stopLua();
    stopManaged();
    stopClangd();
    stopProjectEvaluation();
}

} // namespace frost::app
