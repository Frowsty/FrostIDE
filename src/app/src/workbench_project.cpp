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
#include "frost/project/cmake_syntax.hpp"
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
#include "frost/ui/workbench/file_system_view.hpp"

#include "cmake_integration.hpp"
#include "engine_integration.hpp"
#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "project_edit_process.hpp"
#include "unity_control.hpp"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

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

// Retain the service until its supervisor and process have finished stopping.
sched::Task<void> stopOwnedClangd(std::shared_ptr<lang::ClangdService> service, sched::Task<void> stopped) {
    (void)service;
    co_await std::move(stopped);
}

sched::Task<void> stopOwnedProjectEvaluation(std::shared_ptr<lang::IndependentProjectEvaluation> service) {
    service->stopAndJoin();
    co_return;
}

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

bool isWindowsReservedDeviceName(std::string_view component) {
    const std::size_t dot = component.find('.');
    const std::string_view stem = dot == std::string_view::npos ? component : component.substr(0, dot);
    if (stem.empty())
        return false;
    std::string upper(stem);
    std::ranges::transform(upper, upper.begin(),
                           [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; });
    static constexpr std::array<std::string_view, 22> kReserved{
        "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    return std::ranges::find(kReserved, std::string_view(upper)) != kReserved.end();
}

Result<void> validateProjectNameComponent(std::string_view name) {
    if (name.empty() || name == "." || name == "..") {
        return fail(ErrorCode::InvalidArgument, "enter a valid project name");
    }
    if (name.back() == ' ' || name.back() == '.') {
        return fail(ErrorCode::InvalidArgument, "a project name cannot end with a space or period");
    }
    for (const char raw : name) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch < 0x20U || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|' ||
            ch == '?' || ch == '*') {
            return fail(ErrorCode::InvalidArgument, "the project name contains invalid path characters");
        }
    }
    if (isWindowsReservedDeviceName(name)) {
        return fail(ErrorCode::InvalidArgument, "the project name is a reserved device name");
    }
    return {};
}

std::string sanitizeCMakeTargetName(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char raw : name) {
        const auto ch = static_cast<unsigned char>(raw);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
            ch == '-') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('_');
        }
    }
    if (out.empty() || out.front() == '-') {
        out.insert(0, "app_");
    }
    return out;
}

bool isCTranslationUnit(std::string_view path) noexcept {
    static constexpr std::array<std::string_view, 8> kTranslationUnits{".c",   ".cc",   ".cpp", ".cxx",
                                                                       ".c++", ".cppm", ".ixx", ".cu"};
    const std::string ext = extensionOf(path);
    return std::ranges::find(kTranslationUnits, ext) != kTranslationUnits.end();
}

Result<std::string> cmakeRelativeSourcePath(std::string_view projectRoot, std::string_view sourcePath) {
    if (!sourcePath.starts_with(projectRoot) || sourcePath.size() <= projectRoot.size() ||
        (sourcePath[projectRoot.size()] != '/' && sourcePath[projectRoot.size()] != '\\')) {
        return fail(ErrorCode::InvalidArgument, "the source file must be inside the CMake project directory");
    }
    std::string out(sourcePath.substr(projectRoot.size() + 1));
    std::ranges::replace(out, '\\', '/');
    while (out.starts_with('/')) {
        out.erase(out.begin());
    }
    if (out.empty() || out == "." || out == ".." || out.starts_with("../") || out.find("/../") != std::string::npos) {
        return fail(ErrorCode::InvalidArgument, "the source file must be inside the CMake project directory");
    }
    for (const char raw : out) {
        const auto ch = static_cast<unsigned char>(raw);
        if (ch <= 0x20U || ch == '"' || ch == '#' || ch == '$' || ch == '(' || ch == ')' || ch == ';') {
            return fail(ErrorCode::Unsupported,
                        "the source path contains characters that require manual CMakeLists.txt quoting");
        }
    }
    return out;
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
    startProjectEvaluation(model->directory());
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
            m_services.fs, m_services.launcher, m_services.trust, m_services.settings, *m_build, *m_buildService,
            *m_window, *m_runConsole, m_options.cmakeStatePath, [this] { m_runController->alternateStateChanged(); },
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
        if (m_window->tabs().totalCount() > 0) {
            if (m_git)
                m_git->openProject(m_projectRoot);
            return false;
        }
    }
    m_closingSolution = true;
    stopLua();
    stopManaged();
    stopClangd();
    stopProjectEvaluation();
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

// --- Project and file creation ---

void Workbench::showNewProjectDialog() {
    std::string defaultLocation;
    if (!m_projectRoot.empty()) {
        defaultLocation = std::string(io::parentPath(m_projectRoot));
    } else {
        QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        if (docs.isEmpty())
            docs = QDir::homePath();
        defaultLocation = QDir::toNativeSeparators(docs).toStdString();
    }
    const std::optional<NewProjectSpec> chosen = m_services.prompts.chooseNewProject(m_window, defaultLocation);
    if (!chosen)
        return;
    (void)createNewProject(*chosen);
}

Result<std::string> Workbench::createNewProject(const NewProjectSpec& spec) {
    if (Result<void> validName = validateProjectNameComponent(spec.name); !validName) {
        notify(text("Cannot create project"), qs(validName.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(validName.error()));
    }
    if (spec.location.empty() || !io::isAbsolutePath(spec.location)) {
        Error err(ErrorCode::InvalidArgument, "select an absolute location for the project");
        notify(text("Cannot create project"), qs(err.message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(err));
    }
    Result<std::string> normalizedLocation = io::normalizePath(spec.location);
    if (!normalizedLocation) {
        notify(text("Cannot create project"), qs(normalizedLocation.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(normalizedLocation.error()));
    }
    Result<std::string> targetDir = io::joinPath(*normalizedLocation, spec.name);
    if (!targetDir) {
        notify(text("Cannot create project"), qs(targetDir.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(targetDir.error()));
    }

    Result<bool> existed = m_services.fs.exists(*targetDir);
    if (!existed) {
        notify(text("Cannot create project"), qs(existed.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(existed.error()));
    }
    const bool dirExisted = *existed;
    if (dirExisted) {
        Result<io::FileStatus> status = m_services.fs.status(*targetDir);
        if (!status) {
            notify(text("Cannot create project"), qs(status.error().message()), ui::NotificationKind::Error);
            return std::unexpected(std::move(status.error()));
        }
        if (status->type != io::FileType::Directory) {
            Error err(ErrorCode::AlreadyExists, "a file already exists at " + *targetDir);
            notify(text("Cannot create project"), qs(err.message()), ui::NotificationKind::Error);
            return std::unexpected(std::move(err));
        }
        Result<std::vector<io::DirEntry>> entries = m_services.fs.listDirectory(*targetDir);
        if (!entries) {
            notify(text("Cannot create project"), qs(entries.error().message()), ui::NotificationKind::Error);
            return std::unexpected(std::move(entries.error()));
        }
        if (!entries->empty()) {
            Error err(ErrorCode::AlreadyExists, "the target folder is not empty: " + *targetDir);
            notify(text("Cannot create project"), qs(err.message()), ui::NotificationKind::Error);
            return std::unexpected(std::move(err));
        }
    } else {
        if (Result<void> created = m_services.fs.createDirectories(*targetDir); !created) {
            notify(text("Cannot create project"), qs(created.error().message()), ui::NotificationKind::Error);
            return std::unexpected(std::move(created.error()));
        }
    }

    const std::string canonicalDir = m_documentRegistry->canonicalize(*targetDir);
    if (spec.kind == NewProjectTemplate::EmptyFolder) {
        Result<void> opened = openFolder(canonicalDir, false);
        if (!opened) {
            return std::unexpected(std::move(opened.error()));
        }
        if (m_window) {
            m_window->toolWindows().show(ui::kFileSystemToolWindow, false);
        }
        return canonicalDir;
    }

    const std::string targetName = sanitizeCMakeTargetName(spec.name);
    Result<std::string> cmakeListsPath = io::joinPath(canonicalDir, "CMakeLists.txt");
    Result<std::string> mainCppPath = io::joinPath(canonicalDir, "main.cpp");
    if (!cmakeListsPath || !mainCppPath) {
        Error err(ErrorCode::InvalidArgument, "could not construct starter project file paths");
        notify(text("Cannot create project"), qs(err.message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(err));
    }

    const std::string cmakeContents = std::format("cmake_minimum_required(VERSION 3.20)\n"
                                                  "project({0} LANGUAGES CXX)\n"
                                                  "\n"
                                                  "set(CMAKE_CXX_STANDARD 20)\n"
                                                  "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
                                                  "set(CMAKE_CXX_EXTENSIONS OFF)\n"
                                                  "\n"
                                                  "add_executable({0}\n"
                                                  "    main.cpp\n"
                                                  ")\n",
                                                  targetName);
    const std::string mainContents = std::format("#include <iostream>\n"
                                                 "\n"
                                                 "int main() {{\n"
                                                 "    std::cout << \"Hello from {}!\\n\";\n"
                                                 "    return 0;\n"
                                                 "}}\n",
                                                 targetName);

    const auto cleanupCreated = [&] {
        (void)m_services.fs.removeFile(*mainCppPath);
        (void)m_services.fs.removeFile(*cmakeListsPath);
        if (!dirExisted) {
            (void)m_services.fs.removeDirectory(canonicalDir);
        }
    };

    if (Result<void> wroteLists = m_services.fs.writeFileAtomic(*cmakeListsPath, cmakeContents); !wroteLists) {
        cleanupCreated();
        notify(text("Cannot create project"), qs(wroteLists.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(wroteLists.error()));
    }
    if (Result<void> wroteMain = m_services.fs.writeFileAtomic(*mainCppPath, mainContents); !wroteMain) {
        cleanupCreated();
        notify(text("Cannot create project"), qs(wroteMain.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(wroteMain.error()));
    }

    Result<void> openedProject = openCMakeProject(*cmakeListsPath);
    if (!openedProject) {
        return std::unexpected(std::move(openedProject.error()));
    }
    if (m_window) {
        m_window->toolWindows().show(ui::kFileSystemToolWindow, false);
    }
    if (m_nav && m_nav->fileSystemView()) {
        m_nav->fileSystemView()->selectWhenAvailable(*mainCppPath);
    }
    (void)openPath(*mainCppPath);
    return canonicalDir;
}

Result<std::string> Workbench::createNewFile(std::string_view directory, std::string_view relativeName) {
    if (!m_nav) {
        return fail(ErrorCode::InvalidArgument, "the workbench has no window");
    }
    const std::string baseDir = directory.empty() ? defaultNewItemDirectory() : std::string(directory);
    return m_nav->createNewFile(baseDir, relativeName);
}

Result<std::string> Workbench::createNewFolder(std::string_view directory, std::string_view relativeName) {
    if (!m_nav) {
        return fail(ErrorCode::InvalidArgument, "the workbench has no window");
    }
    const std::string baseDir = directory.empty() ? defaultNewItemDirectory() : std::string(directory);
    return m_nav->createNewFolder(baseDir, relativeName);
}

Result<std::string> Workbench::createNewProjectSource(std::string_view projectPath, std::string_view relativeName) {
    if (!m_nav) {
        return fail(ErrorCode::InvalidArgument, "the workbench has no window");
    }
    const std::string canonicalProject = m_documentRegistry->canonicalize(projectPath);
    const std::string projectDir(io::parentPath(canonicalProject));
    m_suppressNewFileNotice = true;
    Result<std::string> created = m_nav->createNewFile(projectDir, relativeName);
    m_suppressNewFileNotice = false;
    if (!created) {
        return created;
    }
    removeProjectFile(qs(canonicalProject), qs(*created), true);
    return created;
}

void Workbench::onNewFileCreated(const std::string& path) {
    if (m_suppressNewFileNotice || !m_window || !isCTranslationUnit(path)) {
        return;
    }
    if (m_cmake) {
        ui::Notification notice;
        notice.title = text("Created %1").arg(qs(io::fileName(path)));
        notice.text = text("New C/C++ source files are not added to CMake targets automatically.");
        notice.kind = ui::NotificationKind::Info;
        notice.actions.push_back(ui::NotificationAction{
            text("Add to CMakeLists.txt"),
            [self = QPointer<Workbench>(this), path] {
                if (self)
                    (void)self->addSourceToCMakeLists(path);
            },
        });
        notice.actions.push_back(ui::NotificationAction{
            text("Open CMakeLists.txt"),
            [self = QPointer<Workbench>(this)] {
                if (!self || self->m_projectRoot.empty())
                    return;
                if (const auto lists = io::joinPath(self->m_projectRoot, "CMakeLists.txt"))
                    (void)self->openPath(*lists);
            },
        });
        m_window->notifications().notify(std::move(notice));
    }
}

Result<void> Workbench::addSourceToCMakeLists(std::string_view sourcePath, std::string_view targetName) {
    if (m_projectRoot.empty()) {
        return fail(ErrorCode::InvalidArgument, "no project is open");
    }
    Result<std::string> listsPath = io::joinPath(m_projectRoot, "CMakeLists.txt");
    if (!listsPath) {
        return std::unexpected(std::move(listsPath.error()));
    }
    const std::string canonicalLists = m_documentRegistry->canonicalize(*listsPath);
    if (!m_services.fs.exists(canonicalLists).value_or(false)) {
        return fail(ErrorCode::NotFound, "CMakeLists.txt was not found in the project root");
    }
    if (const document::FileDocument* openLists = m_documentRegistry->find(canonicalLists);
        openLists != nullptr && openLists->dirty()) {
        Error err(ErrorCode::Busy, "save or revert unsaved changes in CMakeLists.txt before adding a source file");
        notify(text("Cannot update CMakeLists.txt"), qs(err.message()), ui::NotificationKind::Warning);
        return std::unexpected(std::move(err));
    }

    const std::string canonicalRoot = m_documentRegistry->canonicalize(m_projectRoot);
    const std::string canonicalSource = m_documentRegistry->canonicalize(sourcePath);
    if (!m_services.fs.exists(canonicalSource).value_or(false)) {
        return fail(ErrorCode::NotFound, "source file does not exist: " + canonicalSource);
    }
    Result<std::string> relSource = cmakeRelativeSourcePath(canonicalRoot, canonicalSource);
    if (!relSource) {
        notify(text("Manual CMakeLists.txt edit required"), qs(relSource.error().message()),
               ui::NotificationKind::Warning);
        return std::unexpected(std::move(relSource.error()));
    }

    Result<std::string> content = m_services.fs.readFile(canonicalLists);
    if (!content) {
        notify(text("Cannot read CMakeLists.txt"), qs(content.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(content.error()));
    }

    Result<std::vector<project::CMakeSyntaxCommand>> commands = project::scanCMakeSyntax(*content);
    if (!commands) {
        notify(text("Manual CMakeLists.txt edit required"), qs(commands.error().message()),
               ui::NotificationKind::Warning);
        return std::unexpected(std::move(commands.error()));
    }

    std::vector<const project::CMakeSyntaxCommand*> candidates;
    for (const project::CMakeSyntaxCommand& cmd : *commands) {
        if (cmd.scopeDepth != 0 || cmd.nestedArguments || cmd.name == "add_subdirectory" || cmd.name == "include") {
            Error err(ErrorCode::Unsupported,
                      "CMakeLists.txt uses conditional or multi-directory commands; update it manually");
            notify(text("Manual CMakeLists.txt edit required"), qs(err.message()), ui::NotificationKind::Warning);
            return std::unexpected(std::move(err));
        }
        if (cmd.name != "add_executable" && cmd.name != "add_library") {
            continue;
        }
        if (cmd.arguments.empty() || !std::ranges::all_of(cmd.arguments, &project::CMakeSyntaxArgument::isLiteral)) {
            Error err(ErrorCode::Unsupported,
                      "CMake target declaration uses non-literal arguments; update CMakeLists.txt manually");
            notify(text("Manual CMakeLists.txt edit required"), qs(err.message()), ui::NotificationKind::Warning);
            return std::unexpected(std::move(err));
        }
        const bool isImportedOrAlias = std::ranges::any_of(cmd.arguments, [](const project::CMakeSyntaxArgument& arg) {
            return arg.literal == "IMPORTED" || arg.literal == "ALIAS" || arg.literal == "INTERFACE";
        });
        if (isImportedOrAlias) {
            continue;
        }
        if (!targetName.empty() && cmd.arguments.front().literal != targetName) {
            continue;
        }
        candidates.push_back(&cmd);
    }

    if (candidates.size() != 1) {
        Error err(ErrorCode::Unsupported, "CMakeLists.txt does not have a single simple top-level C/C++ target");
        notify(text("Manual CMakeLists.txt edit required"), qs(err.message()), ui::NotificationKind::Warning);
        return std::unexpected(std::move(err));
    }

    const project::CMakeSyntaxCommand* selected = candidates.front();
    for (std::size_t i = 1; i < selected->arguments.size(); ++i) {
        if (selected->arguments[i].literal == *relSource) {
            return {};
        }
    }

    const std::string_view newline = content->find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::size_t firstArgBegin = selected->arguments.front().begin;
    const std::size_t closePos = selected->close;
    const std::string_view spanView = std::string_view(*content).substr(firstArgBegin, closePos - firstArgBegin);
    const bool multiline = spanView.find('\n') != std::string_view::npos;

    std::string updated = *content;
    if (multiline) {
        std::size_t lineStart = closePos;
        while (lineStart > 0 && (updated[lineStart - 1] == ' ' || updated[lineStart - 1] == '\t')) {
            --lineStart;
        }
        if (lineStart > 0 && updated[lineStart - 1] == '\n') {
            updated.insert(lineStart, std::format("    {}{}", *relSource, newline));
        } else {
            updated.insert(closePos, std::format("{}    {}{}", newline, *relSource, newline));
        }
    } else {
        updated.insert(closePos, std::format(" {}", *relSource));
    }

    if (Result<std::vector<project::CMakeSyntaxCommand>> verified = project::scanCMakeSyntax(updated); !verified) {
        return std::unexpected(std::move(verified.error()));
    }
    if (Result<io::SafeWriteResult> written = m_services.fs.safeWrite(canonicalLists, updated, {}); !written) {
        notify(text("Cannot update CMakeLists.txt"), qs(written.error().message()), ui::NotificationKind::Error);
        return std::unexpected(std::move(written.error()));
    }
    if (document::FileDocument* openLists = m_documents->find(canonicalLists); openLists != nullptr) {
        (void)m_documents->checkDisk(*openLists);
        m_documentRegistry->notifyChanged(canonicalLists);
    }
    notify(text("CMakeLists.txt updated"),
           text("Added %1 to target %2.").arg(qs(*relSource), qs(selected->arguments.front().literal)),
           ui::NotificationKind::Info);
    return {};
}

// --- clangd ---

void Workbench::stopProjectEvaluation() {
    if (m_streamedProjects) {
        m_streamedProjects->invalidate();
        m_streamedProjects.reset();
    }
    std::erase_if(m_retiredProjectEvaluations, [](auto& stopped) {
        if (!stopped.ready())
            return false;
        stopped.join();
        return true;
    });
    if (!m_projectEvaluation)
        return;
    auto old = std::move(m_projectEvaluation);
    old->requestStop();
    m_retiredProjectEvaluations.push_back(
        m_pool->spawn(sched::Lane::Background, stopOwnedProjectEvaluation(std::move(old))));
}

void Workbench::startProjectEvaluation(const std::string& root) {
    if (m_explorer) {
        m_explorer->endSpeedSearch();
        if (m_solution)
            m_explorer->setSolution(m_solution);
    }
    stopProjectEvaluation();
    const auto admission = m_services.trust.snapshot(root);
    m_projectTrustRevision = admission.revision;
    if (!m_solution || !admission.trusted || m_options.compileCommandsCache.empty())
        return;
    auto cache = io::joinPath(m_options.compileCommandsCache, cacheNameFor(root));
    if (!cache)
        return;
    m_streamedProjects = std::make_shared<lang::StreamedProjectContexts>(m_commandsRetirement, m_solution);
    lang::MsBuildProviderOptions options;
    options.retirement = m_commandsRetirement;
    options.streamedProjects = m_streamedProjects;
    if (const auto active = m_build->activeConfigurationIndex(); active && *active < m_build->configurations().size())
        options.configuration = m_build->configurations()[*active].name();
    const auto settings = m_services.settings.snapshot();
    options.msbuildPath =
        !m_options.msbuildPath.empty() ? m_options.msbuildPath : settings->get(settings::key::toolsMsbuildPath);
    options.locateMsBuild = [launcher = &m_services.launcher, fs = &m_services.fs]() -> Result<std::string> {
        auto instance = build::findMsBuild(*launcher, *fs);
        if (!instance)
            return std::unexpected(std::move(instance.error()));
        if (instance->msbuildPath.empty())
            return fail(ErrorCode::NotFound, instance->displayName + " has no MSBuild.exe");
        return instance->msbuildPath;
    };
    options.trusted = [trust = &m_services.trust, root, revision = admission.revision] {
        const auto current = trust->snapshot(root);
        return current.trusted && current.revision == revision;
    };
    options.fallback.configuration = options.configuration;
    m_projectEvaluation = std::make_shared<lang::IndependentProjectEvaluation>(m_services.launcher, m_services.fs,
                                                                               m_solution, *cache, std::move(options));
}

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
        options.retirement = m_commandsRetirement;
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
        const auto admission = m_services.trust.snapshot(root);
        options.trusted = [trust = &m_services.trust, root, revision = admission.revision] {
            const auto current = trust->snapshot(root);
            return current.trusted && current.revision == revision;
        };
        options.fallback.configuration = options.configuration;
        if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
            options.msvc = [launcher = &m_services.launcher, fs = &m_services.fs] {
                return lang::findMsvcEnvironment(*launcher, *fs);
            };
        }
        m_compileCommands = lang::solutionCompileCommandsChain(m_services.launcher, m_services.fs, m_solution, root,
                                                               *cache, std::move(options), m_projectEvaluation);
    } else {
        m_compileCommands = std::make_shared<lang::ExistingCompileCommandsProvider>(m_services.fs, root);
    }

    lang::ClangdServiceOptions options;
    options.clangdPath = found->path;
    options.workspaceRoot = root;
    options.launch.jobs = m_options.clangdJobs;
    options.compileCommands = m_compileCommands;
    const auto generation = m_clangdGeneration;
    // The owned server supervisors are joined before Workbench's QObject base is destroyed;
    // this worker callback only posts, and the queued GUI callback rechecks the controller.
    options.onLifecycle = [self = QPointer<Workbench>(this), generation] {
        if (self)
            QMetaObject::invokeMethod(
                self.data(),
                [self, generation] {
                    if (self && self->m_clangdGeneration == generation && self->m_language)
                        self->m_language->structureChanged();
                },
                Qt::QueuedConnection);
    };
    // Called on clangd's threads; the queued calls die with this object.
    options.onProgress = [this, generation](const lang::ClangdProgress& progress) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, progress] {
                if (generation == m_clangdGeneration)
                    onClangdProgress(progress);
            },
            Qt::QueuedConnection);
    };
    m_clangd = std::make_shared<lang::ClangdService>(m_services.launcher, io::AsyncContext{*m_reactor, *m_pool},
                                                     std::move(options));
    m_clangdListener =
        m_clangd->diagnostics().addListener([this, generation](const lang::FileDiagnostics& diagnostics) {
            QMetaObject::invokeMethod(
                this,
                [this, generation, file = diagnostics.file] {
                    if (generation == m_clangdGeneration)
                        onLiveDiagnostics(file);
                },
                Qt::QueuedConnection);
        });
    if (auto started = m_clangd->start(); !started) {
        notify(text("clangd cannot start"), qs(started.error().message()), ui::NotificationKind::Warning);
        stopClangd();
        return;
    }
    m_bridge->setServer(m_clangd);
    for (DocumentEditorView* view : m_editors->uniqueDocumentViews())
        syncClangdDocument(*view);
    for (DocumentEditorView* view : m_editors->views()) {
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
    ++m_clangdGeneration;
    std::erase_if(m_retiredClangd, [](auto& stopped) {
        if (!stopped.ready())
            return false;
        stopped.join();
        return true;
    });
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
    auto old = std::move(m_clangd);
    auto stopped = old->stop(); // Signal even when the only pool worker is inside evaluation.
    m_retiredClangd.push_back(
        m_pool->spawn(sched::Lane::Background, stopOwnedClangd(std::move(old), std::move(stopped))));
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
        for (DocumentEditorView* view : m_editors->views()) {
            if (lang::documentKey(view->path()) != key)
                continue;
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
