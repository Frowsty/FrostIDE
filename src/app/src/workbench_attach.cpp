// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): attaching to and detaching from the shell window, the
// main toolbar and the language controller's wiring (split from workbench.cpp, FND-12a). The tool
// windows are attached in workbench_tool_windows.cpp and Run in workbench_run.cpp.

#include "frost/app/welcome_screen.hpp"
#include "frost/app/workbench.hpp"
#include "frost/core/assert.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
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
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/terminal/terminal_tool_window.hpp"
#include "frost/ui/toolwindows/build_actions.hpp"
#include "frost/ui/toolwindows/build_output.hpp"
#include "frost/ui/toolwindows/file_manager.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"
#include "frost/ui/tr.hpp"
#include "frost/ui/workbench/file_system_view.hpp"
#include "frost/vcs/git_locator.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "project_properties_dialog.hpp"
#include "settings_dialog.hpp"
#include "unity_control.hpp"
#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QMenu>
#include <QStatusBar>
#include <QTimer>

#include <cstring>
#include <utility>

namespace frost::app {

namespace key = settings::key;
namespace wa = workbench_actions;

namespace {

constexpr std::string_view kLog = "workbench";

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

} // namespace

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
    nav.directoryRoles = m_directoryRoleStore.get();
    nav.autosave = m_autosave.get();
    nav.applicationSettingsFile = m_options.applicationSettingsFile;
    nav.closeProjectHook = [this] { static_cast<void>(closeProject()); };
    nav.openProjectHook = [this](const std::string& root) { return reopenRoot(root); };
    nav.promptNewItem = [this](const std::string& directory, bool folder) {
        return m_services.prompts.chooseNewItemName(m_window, directory, folder);
    };
    nav.defaultNewItemDirectory = [this] { return defaultNewItemDirectory(); };
    nav.fileCreated = [this](const std::string& path) { onNewFileCreated(path); };
    nav.saveAsHook = [this] { showSaveAsDialog(); };
    nav.promptRename = [this](const std::string& path, bool folder) {
        return m_services.prompts.chooseRenameName(m_window, path, folder);
    };
    nav.confirmDelete = [this](const std::string& path, bool folder) {
        return m_services.prompts.confirmDelete(m_window, qs(io::fileName(path)), folder);
    };
    nav.renamePathHook = [this](const std::string& path) { promptAndRenameItem(path); };
    nav.deletePathHook = [this](const std::string& path) { promptAndDeleteItem(path); };
    nav.queryWorkspaceSymbols = [this](std::string_view query, bool classesOnly,
                                       std::function<void(std::vector<ui::WorkbenchSymbol>)> done) {
        queryWorkspaceSymbols(query, classesOnly, std::move(done));
    };
    nav.filesChanged = [this](const io::FileChangeBatch& batch) {
        if (m_cmake)
            m_cmake->filesChanged(batch);
        if (m_git)
            m_git->filesChanged(batch);
    };
    m_nav = std::make_unique<ui::Workbench>(window, std::move(nav));
    if (ui::FileSystemView* fsView = m_nav->fileSystemView()) {
        connect(fsView, &ui::FileSystemView::openContainingFolderRequested, this, [this](const QString& path) {
            const std::string stdPath = path.toStdString();
            const auto st = m_services.fs.status(stdPath);
            const bool isDirectory = st && st->type == io::FileType::Directory;
            if (auto revealed = ui::revealInFileManager(m_services.launcher, stdPath, isDirectory); !revealed) {
                notify(text("Open Containing Folder"), qs(revealed.error().message()), ui::NotificationKind::Error);
            }
        });
        connect(fsView, &ui::FileSystemView::openInTerminalRequested, this, [this](const QString& dir) {
            if (m_terminal) {
                (void)m_terminal->openInTerminal(dir.toStdString());
            }
        });
        connect(fsView, &ui::FileSystemView::addPathsToProjectRequested, this, [this](const QStringList& paths) {
            std::vector<std::string> stdPaths;
            stdPaths.reserve(static_cast<std::size_t>(paths.size()));
            for (const QString& p : paths) {
                stdPaths.push_back(QDir::toNativeSeparators(p).toStdString());
            }
            addPathsToActiveProject(stdPaths);
        });
        connect(fsView, &ui::FileSystemView::addExistingFilesToProjectRequested, this,
                [this](const QString& directory) {
                    const QString startDir = directory.isEmpty() ? qs(m_projectRoot) : directory;
                    const QStringList chosen = QFileDialog::getOpenFileNames(
                        m_window, text("Add Existing Files to Project"), startDir,
                        text("C/C++ and Project Files (*.c *.cc *.cpp *.cxx *.c++ *.cppm *.ixx *.h *.hh *.hpp *.hxx "
                             "*.inl *.rc *.natvis *.hlsl *.glsl);;All Files (*.*)"));
                    if (chosen.isEmpty()) {
                        return;
                    }
                    std::vector<std::string> stdPaths;
                    stdPaths.reserve(static_cast<std::size_t>(chosen.size()));
                    for (const QString& p : chosen) {
                        stdPaths.push_back(QDir::toNativeSeparators(p).toStdString());
                    }
                    addPathsToActiveProject(stdPaths);
                });
        connect(fsView, &ui::FileSystemView::addExistingFolderToProjectRequested, this,
                [this](const QString& directory) {
                    const QString startDir = directory.isEmpty() ? qs(m_projectRoot) : directory;
                    const QString chosen =
                        QFileDialog::getExistingDirectory(m_window, text("Add Existing Folder to Project"), startDir);
                    if (chosen.isEmpty()) {
                        return;
                    }
                    addPathsToActiveProject({QDir::toNativeSeparators(chosen).toStdString()});
                });
        fsView->setCanPasteProvider([this] { return canPastePaths(); });
        connect(fsView, &ui::FileSystemView::copyPathsRequested, this, [this](const QStringList& paths) {
            std::vector<std::string> stdPaths;
            stdPaths.reserve(static_cast<std::size_t>(paths.size()));
            for (const QString& p : paths) {
                stdPaths.push_back(QDir::toNativeSeparators(p).toStdString());
            }
            copyPathsToClipboard(stdPaths);
        });
        connect(fsView, &ui::FileSystemView::cutPathsRequested, this, [this](const QStringList& paths) {
            std::vector<std::string> stdPaths;
            stdPaths.reserve(static_cast<std::size_t>(paths.size()));
            for (const QString& p : paths) {
                stdPaths.push_back(QDir::toNativeSeparators(p).toStdString());
            }
            cutPathsToClipboard(stdPaths);
        });
        connect(fsView, &ui::FileSystemView::pasteIntoDirectoryRequested, this, [this](const QString& targetDir) {
            pasteClipboardIntoDirectory(QDir::toNativeSeparators(targetDir).toStdString());
        });
        connect(fsView, &ui::FileSystemView::duplicatePathRequested, this,
                [this](const QString& path, bool isDirectory) {
                    duplicatePathInExplorer(QDir::toNativeSeparators(path).toStdString(), isDirectory);
                });
        connect(fsView, &ui::FileSystemView::movePathsToDirectoryRequested, this,
                [this](const QStringList& sourcePaths, const QString& targetDirectory) {
                    std::vector<std::string> stdPaths;
                    stdPaths.reserve(static_cast<std::size_t>(sourcePaths.size()));
                    for (const QString& p : sourcePaths) {
                        stdPaths.push_back(QDir::toNativeSeparators(p).toStdString());
                    }
                    movePhysicalPathsToDirectory(stdPaths, QDir::toNativeSeparators(targetDirectory).toStdString());
                });
    }

    attachToolWindows();
    attachNavigationBar();
    m_engine = std::make_unique<EngineIntegration>(*this);
    m_unity = std::make_unique<UnityControlIntegration>(*this);
    attachToolbar();
    attachRun();
    if (auto registered = ui::registerDebugToolWindow(m_window->toolWindows(), m_registry, *m_debug); !registered)
        log::warn(kLog, "debug tool window: {}", registered.error().describe());
    attachLanguage();
    attachResolveContext(); // CPP-3.
    connect(&window.tabs(), &ui::EditorTabs::currentEditorChanged, this, [this](ui::IEditorView*) {
        m_language->editorChanged();
        updateLanguageTarget();
        updateResolveContext();
    });
    attachTerminal();
    attachFind();
    if (m_options.git)
        m_git = std::make_unique<GitIntegration>(*this);
    connect(m_build.get(), &ui::BuildController::needsTrust, this, [this](const QStringList&) {
        if (m_projectTrustDialog) {
            delete m_projectTrustDialog.data();
            m_projectTrustDialog.clear();
        }
    });
    m_trustPrompter = new ui::TrustPrompter(*m_build, &window, this);

    auto* welcome = new WelcomeScreen();
    m_welcome = welcome;
    window.tabs().setEmptyStateWidget(welcome);
    wireWelcomeScreen(welcome);

    auto* launcher = new LauncherWindow();
    m_launcherWindow = launcher;
    wireWelcomeScreen(launcher->welcomeScreen());
    if (m_appearance != nullptr) {
        connect(m_appearance.get(), &ui::AppearanceManager::appearanceChanged, launcher, [launcher] {
            for (QWidget* w : launcher->findChildren<QWidget*>()) {
                if (w != nullptr) {
                    w->update();
                }
            }
            launcher->update();
        });
    }
    connect(launcher, &LauncherWindow::closedByUser, this, [this] {
        m_exitConfirmed = true; // The user closed the launcher; the IDE window is not showing.
        // A vetoed close (the unsaved-changes prompt) leaves later closes to general.confirmExit.
        if (m_window && !m_window->close())
            m_exitConfirmed = false;
        QCoreApplication::quit();
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
    if (m_launcherWindow) {
        m_launcherWindow->setTransitioningToIde(true);
        delete m_launcherWindow.data();
        m_launcherWindow = nullptr;
    }
    delete m_settingsDialog.data();
    m_settingsDialog = nullptr;
    delete m_projectTrustDialog.data();
    m_projectTrustDialog.clear();
    m_projectTrustPromptedRoot.clear();
    m_window->mainToolbar().setProjectMenuBuilder({});
    m_window->mainToolbar().setVcsMenuBuilder({});
    m_window->mainToolbar().setBuildMenuBuilder({});
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
    // The Run tool window outlives the window with its controller (~Workbench deletes it).
    if (m_runWindow != nullptr) {
        m_runWindow->hide();
        m_runWindow->setParent(nullptr);
    }
    m_window = nullptr;
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
        auto* newProj = menu.addAction(text("New Project..."));
        connect(newProj, &QAction::triggered, this, [this] {
            QTimer::singleShot(0, this, [this] {
                if (m_window && m_nav)
                    showNewProjectDialog();
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
    toolbar.setBuildMenuBuilder([this](QMenu& menu) {
        if (m_buildActions != nullptr) {
            menu.addAction(m_buildActions->buildSolution());
            menu.addAction(m_buildActions->rebuildSolution());
            menu.addAction(m_buildActions->cleanSolution());
            m_buildActions->updateEnabled(); // Compile follows the active editor's file.
            menu.addAction(m_buildActions->compileFile());
            menu.addSeparator();
            menu.addAction(m_buildActions->cancelBuild());
        }
        if (m_solution && m_runService && m_runService->startupProject()) {
            const project::ProjectId startupId = *m_runService->startupProject();
            if (startupId.value() < m_solution->projects().size()) {
                const project::ProjectInfo& entry = m_solution->project(startupId);
                menu.addSeparator();
                const QString projName = qs(entry.name);
                QAction* buildStartup = menu.addAction(text("Build '%1'").arg(projName));
                buildStartup->setEnabled(m_build && m_build->canBuild());
                connect(buildStartup, &QAction::triggered, this, [this, startupId] {
                    if (m_build) {
                        (void)m_build->build(build::BuildAction::Build, startupId);
                    }
                });
                QAction* rebuildStartup = menu.addAction(text("Rebuild '%1'").arg(projName));
                rebuildStartup->setEnabled(m_build && m_build->canBuild());
                connect(rebuildStartup, &QAction::triggered, this, [this, startupId] {
                    if (m_build) {
                        (void)m_build->build(build::BuildAction::Rebuild, startupId);
                    }
                });
                QAction* cleanStartup = menu.addAction(text("Clean '%1'").arg(projName));
                cleanStartup->setEnabled(m_build && m_build->canBuild());
                connect(cleanStartup, &QAction::triggered, this, [this, startupId] {
                    if (m_build) {
                        (void)m_build->build(build::BuildAction::Clean, startupId);
                    }
                });
            }
        }
    });
    toolbar.setSlotAction(ui::MainToolbar::kDebug, "run.debug");
    auto* combo = new ui::ConfigurationCombo(*m_build, &toolbar);
    combo->setObjectName(QStringLiteral("solutionConfigurations"));
    combo->setConfigurationManagerHandler([this] {
        if (!m_window || !m_solution)
            return;
        showConfigurationManagerDialog(
            m_window, m_services.fs, *m_solution, static_cast<int>(m_build->activeConfigurationIndex().value_or(0)),
            [this](int idx) {
                if (idx >= 0)
                    (void)m_build->setActiveConfiguration(static_cast<std::size_t>(idx));
            },
            [this] {
                refreshSolutionExplorerForFsChange();
                startProjectEvaluation(m_projectRoot);
                if (m_clangd)
                    restartClangd();
            });
    });
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

} // namespace frost::app
