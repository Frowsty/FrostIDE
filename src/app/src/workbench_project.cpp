// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp), the project side: opening solutions, CMake projects, folders
// and files, the project trust offer, closing the project, the last project at start-up and the
// recent-projects list. Its siblings (FND-12a): project and file creation in
// workbench_new_project.cpp, source membership edits (CMakeLists.txt, .vcxproj) in
// workbench_project_sources.cpp, Solution Explorer file operations in workbench_file_ops.cpp and
// clangd and the project evaluation in workbench_clangd.cpp.

#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/debug/cmake_dap.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/msbuild_compile_commands.hpp"
#include "frost/lang/msvc_environment.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/standalone_project.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"
#include "frost/ui/tr.hpp"

#include "cmake_integration.hpp"
#include "engine_binding_integration.hpp"
#include "engine_integration.hpp"
#include "engine_mixed_launch.hpp"
#include "engine_run_debug.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "project_edit_process.hpp"
#include "standalone_build_backend.hpp"
#include "unity_control.hpp"
#include "workbench_project_helpers.hpp"
#include "workbench_source_rename_preview.hpp"
#include <QDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <utility>

namespace frost::app {

namespace {

constexpr std::string_view kLog = "workbench";

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

} // namespace

bool isCFamilySource(std::string_view path) noexcept {
    static constexpr std::array<std::string_view, 18> kExtensions{".c",   ".cc",  ".cpp", ".cxx", ".c++", ".cppm",
                                                                  ".ixx", ".h",   ".hh",  ".hpp", ".hxx", ".h++",
                                                                  ".inl", ".ipp", ".tpp", ".m",   ".mm",  ".cu"};
    const std::string extension = extensionOf(path);
    return std::ranges::find(kExtensions, extension) != kExtensions.end();
}

bool isSolutionFile(std::string_view path) noexcept {
    const std::string extension = extensionOf(path);
    return extension == ".sln" || extension == ".slnx";
}

// --- Opening ---

Result<void> Workbench::openCMakeProject(std::string_view path) {
    const std::string canonical = m_documentRegistry->canonicalize(path);
    const auto status = m_services.fs.status(canonical);
    if (!status)
        return std::unexpected(status.error());
    if (status->type != io::FileType::Regular || io::fileName(canonical) != "CMakeLists.txt")
        return fail(ErrorCode::InvalidArgument, "a CMake project must name an existing CMakeLists.txt");
    return openFolder(std::string(io::parentPath(canonical)), false, canonical);
}

Result<void> Workbench::openPath(std::string_view path, FileLocation at) {
    if (m_solutionLoading || m_shutDown)
        return fail(ErrorCode::Busy, "A solution is loading or the Workbench is closing");
    // The registry's canonical spelling (one document per file), the lexical form for a missing one.
    const std::string canonical = m_documentRegistry->canonicalize(path);
    Result<io::FileStatus> status = m_services.fs.status(canonical);
    if (!status) {
        notify(text("Cannot open %1").arg(qs(canonical)), qs(status.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(status.error()));
    }
    if (status->type == io::FileType::Directory) {
        return openFolder(canonical);
    }
    if (extensionOf(canonical) == ".frostproj")
        return openFolder(std::string(io::parentPath(canonical)), false, canonical);
    const auto marker = io::fileName(canonical);
    if (!hasProject() &&
        (marker == "compile_commands.json" || marker == "Makefile" || marker == "makefile" || marker == "GNUmakefile"))
        return openFolder(std::string(io::parentPath(canonical)), false, canonical);
    if (isSolutionFile(canonical)) {
        return openSolution(canonical);
    }
    if (!m_nav) {
        return fail(ErrorCode::InvalidArgument, "the workbench has no window");
    }
    std::optional<ui::LinePosition> position;
    if (at.line > 0) {
        position = ui::LinePosition{at.line, std::max(at.column, 1)};
    }
    // The shell notifies a refusal (the large-file guard, a read error) itself.
    Result<int> opened = m_nav->openFile(canonical, position);
    if (!opened) {
        return std::unexpected(std::move(opened.error()));
    }
    return {};
}

Result<void> Workbench::openExternalProject(std::string_view path) {
    // Resolve the explicit project argument itself: on POSIX DocumentRegistry normally keeps
    // symlink spellings, while project IPC and FrostEngine identify the target directory.
    const QString resolved = QFileInfo(qs(path)).canonicalFilePath();
    const std::string canonical =
        resolved.isEmpty() ? m_documentRegistry->canonicalize(path) : resolved.toUtf8().toStdString();
    auto status = m_services.fs.status(canonical);
    if (!status)
        return std::unexpected(std::move(status.error()));
    if (status->type == io::FileType::Regular && extensionOf(canonical) == ".frostproj")
        return openPath(canonical);
    const auto marker = io::fileName(canonical);
    if (status->type == io::FileType::Regular &&
        (marker == "compile_commands.json" || marker == "Makefile" || marker == "makefile" || marker == "GNUmakefile"))
        return openFolder(std::string(io::parentPath(canonical)), false, canonical);
    if (status->type != io::FileType::Directory)
        return fail(ErrorCode::InvalidArgument,
                    "--project requires a directory, .frostproj, Makefile or compile_commands.json");
    auto entries = m_services.fs.listDirectory(canonical);
    if (!entries)
        return std::unexpected(std::move(entries.error()));
    std::vector<std::string> projects;
    for (const auto& entry : *entries) {
        if (entry.type == io::FileType::Regular && extensionOf(entry.name) == ".frostproj")
            projects.push_back(entry.name);
    }
    if (projects.size() > 1)
        return fail(ErrorCode::InvalidArgument, "project directory has multiple .frostproj files");
    if (!projects.empty()) {
        auto file = io::joinPath(canonical, projects.front());
        if (!file)
            return std::unexpected(std::move(file.error()));
        return openPath(*file);
    }
    return openFolder(canonical, false);
}

Result<void> Workbench::openSolution(const std::string& path) {
    if (m_solutionLoading || m_shutDown)
        return fail(ErrorCode::Busy, "A solution is loading or the Workbench is closing");
    if (m_solution && lang::documentKey(m_solution->path()) == lang::documentKey(path)) {
        transitionLauncherToMainWindow();
        offerProjectTrust();
        return {}; // Already the project.
    }
    QPointer<Workbench> owner(this);
    m_solutionLoading = true;
    m_solutionLoadStop = std::stop_source{};
    auto loaded = loadSolutionInBackground(
        m_window, m_services.fs, path,
        {.cppHierarchy = m_cppHierarchyMode, .unloadedProjectPaths = m_unloadedProjectPaths}, m_solutionLoadStop);
    if (!owner)
        return fail(ErrorCode::Cancelled, "Workbench closed while reading the solution");
    m_solutionLoading = false;
    if (m_shutDown)
        return fail(ErrorCode::Cancelled, "Workbench shut down while reading the solution");
    if (!loaded) {
        if (loaded.error().code() == ErrorCode::Cancelled)
            return std::unexpected(std::move(loaded.error()));
        notify(text("Cannot open %1").arg(qs(path)), qs(loaded.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(loaded.error()));
    }
    if (!closeForOpen()) {
        return fail(ErrorCode::Cancelled, "the current project could not be closed");
    }
    auto model = std::make_shared<const project::SolutionModel>(std::move(*loaded));
    m_solution = model;
    if (model->approximate())
        notify(text("Approximate solution"), qs(std::string(model->approximationNotice())),
               ui::NotificationKind::Warning);
    if (auto opened = m_build->openSolution(model); !opened) {
        // The explorer still shows the solution; building is unavailable.
        notify(text("The build service cannot open %1").arg(qs(model->name())), qs(opened.error().message()),
               ui::NotificationKind::Warning);
        if (m_explorer) {
            m_explorer->setSolution(model);
        }
    }
    if (auto journal = io::joinPath(model->directory(), ".frost-project-edit-journal"); journal) {
        const auto exists = m_services.fs.exists(*journal);
        if (!exists || *exists) {
            if (auto blocked = m_buildService->beginProjectEdit(); blocked) {
                notify(text("Project recovery required"),
                       text("A project edit journal is present or cannot be checked. Builds are blocked until it is "
                            "resolved."),
                       ui::NotificationKind::Warning);
            }
        }
    }
    if (auto opened = m_runService->openSolution(model); !opened) {
        log::warn(kLog, "run service: {}", opened.error().describe());
    }
    m_runController->refreshStartupCombo();
    // The navigation workbench: Go to File over the solution's files, the session's tabs.
    if (m_nav) {
        if (auto opened = m_nav->openSolution(*model); !opened) {
            log::warn(kLog, "navigation: {}", opened.error().describe());
        }
    }
    setProject(model->directory(), qs(model->name()), path);
    if (m_window) {
        m_window->toolWindows().show(ui::tool_windows::kProject, false);
    }
    startProjectEvaluation(model->directory());
    startClangd(model->directory());
    startLua(model->directory());
    startManaged(model->directory());
    refreshAllMarkers();
    offerProjectTrust();
    return {};
}

Result<void> Workbench::openFolder(const std::string& path, bool discoverSolution, std::string recentPath) {
    if (m_solutionLoading || m_shutDown)
        return fail(ErrorCode::Busy, "A solution is loading or the Workbench is closing");
    // A folder with one solution opens that solution (.sln before .slnx of the same name).
    if (Result<std::vector<io::DirEntry>> entries = m_services.fs.listDirectory(path); discoverSolution && entries) {
        std::vector<std::string> solutions;
        for (const io::DirEntry& entry : *entries) {
            if (entry.type == io::FileType::Regular && isSolutionFile(entry.name)) {
                solutions.push_back(entry.name);
            }
        }
        std::ranges::sort(solutions);
        const auto stem = [](const std::string& name) { return name.substr(0, name.find_last_of('.')); };
        const bool oneSolution = !solutions.empty() && std::ranges::all_of(solutions, [&](const std::string& name) {
            return stem(name) == stem(solutions.front());
        });
        if (oneSolution) {
            if (Result<std::string> solution = io::joinPath(path, solutions.front()); solution) {
                return openSolution(*solution);
            }
        }
    }
    if (recentPath.empty())
        recentPath = path;
    if (m_projectRoot == path && !m_solution && (recentPath == path || m_projectIdentity == recentPath)) {
        // The same directory may have been opened by a folder or a .frostproj. Preserve the
        // caller's explicit choice for Open Recent and the next process's session restore.
        if (auto added = addRecentProject(m_services.settings, recentPath); !added) {
            log::warn(kLog, "recent projects: {}", added.error().describe());
        }
        refreshRecent();
        m_projectIdentity = recentPath;
        if (m_sessions) {
            if (auto saved = m_sessions->setLastProject(m_projectIdentity); !saved)
                log::warn(kLog, "last project: {}", saved.error().describe());
        }
        transitionLauncherToMainWindow();
        offerProjectTrust();
        return {};
    }
    if (!closeForOpen()) {
        return fail(ErrorCode::Cancelled, "the current project could not be closed");
    }
    const auto lists = io::joinPath(path, "CMakeLists.txt");
    // CMK-17: ordinary folders remain lightweight. No Make recipe is evaluated by opening one.
    // CMake wins for folder discovery; explicit Make/database markers select the standalone model.
    const bool explicitStandalone = io::fileName(recentPath) == "compile_commands.json" ||
                                    io::fileName(recentPath) == "Makefile" || io::fileName(recentPath) == "makefile" ||
                                    io::fileName(recentPath) == "GNUmakefile";
    if (m_window && !explicitStandalone && lists && m_services.fs.exists(*lists).value_or(false)) {
        m_cmake = std::make_unique<CMakeIntegration>(
            m_services.fs, m_services.launcher, m_services.trust, m_services.settings, *m_build, *m_buildService,
            *m_window, *m_runWindow, *m_runManager, m_options.cmakeStatePath,
            [this] { m_runController->alternateStateChanged(); },
            [this](const std::string& file) { (void)openPath(file); },
            [this] {
                // FND-9a: the open's own model publication (and a configure that leaves the
                // database where clangd already reads it) must not spawn a second clangd.
                if (m_cmake && !m_clangdDeferredRoot.empty())
                    resumeDeferredClangd();
                else if (m_cmake && m_clangd)
                    restartClangdIfCMakeStateChanged();
            });
        connectCMakeRunConfigurations(); // RUN-2/RUN-3: the targets' run configurations.
        m_cmake->setConfigureDebugger([this](std::string endpoint, std::string root,
                                             std::function<void()> cancel) -> Result<void> {
            if (!m_debug)
                return fail(ErrorCode::Unsupported, "Debugger UI is unavailable");
            debug::DebugTarget target;
            target.name = "CMake configure";
            target.program = io::joinPath(root, "CMakeLists.txt").value_or("");
            target.trustRoots = {root};
            auto admission = [trust = &m_services.trust, root] { return trust->requireTrusted(root); };
            return m_debug->startAdapterTarget(target, debug::cmakeDapFactory(std::move(endpoint), std::move(cancel)),
                                               rpc::Json::object(), "cmake", debug::DebugTargetMode::Launch, false,
                                               std::move(admission));
        });
        if (auto opened = m_cmake->open(path); !opened) {
            m_cmake.reset();
            return std::unexpected(std::move(opened.error()));
        }
    }
    if (!m_cmake) {
        auto standalone = project::readStandaloneProject(m_services.fs, path, false);
        if (!standalone)
            return std::unexpected(standalone.error());
        if (*standalone) {
            const auto marker = io::fileName(recentPath);
            if (marker == "Makefile" || marker == "makefile" || marker == "GNUmakefile")
                (**standalone).makefile = recentPath;
            m_standalone = std::make_unique<StandaloneBuildBackend>(
                std::move(**standalone), m_services.fs, m_services.launcher, m_services.trust, *m_buildService);
            const QPointer<StandaloneBuildBackend> identity(m_standalone.get());
            m_standalone->setCallbacks(m_build->serviceCallbacks([identity] { return !identity.isNull(); }));
            m_build->setBackend(m_standalone.get());
            if (m_window)
                m_standalone->attachMenu(*m_window);
        }
    }
    if (m_nav) {
        if (auto opened = m_nav->openProject(path); !opened) {
            log::warn(kLog, "navigation: {}", opened.error().describe());
        }
        if (m_cmake) {
            m_cmake->setTargetPreferences(m_nav->cmakeTargets(), [this](const auto& targets) {
                if (m_nav)
                    m_nav->setCMakeTargets(targets);
            });
            m_cmake->setArgumentPreferences(m_nav->cmakeArguments(), [this](const auto& arguments) {
                if (m_nav)
                    m_nav->setCMakeArguments(arguments);
            });
        }
    }
    const std::string_view name = io::fileName(path);
    setProject(path, qs(name.empty() ? std::string_view(path) : name), std::move(recentPath));
    if (m_window) {
        // A folder has no solution to show: the File System view takes the Solution Explorer's place.
        m_window->toolWindows().hide(ui::tool_windows::kProject);
        if (!m_cmake)
            m_window->toolWindows().show(ui::kFileSystemToolWindow, false);
    }
    /*
     * FND-9a: resolve where clangd's compile commands come from before starting it, so an open
     * spawns one clangd. A trusted CMake folder whose configure-on-open is still producing its first
     * compile_commands.json starts clangd when that configure ends (resumeDeferredClangd), instead
     * of starting now without a database and restarting when it appears. With a database already in
     * the active build directory, clangd starts now and the configure's model does not restart it.
     */
    if (m_options.clangd && m_cmake && m_cmake->service().state() != build::BuildState::Idle &&
        !cmakeClangdState(m_services.fs, m_cmake->service()).ends_with(kCMakeDatabasePresent)) {
        stopClangd();
        m_clangdDeferredRoot = path;
        m_clangdDeferral =
            connect(m_build.get(), &ui::BuildController::stateChanged, this, [this](build::BuildState state) {
                if (state == build::BuildState::Idle)
                    resumeDeferredClangd(true);
            });
        log::info(kLog, "clangd waits for the CMake configure of {}", path);
    } else {
        startClangd(path);
    }
    startLua(path);
    startManaged(path);
    offerProjectTrust();
    return {};
}

void Workbench::offerProjectTrust() {
    if (m_shutDown || m_projectRoot.empty() || !m_window || !m_build) {
        return;
    }
    QStringList untrustedRoots;
    const auto addUntrusted = [&](std::string_view candidate) {
        if (candidate.empty() || m_services.trust.isTrusted(candidate)) {
            return;
        }
        const QString qpath = qs(candidate);
        if (!untrustedRoots.contains(qpath)) {
            untrustedRoots.push_back(qpath);
        }
    };
    addUntrusted(m_projectRoot);
    if (m_solution) {
        for (const project::ProjectInfo& proj : m_solution->projects()) {
            if (proj.nodeKind == project::NodeKind::Project && !proj.path.empty()) {
                addUntrusted(io::parentPath(proj.path));
            }
        }
    }
    if (untrustedRoots.isEmpty()) {
        m_window->setProjectTrusted(true);
        if (m_projectTrustDialog) {
            delete m_projectTrustDialog.data();
            m_projectTrustDialog.clear();
        }
        return;
    }
    m_window->setProjectTrusted(false);
    if (m_projectTrustDialog) {
        m_projectTrustDialog->setRoots(untrustedRoots);
        return;
    }
    if (m_projectTrustPromptedRoot == m_projectRoot) {
        return;
    }
    m_projectTrustPromptedRoot = m_projectRoot;
    auto* dialog = new ui::TrustDialog(*m_build, untrustedRoots, m_window, ui::TrustPurpose::ProjectOpen);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_projectTrustDialog = dialog;
    const std::string root = m_projectRoot;
    connect(dialog, &QDialog::accepted, this, [this, root] {
        m_projectTrustDialog.clear();
        if (m_projectRoot != root || !m_services.trust.isTrusted(root)) {
            return;
        }
        if (m_window) {
            m_window->setProjectTrusted(true);
        }
        if (m_solution) {
            startProjectEvaluation(root);
            if (m_clangd) {
                restartClangd();
            }
        } else if (m_cmake) {
            /* While a configure the grant resumed is running, nothing happens here: its model
             * publication restarts clangd once, trusted, on the database it writes, and the trust
             * watch covers an operation that publishes none once the service is idle (FND-9a: no
             * restart in between). */
            if (m_cmake->service().state() == build::BuildState::Idle) {
                if (!m_cmake->service().model()) {
                    // The configure's model publication restarts clangd if the database or trust moved.
                    (void)m_cmake->service().configure();
                } else {
                    // A clangd started restricted on an existing database restarts trusted (.clangd,
                    // --query-driver); without a running clangd this does nothing.
                    restartClangdIfCMakeStateChanged();
                }
            }
        }
        m_luaObservedTrust = true;
        startLua(root);
        startManaged(root);
        if (m_engine) {
            m_engine->trustChanged();
        }
    });
    connect(dialog, &QDialog::finished, this, [this, dialog](int) {
        if (m_projectTrustDialog == dialog) {
            m_projectTrustDialog.clear();
        }
        dialog->setObjectName(QString());
        dialog->deleteLater();
    });
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        dialog->setAttribute(Qt::WA_ShowWithoutActivating, true);
        dialog->show();
        if (m_window->isVisible()) {
            m_window->activateWindow();
        }
    } else {
        dialog->show();
        const auto centerAndActivate = [dialog = QPointer<ui::TrustDialog>(dialog),
                                        win = QPointer<ui::ShellWindow>(m_window)] {
            if (!dialog) {
                return;
            }
            if (win && win->isVisible()) {
                const QSize hint = dialog->sizeHint().expandedTo(dialog->minimumSize());
                const QRect parentFrame = win->frameGeometry();
                dialog->move(parentFrame.center().x() - hint.width() / 2, parentFrame.center().y() - hint.height() / 2);
            }
            dialog->show();
            dialog->raise();
            dialog->activateWindow();
        };
        if (m_window->isVisible()) {
            centerAndActivate();
        } else {
            QTimer::singleShot(0, dialog, centerAndActivate);
        }
    }
}

// NOLINTNEXTLINE(performance-unnecessary-value-param): Own identities across reentrant project callbacks.
void Workbench::setProject(std::string root, QString name, std::string recentPath) {
    invalidateManagedDebug();
    cancelEngineTestDebugPreparation();
    if (m_engineBindings)
        m_engineBindings->cancel();
    if (m_engineRunDebug)
        m_engineRunDebug->cancel();
    if (m_engineMixedLaunch)
        m_engineMixedLaunch->cancel();
    cancelFind();
    m_cppRenameStop.request_stop();
    ++m_cppRenameGeneration;
    m_cppRenameAdmission.reset();
    if (!m_resourceGovernor)
        m_resourceGovernor = std::make_shared<sched::ResourceGovernor>();
    m_cppInspectionStop.request_stop();
    ++m_cppInspectionGeneration;
    m_lastCppInspectionReport.reset();
    m_cppInspectionTrustRevision.reset();
    if (m_problems)
        m_problems->model()->setProblems(ui::DiagnosticSource::Inspection, {});
    if (m_debug)
        m_debug->cancelRerun();
    m_projectRoot = std::move(root);
    if (m_git)
        m_git->openProject(m_projectRoot);
    if (m_debug) {
        if (auto loaded = m_debug->setWorkspaceRoot(m_projectRoot); !loaded)
            log::warn(kLog, "debug breakpoints: {}", loaded.error().describe());
    }
    m_projectIdentity = recentPath;
    if (m_sessions) {
        if (auto saved = m_sessions->setLastProject(m_projectIdentity); !saved)
            log::warn(kLog, "last project: {}", saved.error().describe());
    }
    if (auto added = addRecentProject(m_services.settings, recentPath); !added) {
        log::warn(kLog, "recent projects: {}", added.error().describe());
    }
    refreshRecent();
    if (m_window) {
        m_window->setProject(name);
        m_window->tabs().setEmptyStateWidget(nullptr);
        m_window->refreshActions();
    }
    transitionLauncherToMainWindow();
    emit projectChanged(name);
}

bool Workbench::closeForOpen() {
    if (m_solutionLoading) {
        m_solutionLoadStop.request_stop();
        return false; // Do not replace/close the current project while the modal worker owns its inputs.
    }
    if (m_projectEditWaitingForReaders || (m_projectEdit && m_projectEdit->busy())) {
        notify(text("The project stays open"), text("Wait for the project edit to finish."),
               ui::NotificationKind::Warning);
        return false;
    }
    m_language->structureChanged();
    cancelEngineTestDebugPreparation();
    invalidateManagedDebug();
    if (m_engineBindings)
        m_engineBindings->cancel();
    if (m_engineRunDebug)
        m_engineRunDebug->cancel();
    if (m_engineMixedLaunch)
        m_engineMixedLaunch->cancel();
    if (m_engineLuaDebug)
        m_engineLuaDebug->cancel();
    if (m_engine)
        m_engine->cancel();
    if (m_unity)
        m_unity->cancel();
    if (!hasProject()) {
        return true;
    }
    // Check the services themselves: the controller's queued state can lag a newly started build.
    // Keep the current project and its tabs intact until the user stops the build or run.
    if (anyDebugSessionActive() ||
        (m_solution && (m_buildService->state() != build::BuildState::Idle || !m_runService->activeRuns().empty())) ||
        (m_cmake && (m_cmake->service().state() != build::BuildState::Idle || m_cmake->running())) ||
        (m_standalone && m_standalone->state() != build::BuildState::Idle)) {
        notify(text("The project stays open"), text("Stop the build and running programs before closing the project."),
               ui::NotificationKind::Warning);
        return false;
    }
    // A save-failure prompt can run a nested event loop, so cancel a queued rerun before
    // closing any tab. A cancelled close also leaves that rerun cancelled.
    if (m_debug)
        m_debug->cancelRerun();
    // Stop background operations before closing project documents.
    cancelFind();
    if (m_git)
        m_git->closeProject();
    // The session is saved and every tab saved and closed; a refusal keeps everything open.
    if (m_nav) {
        if (!m_nav->closeProject()) {
            if (m_git)
                m_git->openProject(m_projectRoot);
            return false;
        }
    } else if (m_window) {
        m_window->tabs().closeAll();
        if (m_window->tabs().totalCount() > 0) {
            if (m_git)
                m_git->openProject(m_projectRoot);
            return false;
        }
    }
    m_closingSolution = true;
    m_cppRenameStop.request_stop();
    ++m_cppRenameGeneration;
    m_cppRenameAdmission.reset();
    m_cppInspectionStop.request_stop();
    ++m_cppInspectionGeneration;
    m_lastCppInspectionReport.reset();
    m_cppInspectionTrustRevision.reset();
    if (m_problems)
        m_problems->model()->setProblems(ui::DiagnosticSource::Inspection, {});
    delete m_projectTrustDialog.data();
    m_projectTrustDialog.clear();
    m_projectTrustPromptedRoot.clear();
    stopLua();
    stopManaged();
    stopClangd();
    stopProjectEvaluation();
    if (m_cmake) {
        m_build->setBackend(m_buildBackend.get());
        m_cmake.reset();
    }
    if (m_standalone) {
        m_build->setBackend(m_buildBackend.get());
        m_standalone.reset();
    }
    if (m_solution) {
        if (m_buildService->projectEditInProgress())
            m_buildService->cancelProjectEdit(); // The unresolved on-disk journal remains untouched.
        if (auto closed = m_build->closeSolution(); !closed) {
            log::warn(kLog, "closing the solution: {}", closed.error().describe());
        }
        m_runService->stopAll();
        if (auto closed = m_runService->closeSolution(); !closed) {
            log::warn(kLog, "closing the solution's runs: {}", closed.error().describe());
        }
        m_runController->refreshStartupCombo();
        if (m_explorer) {
            m_explorer->setSolution(nullptr);
        }
    }
    m_solution.reset();
    m_unloadedProjectPaths.clear();
    m_closingSolution = false;
    if (m_debug)
        (void)m_debug->setWorkspaceRoot({});
    m_projectRoot.clear();
    m_projectIdentity.clear();
    if (m_window) {
        m_window->setProject(QString());
        m_window->tabs().setEmptyStateWidget(m_welcome);
        m_window->refreshActions();
    }
    refreshRecent();
    emit projectChanged(QString());
    return true;
}

bool Workbench::closeProject() {
    if (!hasProject()) {
        return true;
    }
    if (!closeForOpen()) {
        return false;
    }
    // Closed on purpose: the next start shows the Welcome screen (Rider), not this project.
    if (m_sessions) {
        if (auto cleared = m_sessions->clearLastProject(); !cleared) {
            log::warn(kLog, "last project: {}", cleared.error().describe());
        }
    }
    if (m_launcherModeEnabled && m_launcherWindow) {
        showLauncherWindow();
    }
    return true;
}

bool Workbench::restoreLastProject() {
    if (!m_nav || !m_services.settings.snapshot()->get(settings::key::generalReopenLastProject)) {
        return false;
    }
    return m_nav->restoreLastProject();
}

Result<void> Workbench::openProjectIdentity(const std::string& path) {
    const auto marker = io::fileName(path);
    if (marker == "compile_commands.json" || marker == "Makefile" || marker == "makefile" || marker == "GNUmakefile")
        return openFolder(std::string(io::parentPath(path)), false, path);
    return io::fileName(path) == "CMakeLists.txt" ? openCMakeProject(path) : openPath(path);
}

Result<void> Workbench::reopenRoot(const std::string& root) {
    const auto marker = io::fileName(root);
    if (marker == "compile_commands.json" || marker == "Makefile" || marker == "makefile" || marker == "GNUmakefile")
        return openProjectIdentity(root);
    if (io::fileName(root) == "CMakeLists.txt")
        return openCMakeProject(root);
    if (isSolutionFile(root) || extensionOf(root) == ".frostproj")
        return openPath(root);
    // Older session stores keep roots; the recent projects say which solution in it the user opened.
    const std::string key = lang::documentKey(root);
    for (const std::string& recent : recentProjects(*m_services.settings.snapshot())) {
        const std::string recentRoot =
            isSolutionFile(recent) || extensionOf(recent) == ".frostproj" || io::fileName(recent) == "CMakeLists.txt" ||
                    io::fileName(recent) == "compile_commands.json" || io::fileName(recent) == "Makefile" ||
                    io::fileName(recent) == "makefile" || io::fileName(recent) == "GNUmakefile"
                ? std::string(io::parentPath(recent))
                : m_documentRegistry->canonicalize(recent);
        if (lang::documentKey(m_documentRegistry->canonicalize(recentRoot)) == key) {
            return openProjectIdentity(recent);
        }
    }
    return openPath(root);
}

} // namespace frost::app
