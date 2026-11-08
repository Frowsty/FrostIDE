// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp): attachToolWindows, the tool windows (Solution Explorer,
// file system, Build, Problems, Run, Debug, Terminal, Find, VCS and the rest) created and wired
// into the shell window (split from workbench.cpp, FND-12a).

#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/ui/debug_actions.hpp"
#include "frost/ui/find/find_feature.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/run_actions.hpp"
#include "frost/ui/run_controller.hpp"
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
#include "project_edit_process.hpp"
#include "project_properties_dialog.hpp"
#include "settings_dialog.hpp"
#include "unity_control.hpp"
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>

#include <algorithm>
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

void Workbench::attachToolWindows() {
    ui::ToolWindowManager& windows = m_window->toolWindows();

    auto* explorer = new ui::SolutionExplorer();
    explorer->setCppHierarchyMode(m_cppHierarchyMode);
    m_projectEdit = std::make_unique<ProjectEditProcess>(m_services.launcher, m_services.clock);
    explorer->setController(m_build.get());
    explorer->setProcessLauncher(&m_services.launcher);
    // Project membership edits go only through the ProjectHost helper (removeProjectFile finds it
    // beside the application when no override is set); the regex writers are gone (SLN-1, FND-3).
    explorer->setRemoveProjectFileHandler(
        [this](const QString& project, const QString& source) { removeProjectFile(project, source); });
    explorer->setAddProjectFileHandler([this](const QString& project) {
        if (m_options.selectExistingProjectSource) {
            const QString source = m_options.selectExistingProjectSource(project);
            if (!source.isEmpty()) {
                removeProjectFile(project, source, true);
            }
            return;
        }
        const QString selected =
            QFileDialog::getOpenFileName(m_window, text("Add Existing C/C++ File"), QFileInfo(project).absolutePath(),
                                         text("C/C++ Sources and Headers (*.c *.cpp *.h *.hpp)"));
        if (!selected.isEmpty()) {
            removeProjectFile(project, selected, true);
        }
    });
    explorer->setAddNewProjectFileHandler([this, explorer](const QString& project) {
        const std::string targetFilter = explorer ? explorer->contextFilterPath().toStdString() : std::string{};
        const std::string dir = QDir::toNativeSeparators(QFileInfo(project).absolutePath()).toStdString();
        const std::string name = m_services.prompts.chooseNewItemName(m_window, dir, false);
        if (name.empty()) {
            return;
        }
        auto created = createNewProjectSource(project.toStdString(), name);
        if (created && !targetFilter.empty()) {
            // ProjectHost files the new item itself; choosing the filter needs SLN-6.
            notify(text("Add New File"),
                   text("The file was not placed in filter '%1': moving files to a filter requires safe solution "
                        "and project edits (SLN-6).")
                       .arg(qs(targetFilter)),
                   ui::NotificationKind::Warning);
        }
    });
    explorer->setNewItemHandlers(
        [this](const QString& dir) {
            if (m_nav)
                m_nav->promptAndCreateNewFile(dir.toStdString());
        },
        [this](const QString& dir) {
            if (m_nav)
                m_nav->promptAndCreateNewFolder(dir.toStdString());
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
    connect(explorer, &ui::SolutionExplorer::renameRequested, this,
            [this](const QString& path, bool) { promptAndRenameItem(path.toStdString()); });
    connect(explorer, &ui::SolutionExplorer::deleteRequested, this,
            [this](const QString& path, bool) { promptAndDeleteItem(path.toStdString()); });
    connect(explorer, &ui::SolutionExplorer::commandFailed, this, [this](const QString& message) {
        notify(text("Solution Explorer"), message, ui::NotificationKind::Error);
    });
    connect(explorer, &ui::SolutionExplorer::projectPropertiesRequested, this, [this](const QString& projectPath) {
        if (!m_window || !m_solution)
            return;
        std::string activeCfg = "Debug|x64";
        if (const auto idx = m_build->activeConfigurationIndex(); idx && *idx < m_build->configurations().size())
            activeCfg = m_build->configurations()[*idx].name();
        showProjectPropertiesDialog(m_window, m_services.fs, projectPath.toStdString(), activeCfg, [this] {
            refreshSolutionExplorerForFsChange();
            startProjectEvaluation(m_projectRoot);
            if (m_clangd)
                restartClangd();
        });
    });
    connect(explorer, &ui::SolutionExplorer::solutionPropertiesRequested, this, [this] {
        if (!m_window || !m_solution)
            return;
        showSolutionPropertiesDialog(
            m_window, m_services.fs, *m_solution, m_runService ? m_runService->startupProject() : std::nullopt,
            [this](project::ProjectId id) {
                if (m_runController)
                    (void)m_runController->setStartupProject(id);
            },
            [this] { refreshSolutionExplorerForFsChange(); });
    });
    connect(explorer, &ui::SolutionExplorer::configurationManagerRequested, this, [this] {
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
    connect(explorer, &ui::SolutionExplorer::editRunConfigurationsRequested, this, [this](const QString& projectPath) {
        if (!m_window || !m_solution || !m_runService)
            return;
        std::string activeCfg = "Debug|x64";
        if (const auto idx = m_build->activeConfigurationIndex(); idx && *idx < m_build->configurations().size())
            activeCfg = m_build->configurations()[*idx].name();
        showEditRunConfigurationsDialog(
            m_window, m_services.fs, m_solution.get(), projectPath.toStdString(), activeCfg,
            m_runService->useExternalConsole(),
            [this](bool ext) {
                if (m_runService)
                    (void)m_runService->setUseExternalConsole(ext);
            },
            [this] { refreshSolutionExplorerForFsChange(); });
    });
    /*
     * Solution and project edits other than the single-file add and remove ProjectHost makes are
     * refused until SLN-6 (safe solution and project edits). The explorer disables their menu
     * actions up front with this reason as the tooltip; a request that still arrives (a key, a drop)
     * is refused at once with a notification, before any picker or confirmation, and nothing is
     * written.
     */
    explorer->setUnsupportedEditsReason(text("Requires safe solution and project edits (SLN-6)."));
    const auto refuseSolutionEdit = [this](const QString& title, const QString& what) {
        notify(title,
               text("%1 is not available yet: it requires safe solution and project edits (SLN-6). No file was "
                    "changed.")
                   .arg(what),
               ui::NotificationKind::Error);
    };
    connect(explorer, &ui::SolutionExplorer::addExistingProjectRequested, this, [refuseSolutionEdit](const QString&) {
        refuseSolutionEdit(text("Add Existing Project"), text("Adding a project to the solution"));
    });
    connect(explorer, &ui::SolutionExplorer::removeProjectFromSolutionRequested, this,
            [refuseSolutionEdit](const QString&) {
                refuseSolutionEdit(text("Remove Project"), text("Removing a project from the solution"));
            });
    connect(explorer, &ui::SolutionExplorer::addProjectReferenceRequested, this, [this](const QString& projectPath) {
        if (!m_window || !m_solution)
            return;
        showAddProjectReferenceDialog(m_window, m_services.fs, *m_solution, projectPath.toStdString(),
                                      [this] { refreshSolutionExplorerForFsChange(); });
    });
    connect(explorer, &ui::SolutionExplorer::manageNuGetPackagesRequested, this, [this](const QString& projectPath) {
        if (!m_window || !m_solution)
            return;
        showNuGetPackageManagerDialog(m_window, m_services.fs, projectPath.toStdString(),
                                      [this] { refreshSolutionExplorerForFsChange(); });
    });
    connect(explorer, &ui::SolutionExplorer::cppHierarchyModeChanged, this, [this](project::CppHierarchyMode mode) {
        m_cppHierarchyMode = mode;
        refreshSolutionExplorerForFsChange();
    });
    connect(explorer, &ui::SolutionExplorer::addFilterRequested, this,
            [refuseSolutionEdit](const QString&, const QString&) {
                refuseSolutionEdit(text("Add Filter"), text("Adding a filter"));
            });
    connect(explorer, &ui::SolutionExplorer::renameFilterRequested, this,
            [refuseSolutionEdit](const QString&, const QString&) {
                refuseSolutionEdit(text("Rename Filter"), text("Renaming a filter"));
            });
    connect(explorer, &ui::SolutionExplorer::removeFilterRequested, this,
            [refuseSolutionEdit](const QString&, const QString&) {
                refuseSolutionEdit(text("Remove Filter"), text("Removing a filter"));
            });
    connect(explorer, &ui::SolutionExplorer::moveFilesToFilterRequested, this,
            [refuseSolutionEdit](const QString&, const QStringList&, const QString&) {
                refuseSolutionEdit(text("Move to Filter"), text("Moving files to a filter"));
            });
    connect(explorer, &ui::SolutionExplorer::moveFilesToNewFilterRequested, this,
            [refuseSolutionEdit](const QString&, const QStringList&) {
                refuseSolutionEdit(text("Move to Filter"), text("Moving files to a new filter"));
            });
    connect(explorer, &ui::SolutionExplorer::addExistingFolderToFilterRequested, this,
            [refuseSolutionEdit](const QString&, const QString&) {
                refuseSolutionEdit(text("Add Existing Folder"), text("Adding a folder to a project"));
            });
    connect(explorer, &ui::SolutionExplorer::removeMultipleProjectFilesRequested, this,
            [refuseSolutionEdit](const QString&, const QStringList&) {
                refuseSolutionEdit(text("Remove from Project"), text("Removing several files from a project"));
            });
    explorer->setCanPasteProvider([this] { return canPastePaths(); });
    connect(explorer, &ui::SolutionExplorer::copyPathsRequested, this, [this](const QStringList& paths) {
        std::vector<std::string> stdPaths;
        stdPaths.reserve(static_cast<std::size_t>(paths.size()));
        for (const QString& p : paths) {
            stdPaths.push_back(QDir::toNativeSeparators(p).toStdString());
        }
        copyPathsToClipboard(stdPaths);
    });
    connect(explorer, &ui::SolutionExplorer::cutPathsRequested, this, [this](const QStringList& paths) {
        std::vector<std::string> stdPaths;
        stdPaths.reserve(static_cast<std::size_t>(paths.size()));
        for (const QString& p : paths) {
            stdPaths.push_back(QDir::toNativeSeparators(p).toStdString());
        }
        cutPathsToClipboard(stdPaths);
    });
    connect(explorer, &ui::SolutionExplorer::pasteIntoDirectoryRequested, this,
            [this](const QString& targetDirectory, const QString& targetProjectPath, const QString& targetFilterPath) {
                pasteClipboardIntoDirectory(QDir::toNativeSeparators(targetDirectory).toStdString(),
                                            QDir::toNativeSeparators(targetProjectPath).toStdString(),
                                            targetFilterPath.toStdString());
            });
    connect(explorer, &ui::SolutionExplorer::duplicatePathRequested, this,
            [this](const QString& path, bool isDirectory, const QString& projectPath, const QString& filterPath) {
                duplicatePathInExplorer(QDir::toNativeSeparators(path).toStdString(), isDirectory,
                                        QDir::toNativeSeparators(projectPath).toStdString(), filterPath.toStdString());
            });
    connect(explorer, &ui::SolutionExplorer::movePhysicalPathsRequested, this,
            [this](const QStringList& sourcePaths, const QString& targetDirectory, const QString& projectPath) {
                std::vector<std::string> stdPaths;
                stdPaths.reserve(static_cast<std::size_t>(sourcePaths.size()));
                for (const QString& p : sourcePaths) {
                    stdPaths.push_back(QDir::toNativeSeparators(p).toStdString());
                }
                movePhysicalPathsToDirectory(stdPaths, QDir::toNativeSeparators(targetDirectory).toStdString(),
                                             QDir::toNativeSeparators(projectPath).toStdString());
            });
    connect(explorer, &ui::SolutionExplorer::addSolutionFolderRequested, this, [refuseSolutionEdit](const QString&) {
        refuseSolutionEdit(text("Add Solution Folder"), text("Adding a solution folder"));
    });
    connect(explorer, &ui::SolutionExplorer::renameSolutionFolderRequested, this,
            [refuseSolutionEdit](const QString&, const QString&) {
                refuseSolutionEdit(text("Rename Solution Folder"), text("Renaming a solution folder"));
            });
    connect(explorer, &ui::SolutionExplorer::removeSolutionFolderRequested, this, [refuseSolutionEdit](const QString&) {
        refuseSolutionEdit(text("Remove Solution Folder"), text("Removing a solution folder"));
    });
    connect(explorer, &ui::SolutionExplorer::moveProjectToSolutionFolderRequested, this,
            [refuseSolutionEdit](const QString&, const QString&) {
                refuseSolutionEdit(text("Move to Solution Folder"), text("Moving a project to a solution folder"));
            });
    connect(explorer, &ui::SolutionExplorer::moveProjectToNewSolutionFolderRequested, this,
            [refuseSolutionEdit](const QString&) {
                refuseSolutionEdit(text("Move to Solution Folder"), text("Moving a project to a new solution folder"));
            });
    connect(explorer, &ui::SolutionExplorer::unloadProjectRequested, this, [this](const QString& projectPath) {
        if (!m_solution || projectPath.isEmpty())
            return;
        const std::string stdPath = QDir::toNativeSeparators(projectPath).toStdString();
        const std::string key = lang::documentKey(stdPath);
        const bool already = std::ranges::any_of(m_unloadedProjectPaths,
                                                 [&](const std::string& p) { return lang::documentKey(p) == key; });
        if (!already) {
            m_unloadedProjectPaths.push_back(stdPath);
        }
        refreshSolutionExplorerForFsChange();
        startProjectEvaluation(m_projectRoot);
        if (m_clangd) {
            restartClangd();
        }
    });
    connect(explorer, &ui::SolutionExplorer::reloadProjectRequested, this, [this](const QString& projectPath) {
        if (!m_solution || projectPath.isEmpty())
            return;
        const std::string stdPath = QDir::toNativeSeparators(projectPath).toStdString();
        const std::string key = lang::documentKey(stdPath);
        std::erase_if(m_unloadedProjectPaths, [&](const std::string& p) { return lang::documentKey(p) == key; });
        refreshSolutionExplorerForFsChange();
        startProjectEvaluation(m_projectRoot);
        if (m_clangd) {
            restartClangd();
        }
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

    ui::ToolWindowSpec nugetSpec;
    nugetSpec.id = std::string(kNuGetToolWindow);
    nugetSpec.title = text("NuGet");
    nugetSpec.side = ui::ToolWindowSide::Bottom;
    nugetSpec.iconId = std::string(ui::icons::kBuildPackage);
    nugetSpec.createContent = [this]() -> QWidget* {
        std::string initialProj;
        if (m_explorer && m_solution) {
            if (const auto projId = m_explorer->currentProject())
                initialProj = m_solution->project(*projId).path;
        }
        return createNuGetToolWindowWidget(
            m_window, m_services.fs, [this]() -> const project::SolutionModel* { return m_solution.get(); },
            std::move(initialProj), [this] { refreshSolutionExplorerForFsChange(); });
    };
    if (auto added = windows.add(std::move(nugetSpec)); !added) {
        log::warn(kLog, "nuget tool window: {}", added.error().describe());
    }

    ui::ToolWindowSpec asmSpec;
    asmSpec.id = std::string(kAsmViewerToolWindow);
    asmSpec.title = text("ASM / IL");
    asmSpec.side = ui::ToolWindowSide::Right;
    asmSpec.iconId = std::string(ui::icons::kNodesFileCode);
    asmSpec.createContent = [this]() -> QWidget* {
        return createAsmViewerToolWindowWidget(m_window, [this]() -> std::pair<std::string, int> {
            if (DocumentEditorView* view = currentView()) {
                const auto caretPos =
                    view->editor().editor().text().pointAt(view->editor().editor().state().primary().head);
                return {view->path(), static_cast<int>(caretPos.line.value()) + 1};
            }
            return {{}, 1};
        });
    };
    if (auto added = windows.add(std::move(asmSpec)); !added) {
        log::warn(kLog, "asm viewer tool window: {}", added.error().describe());
    }

    ui::ToolWindowSpec unitTestsSpec;
    unitTestsSpec.id = std::string(kUnitTestsToolWindow);
    unitTestsSpec.title = text("Unit Tests");
    unitTestsSpec.side = ui::ToolWindowSide::Bottom;
    unitTestsSpec.iconId = std::string(ui::icons::kRunRun);
    unitTestsSpec.createContent = [this]() -> QWidget* {
        return createUnitTestsToolWindowWidget(
            m_window, m_services.fs, [this]() -> const project::SolutionModel* { return m_solution.get(); },
            [this]() -> std::string { return m_projectRoot; },
            [this](const std::string& path, int line) { openLocation(qs(path), line, 1); });
        // No "Debug Selected Test" callback: it started the whole program under the debugger, not
        // the test. Debugging one test is RUN-10's.
    };
    if (auto added = windows.add(std::move(unitTestsSpec)); !added) {
        log::warn(kLog, "unit tests tool window: {}", added.error().describe());
    }
}

} // namespace frost::app
