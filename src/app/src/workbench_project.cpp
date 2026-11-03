// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp), the project side: opening solutions, folders and files,
// closing the project, the last project at start-up, the recent-projects list, and clangd (start
// on solutionCompileCommandsChain, restart on a configuration switch, document sync, live
// diagnostics in Problems and the editors, the "clangd: indexing" background task).

#include "frost/app/workbench.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/compile_commands.hpp"
#include "frost/lang/msbuild_compile_commands.hpp"
#include "frost/lang/msvc_environment.hpp"
#include "frost/lang/uri.hpp"
#include "frost/sched/lane.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/run_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/tr.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "project_edit_process.hpp"
#include "unity_control.hpp"
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <functional>
#include <limits>
#include <utility>

namespace frost::app {

namespace {

constexpr std::string_view kLog = "workbench";

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

// The lower-cased extension of the last component, with its dot ("" for none).
std::string extensionOf(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {};
    }
    std::string out(name.substr(dot));
    std::ranges::transform(out, out.begin(),
                           [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
    return out;
}

// A database directory name for `root`: its last component and a hash of its document key.
std::string cacheNameFor(std::string_view root) {
    const std::size_t slash = root.find_last_of("/\\");
    std::string name(slash == std::string_view::npos ? root : root.substr(slash + 1));
    std::ranges::replace_if(name, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)); }, '_');
    return std::format("{}-{:016x}", name.empty() ? std::string("root") : name,
                       std::hash<std::string>{}(lang::documentKey(root)));
}

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
    if (status->type != io::FileType::Directory)
        return fail(ErrorCode::InvalidArgument, "--project requires a directory or .frostproj file");
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
    if (m_solution && lang::documentKey(m_solution->path()) == lang::documentKey(path)) {
        return {}; // Already the project.
    }
    Result<project::SolutionModel> loaded = project::SolutionModel::load(m_services.fs, path);
    if (!loaded) {
        notify(text("Cannot open %1").arg(qs(path)), qs(loaded.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(loaded.error()));
    }
    if (!closeForOpen()) {
        return fail(ErrorCode::Cancelled, "the current project could not be closed");
    }
    auto model = std::make_shared<const project::SolutionModel>(std::move(*loaded));
    m_solution = model;
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
    startClangd(model->directory());
    startLua(model->directory());
    startManaged(model->directory());
    refreshAllMarkers();
    return {};
}

Result<void> Workbench::openFolder(const std::string& path, bool discoverSolution, std::string recentPath) {
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
    if (m_projectRoot == path && !m_solution) {
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
        return {};
    }
    if (!closeForOpen()) {
        return fail(ErrorCode::Cancelled, "the current project could not be closed");
    }
    const auto lists = io::joinPath(path, "CMakeLists.txt");
    if (m_window && lists && m_services.fs.exists(*lists).value_or(false)) {
        m_cmake = std::make_unique<CMakeIntegration>(
            m_services.fs, m_services.launcher, m_services.trust, m_services.settings, *m_build, *m_window,
            *m_runConsole, m_options.cmakeStatePath, [this] { m_runController->alternateStateChanged(); },
            [this](const std::string& file) { (void)openPath(file); },
            [this] {
                if (m_cmake && m_clangd)
                    restartClangd();
            });
        if (auto opened = m_cmake->open(path); !opened) {
            m_cmake.reset();
            return std::unexpected(std::move(opened.error()));
        }
    }
    if (m_nav) {
        if (auto opened = m_nav->openProject(path); !opened) {
            log::warn(kLog, "navigation: {}", opened.error().describe());
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
    startClangd(path);
    startLua(path);
    startManaged(path);
    return {};
}

void Workbench::setProject(std::string root, QString name, std::string recentPath) {
    invalidateManagedDebug();
    cancelFind();
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
    emit projectChanged(name);
}

bool Workbench::closeForOpen() {
    if (m_projectEdit && m_projectEdit->busy()) {
        notify(text("The project stays open"), text("Wait for the project edit to finish."),
               ui::NotificationKind::Warning);
        return false;
    }
    m_language->structureChanged();
    invalidateManagedDebug();
    if (m_engine)
        m_engine->cancel();
    if (m_unity)
        m_unity->cancel();
    if (!hasProject()) {
        return true;
    }
    // Check the services themselves: the controller's queued state can lag a newly started build.
    // Keep the current project and its tabs intact until the user stops the build or run.
    if ((m_debug && m_debug->debugging()) ||
        (m_solution && (m_buildService->state() != build::BuildState::Idle || !m_runService->activeRuns().empty())) ||
        (m_cmake && (m_cmake->service().state() != build::BuildState::Idle || m_cmake->running()))) {
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
        if (m_window->tabs().count() > 0) {
            if (m_git)
                m_git->openProject(m_projectRoot);
            return false;
        }
    }
    stopLua();
    stopManaged();
    stopClangd();
    if (m_cmake) {
        m_build->setBackend(m_buildBackend.get());
        m_cmake.reset();
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
    return true;
}

bool Workbench::restoreLastProject() {
    if (!m_nav || !m_services.settings.snapshot()->get(settings::key::generalReopenLastProject)) {
        return false;
    }
    return m_nav->restoreLastProject();
}

Result<void> Workbench::openProjectIdentity(const std::string& path) {
    return io::fileName(path) == "CMakeLists.txt" ? openCMakeProject(path) : openPath(path);
}

Result<void> Workbench::reopenRoot(const std::string& root) {
    if (io::fileName(root) == "CMakeLists.txt")
        return openCMakeProject(root);
    if (isSolutionFile(root) || extensionOf(root) == ".frostproj")
        return openPath(root);
    // Older session stores keep roots; the recent projects say which solution in it the user opened.
    const std::string key = lang::documentKey(root);
    for (const std::string& recent : recentProjects(*m_services.settings.snapshot())) {
        const std::string recentRoot =
            isSolutionFile(recent) || extensionOf(recent) == ".frostproj" || io::fileName(recent) == "CMakeLists.txt"
                ? std::string(io::parentPath(recent))
                : m_documentRegistry->canonicalize(recent);
        if (lang::documentKey(m_documentRegistry->canonicalize(recentRoot)) == key) {
            return openProjectIdentity(recent);
        }
    }
    return openPath(root);
}

// --- clangd ---

void Workbench::startClangd(const std::string& root) {
    stopClangd();
    if (!m_options.clangd) {
        return;
    }
    const auto snapshot = m_services.settings.snapshot();
    Result<lang::ClangdLocation> found =
        lang::findClangd(m_services.fs, lang::defaultClangdSearch(lang::configuredClangdPath(*snapshot)));
    if (!found) {
        log::info(kLog, "no clangd, code insight is off: {}", found.error().describe());
        return;
    }
    if (!m_reactor) {
        Result<std::unique_ptr<io::Reactor>> reactor = io::Reactor::create(io::ReactorOptions{1, "frost-clangd-io"});
        if (!reactor) {
            notify(text("clangd cannot start"), qs(reactor.error().message()), ui::NotificationKind::Warning);
            return;
        }
        m_reactor = std::move(*reactor);
    }

    // ADR 0041's order: an existing compile_commands.json, then MSBuild's evaluation (a trusted
    // solution, Windows), then the commands synthesized from the projects.
    std::optional<std::string> cache;
    if (!m_options.compileCommandsCache.empty()) {
        if (Result<std::string> joined = io::joinPath(m_options.compileCommandsCache, cacheNameFor(root)); joined) {
            cache = std::move(*joined);
        }
    }
    if (m_cmake) {
        std::weak_ptr<build::CMakeService> service = m_cmake->sharedService();
        m_compileCommands = std::make_shared<lang::BuildDirectoryCompileCommandsProvider>(m_services.fs, [service] {
            const auto current = service.lock();
            return current ? current->activeBuildDirectory() : std::string();
        });
    } else if (m_solution && cache) {
        lang::MsBuildProviderOptions options;
        if (const std::optional<std::size_t> active = m_build->activeConfigurationIndex();
            active && *active < m_build->configurations().size()) {
            options.configuration = m_build->configurations()[*active].name();
        }
        options.msbuildPath =
            !m_options.msbuildPath.empty() ? m_options.msbuildPath : snapshot->get(settings::key::toolsMsbuildPath);
        options.locateMsBuild = [launcher = &m_services.launcher, fs = &m_services.fs]() -> Result<std::string> {
            Result<build::VsInstance> instance = build::findMsBuild(*launcher, *fs);
            if (!instance) {
                return std::unexpected(std::move(instance.error()));
            }
            if (instance->msbuildPath.empty()) {
                return fail(ErrorCode::NotFound, instance->displayName + " has no MSBuild.exe");
            }
            return instance->msbuildPath;
        };
        options.trusted = [trust = &m_services.trust, root] { return trust->isTrusted(root); };
        options.fallback.configuration = options.configuration;
        if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
            options.msvc = [launcher = &m_services.launcher, fs = &m_services.fs] {
                return lang::findMsvcEnvironment(*launcher, *fs);
            };
        }
        m_compileCommands = lang::solutionCompileCommandsChain(m_services.launcher, m_services.fs, m_solution, root,
                                                               *cache, std::move(options));
    } else {
        m_compileCommands = std::make_shared<lang::ExistingCompileCommandsProvider>(m_services.fs, root);
    }

    lang::ClangdServiceOptions options;
    options.clangdPath = found->path;
    options.workspaceRoot = root;
    options.launch.jobs = m_options.clangdJobs;
    options.compileCommands = m_compileCommands;
    // The owned server supervisors are joined before Workbench's QObject base is destroyed;
    // this worker callback only posts, and the queued GUI callback rechecks the controller.
    options.onLifecycle = [self = QPointer<Workbench>(this)] {
        if (self)
            QMetaObject::invokeMethod(
                self.data(),
                [self] {
                    if (self && self->m_language)
                        self->m_language->structureChanged();
                },
                Qt::QueuedConnection);
    };
    // Called on clangd's threads; the queued calls die with this object.
    options.onProgress = [this](const lang::ClangdProgress& progress) {
        QMetaObject::invokeMethod(this, [this, progress] { onClangdProgress(progress); }, Qt::QueuedConnection);
    };
    m_clangd = std::make_shared<lang::ClangdService>(m_services.launcher, io::AsyncContext{*m_reactor, *m_pool},
                                                     std::move(options));
    m_clangdListener = m_clangd->diagnostics().addListener([this](const lang::FileDiagnostics& diagnostics) {
        QMetaObject::invokeMethod(
            this, [this, file = diagnostics.file] { onLiveDiagnostics(file); }, Qt::QueuedConnection);
    });
    if (auto started = m_clangd->start(); !started) {
        notify(text("clangd cannot start"), qs(started.error().message()), ui::NotificationKind::Warning);
        stopClangd();
        return;
    }
    m_bridge->setServer(m_clangd);
    for (DocumentEditorView* view : m_editors->views()) {
        syncClangdDocument(*view);
        if (isCFamilySource(view->path()) && view == currentView()) {
            auto* binding = new ui::lang::EditorInsightBinding(view->editor(), {*m_bridge, *m_pool});
            binding->attach(*m_clangd);
        }
    }
    m_language->structureChanged();
}

void Workbench::restartClangd() {
    // A configuration switch: other flags, so another database (the chain regenerates it).
    if (hasProject()) {
        log::info(kLog, "the solution configuration changed: clangd restarts");
        startClangd(m_projectRoot);
    }
}

void Workbench::stopClangd() {
    if (!m_clangd) {
        return;
    }
    m_language->structureChanged();
    for (DocumentEditorView* view : m_editors->views()) {
        if (isCFamilySource(view->path()))
            delete view->editor().findChild<ui::lang::EditorInsightBinding*>();
    }
    m_bridge->setServer(nullptr);
    m_clangd->diagnostics().removeListener(m_clangdListener);
    m_clangdListener = 0;
    m_pool->spawn(sched::Lane::UserVisible, m_clangd->stop()).join();
    m_clangd.reset();
    m_compileCommands.reset();
    if (m_indexing) {
        m_indexing->finish();
        m_indexing.reset();
    }
    m_indexToken.clear();
    for (DocumentEditorView* view : m_editors->views()) {
        if (!isCFamilySource(view->path()))
            continue;
        m_liveMarkers.erase(lang::documentKey(view->path()));
        // Without clangd the editors show no live analysis: no squiggles, usages or inspection widget.
        view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, {});
        view->editor().setUsageHighlights({});
        const auto version = view->editor().editor().text().version();
        (void)view->editor().setSemanticTokens(version, {});
        (void)view->editor().setInactiveRanges(version, {});
        view->editor().setInsightEnabled(false);
        refreshMarkers(*view);
    }
    refreshLiveProblems();
}

void Workbench::syncClangdDocument(DocumentEditorView& view) {
    if (!m_clangd || !isCFamilySource(view.path())) {
        return;
    }
    const text::Document& document = view.document().document();
    if (auto opened = m_clangd->openDocument(view.path(), document.text()); opened) {
        return;
    }
    // Already open: a ChangeSet that continues from no known version sends the whole text.
    text::ChangeSet whole;
    whole.baseVersion = std::numeric_limits<std::uint64_t>::max();
    whole.version = document.version();
    (void)m_clangd->changeDocument(view.path(), whole, document.text());
}

void Workbench::onLiveDiagnostics(const std::string& file) {
    if (!m_clangd) {
        return;
    }
    const std::optional<lang::FileDiagnostics> set = m_clangd->diagnostics().forFile(file);
    if (!set || !set->stale) {
        // A stale publish (for a text the user has typed past) waits for the next one.
        const std::string key = lang::documentKey(file);
        std::vector<ui::LineMarker> markers;
        if (set) {
            for (const lang::LiveDiagnostic& d : set->diagnostics) {
                if (d.severity != lang::DiagnosticSeverity::Error && d.severity != lang::DiagnosticSeverity::Warning) {
                    continue;
                }
                const ui::MarkerSeverity severity = d.severity == lang::DiagnosticSeverity::Error
                                                        ? ui::MarkerSeverity::Error
                                                        : ui::MarkerSeverity::Warning;
                if (d.line > 0) {
                    markers.push_back(
                        ui::LineMarker{LineIndex{static_cast<std::uint32_t>(d.line - 1)}, severity, d.message});
                }
            }
        }
        if (markers.empty()) {
            m_liveMarkers.erase(key);
        } else {
            m_liveMarkers[key] = std::move(markers);
        }
        if (DocumentEditorView* view = m_editors->find(file); view != nullptr) {
            // The squiggles: every live diagnostic (editor-insight's translation; ranges of the
            // published text version, mapped by the view through the edits made since).
            view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, set ? ui::lang::toEditorDiagnostics(*set)
                                                                                : std::vector<ui::EditorDiagnostic>{});
            refreshMarkers(*view);
        }
    }
    refreshLiveProblems();
}

void Workbench::refreshLiveProblems() {
    if (!m_problems) {
        return;
    }
    std::vector<build::BuildDiagnostic> rows;
    std::vector<lang::LiveDiagnostic> live;
    if (m_clangd)
        live = m_clangd->diagnostics().problems();
    if (m_lua) {
        auto lua = m_lua->diagnostics().problems();
        live.insert(live.end(), lua.begin(), lua.end());
    }
    for (const lang::LiveDiagnostic& d : live) {
        build::BuildDiagnostic row;
        row.diagnostic.severity = d.severity == lang::DiagnosticSeverity::Error     ? build::Severity::Error
                                  : d.severity == lang::DiagnosticSeverity::Warning ? build::Severity::Warning
                                                                                    : build::Severity::Message;
        row.diagnostic.code = d.code;
        row.diagnostic.message = d.message;
        row.diagnostic.file = d.file;
        row.diagnostic.line = d.line;
        row.diagnostic.column = d.column;
        row.path = d.file;
        row.jumpable = !d.file.empty() && d.line > 0;
        rows.push_back(std::move(row));
    }
    m_problems->model()->setProblems(ui::DiagnosticSource::Live, rows);
}

void Workbench::onClangdProgress(const lang::ClangdProgress& progress) {
    if (!m_clangd || !m_window) {
        return;
    }
    // clangd's background index reports as the work-done progress titled "indexing".
    const bool indexing = progress.token == m_indexToken || (progress.kind == lang::ClangdProgress::Kind::Begin &&
                                                             progress.title.find("index") != std::string::npos);
    if (!indexing) {
        return;
    }
    switch (progress.kind) {
    case lang::ClangdProgress::Kind::Begin:
        if (m_indexing) {
            m_indexing->finish();
        }
        m_indexToken = progress.token;
        m_indexing = m_window->backgroundTasks().start(text("clangd: indexing"), false);
        [[fallthrough]];
    case lang::ClangdProgress::Kind::Report:
        if (m_indexing) {
            if (progress.percentage) {
                m_indexing->setProgress(static_cast<double>(*progress.percentage) / 100.0);
            }
            if (!progress.message.empty()) {
                m_indexing->setDetail(qs(progress.message));
            }
        }
        return;
    case lang::ClangdProgress::Kind::End:
        if (m_indexing) {
            m_indexing->finish();
            m_indexing.reset();
        }
        m_indexToken.clear();
        return;
    }
}

} // namespace frost::app
