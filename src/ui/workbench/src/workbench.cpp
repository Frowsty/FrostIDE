// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/ui/workbench/workbench.hpp): construction, project scanning and file watching,
// directory roles, settings files, notifications, autosave polling, and window event handling.

#include "frost/core/log.hpp"
#include "frost/input/keymap.hpp"
#include "frost/project/directory_roles.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/settings/project_layout.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/workbench/file_system_view.hpp"
#include "frost/ui/workbench/merge_view.hpp"

#include "workbench_internal.hpp"
#include <QCloseEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QListWidget>
#include <QMouseEvent>
#include <QTimer>

#include <format>
#include <mutex>

namespace frost::ui {

using workbench_detail::fromUtf8;
using workbench_detail::nativePath;
using workbench_detail::text;

/*
 * The watcher pump: a task on the scheduler awaits the watch's batches and posts each to the
 * workbench's thread. The mailbox's target is cleared (under its mutex) before the workbench goes,
 * so no batch is posted afterwards; batches already queued die with the workbench (their context
 * object).
 */
struct WatchMailbox {
    std::mutex mutex;
    Workbench* target = nullptr;

    void post(io::FileChangeBatch batch) {
        const std::scoped_lock lock(mutex);
        if (target != nullptr) {
            QMetaObject::invokeMethod(
                target, [target = target, batch = std::move(batch)] { target->applyFileChanges(batch); },
                Qt::QueuedConnection);
        }
    }
};

namespace {

sched::Task<nav::ProjectFiles> prepareInitialFiles(std::string root,
                                                   std::shared_ptr<const std::vector<std::string>> solutionFiles,
                                                   std::stop_token token) {
    nav::ProjectFiles files(std::move(root));
    if (token.stop_requested() || !files.setSolutionFilesCancellable(*solutionFiles, token))
        files = nav::ProjectFiles();
    solutionFiles.reset();
    co_return files;
}

// Enumeration and all path/index preparation run on the background lane.
sched::Task<Result<std::shared_ptr<const nav::ProjectFiles>>>
prepareProjectFiles(io::IFileSystem& fs, sched::IScheduler& scheduler, std::string root,
                    std::shared_ptr<const std::vector<std::string>> solutionFiles,
                    std::shared_ptr<const nav::ProjectFiles> previous, std::stop_token token,
                    std::shared_ptr<const project::DirectoryRoles> roles) {
    // Locals die on this worker; coroutine parameters survive until the GUI joins the frame.
    auto baseline = std::move(previous);
    nav::ProjectFiles files(root);
    if (!files.setSolutionFilesCancellable(*solutionFiles, token)) {
        files = nav::ProjectFiles();
        solutionFiles.reset();
        co_return fail(ErrorCode::Cancelled, "project scan cancelled");
    }
    solutionFiles.reset();
    {
        auto scan = co_await nav::scanProject(fs, scheduler, std::move(root), token, std::move(roles));
        if (!scan) {
            files = nav::ProjectFiles();
            co_return std::unexpected(std::move(scan.error()));
        }
        if (token.stop_requested() || !files.setScanCancellable(*scan, token)) {
            files = nav::ProjectFiles();
            scan = fail(ErrorCode::Cancelled, "project scan cancelled");
            co_return std::unexpected(std::move(scan.error()));
        }
        // Drop the raw scan on this worker before the result reaches the GUI.
    }
    // Watcher traffic (including ignored build/log files) need not change the visible tree.
    // Compare on this worker, then preserve snapshot identity so the GUI need not reset.
    const auto sameNode = [&token](const nav::ProjectFiles::Node& a, const nav::ProjectFiles::Node& b) {
        return !token.stop_requested() && a.name == b.name && a.path == b.path && a.relative == b.relative &&
               a.directory == b.directory && a.parent == b.parent && a.children == b.children && a.row == b.row;
    };
    const auto samePath = [&token](const std::string& a, const std::string& b) {
        return !token.stop_requested() && a == b;
    };
    const bool unchanged = baseline && files.root() == baseline->root() &&
                           std::ranges::equal(files.nodes(), baseline->nodes(), sameNode) &&
                           std::ranges::equal(files.candidates(), baseline->candidates(), samePath) &&
                           std::ranges::equal(files.displayCandidates(), baseline->displayCandidates(), samePath);
    if (token.stop_requested())
        co_return fail(ErrorCode::Cancelled, "project scan cancelled");
    if (unchanged)
        co_return std::move(baseline);
    co_return std::make_shared<const nav::ProjectFiles>(std::move(files));
}

sched::Task<Result<void>> pumpWatch(io::IFileWatcher& watcher, std::string root, std::shared_ptr<WatchMailbox> mailbox,
                                    std::shared_ptr<const project::DirectoryRoles> roles, std::stop_token token) {
    io::WatchOptions options;
    // Nothing below a Generated or Excluded folder (build trees, download caches, .git) is watched
    // or reported (UX-23); on Linux those folders get no inotify watches at all.
    if (roles)
        options.excludedBelow = [roles = std::move(roles)](std::string_view path) {
            return roles->belowUntraversed(path);
        };
    auto watch = co_await watcher.watch(std::move(root), std::move(options), token);
    if (!watch) {
        co_return std::unexpected(std::move(watch.error()));
    }
    for (;;) {
        auto batch = co_await (*watch)->next(token);
        if (!batch) {
            co_return std::unexpected(std::move(batch.error()));
        }
        mailbox->post(std::move(*batch));
    }
}

} // namespace

struct Workbench::WatchState {
    std::shared_ptr<WatchMailbox> mailbox = std::make_shared<WatchMailbox>();
    std::stop_source stop;
    std::optional<sched::JoinHandle<Result<void>>> handle;
};

Workbench::Workbench(ShellWindow& shell, WorkbenchServices services)
    : m_shell(shell), m_services(std::move(services)), m_findActions(shell.environment().registry),
      m_scanTimer(new QTimer(this)), m_autosaveTimer(new QTimer(this)) {
    setObjectName(QStringLiteral("workbench"));
    m_scanTimer->setObjectName(QStringLiteral("projectScanTimer"));
    m_scanTimer->setInterval(25);
    connect(m_scanTimer, &QTimer::timeout, this, [this] { pollScan(); });
    addToolWindow();
    addTodoToolWindow();
    registerActions();
    {
        std::vector<input::Binding> defaults(m_shell.environment().keymap.moduleDefaults().begin(),
                                             m_shell.environment().keymap.moduleDefaults().end());
        defaults.push_back({std::string(workbench_actions::kToggleBookmark),
                            {*input::parseSequence("F11", input::SyntaxMode::Strict)}});
        defaults.push_back({std::string(workbench_actions::kShowBookmarks),
                            {*input::parseSequence("Shift+F11", input::SyntaxMode::Strict)}});
        m_shell.environment().keymap.setModuleDefaults(std::move(defaults));
    }
    m_services.editors.setActions(BannerActions{
        [this](const std::string& path) { reloadFromDisk(path); },
        [this](const std::string& path) { overwriteDisk(path); },
        [this](const std::string& path) { static_cast<void>(showMerge(path)); },
    });
    m_services.saveManager.setExternalChangeListener(
        [this](const document::ExternalChange& change) { handleExternalChange(change); });
    EditorTabs& tabs = m_shell.tabs();
    connect(&tabs, &EditorTabs::statusUpdated, this, [this](IEditorView* editor) { onEditorStatus(editor); });
    connect(&tabs, &EditorTabs::currentEditorChanged, this,
            [this](IEditorView* editor) { onCurrentEditorChanged(editor); });
    m_autosaveTimer->setObjectName(QStringLiteral("autosaveTimer"));
    m_autosaveTimer->setInterval(500);
    connect(m_autosaveTimer, &QTimer::timeout, this, [this] { pollAutosave(); });
    if (m_services.autosave != nullptr) {
        m_autosaveTimer->start();
    }
    m_shell.installEventFilter(this);
    if (QCoreApplication* app = QCoreApplication::instance(); app != nullptr) {
        app->installEventFilter(this);
    }
}

Workbench::~Workbench() {
    if (QCoreApplication* app = QCoreApplication::instance(); app != nullptr) {
        app->removeEventFilter(this);
    }
    m_shell.removeEventFilter(this);
    static_cast<void>(saveSession());
    stopWatching();
    if (m_initialFiles) {
        m_initialStop.request_stop();
        auto files = m_initialFiles->join();
        m_initialFiles.reset();
        retireSnapshot(m_services.scheduler, std::make_shared<nav::ProjectFiles>(std::move(files)));
    }
    if (m_scan) {
        m_scanStop.request_stop();
        auto result = m_scan->join();
        m_scan.reset();
        if (result) {
            retireSnapshot(m_services.scheduler, std::move(*result));
        }
    }
    if (m_todoScan) {
        m_todoStop.request_stop();
        static_cast<void>(m_todoScan->join());
        m_todoScan.reset();
    }
    if (m_fileSystem != nullptr) {
        m_fileSystem->setFiles({});
    }
    retireSnapshot(m_services.scheduler, std::move(m_files));
    retireSolutionSnapshot(m_services.scheduler, std::move(m_solutionFiles));
    m_services.saveManager.setExternalChangeListener({});
    m_services.editors.setActions({});
    // Each view's destroyed() erases its entry: take the map first.
    const auto merges = std::exchange(m_merges, {});
    for (const auto& [path, view] : merges) {
        delete view.data();
    }
}

// --- Project ---

Result<void> Workbench::openProject(const std::string& path) {
    const std::string canonical = m_services.documents.canonicalize(path);
    auto status = m_services.fs.status(canonical);
    if (!status) {
        return std::unexpected(std::move(status.error()));
    }
    if (status->type == io::FileType::Directory) {
        return openRoot(canonical, {}, {});
    }
    const QString extension = lowerExtension(canonical);
    if (extension != QStringLiteral(".sln") && extension != QStringLiteral(".slnx")) {
        return fail(ErrorCode::InvalidArgument, "'" + canonical + "' is not a folder or a solution");
    }
    auto model = project::SolutionModel::load(m_services.fs, canonical);
    if (!model) {
        return std::unexpected(std::move(model.error()));
    }
    return openSolution(*model);
}

Result<void> Workbench::openSolution(const project::SolutionModel& solution) {
    std::string path = m_services.documents.canonicalize(solution.path());
    std::string root(nav::parentOf(path));
    return openRoot(std::move(root), std::move(path), nav::solutionFiles(solution));
}

Result<void> Workbench::refreshSolution(const project::SolutionModel& solution) {
    if (m_solution.empty() || m_services.documents.canonicalize(solution.path()) != m_solution)
        return fail(ErrorCode::InvalidArgument, "the solution being refreshed is not open");
    ++m_projectGeneration;
    m_scanStop.request_stop();
    m_initialStop.request_stop();
    m_scanPublished = false;
    auto old = std::move(m_solutionFiles);
    m_solutionFiles = std::make_shared<const std::vector<std::string>>(nav::solutionFiles(solution));
    retireSolutionSnapshot(m_services.scheduler, std::move(old));
    startInitialFiles();
    refresh();
    return {};
}

Result<void> Workbench::openRoot(std::string root, std::string solution, std::vector<std::string> solutionFiles) {
    if (!m_root.empty() && !closeProject()) {
        return fail(ErrorCode::Cancelled, "a tab of the open project could not be closed");
    }
    m_solution = std::move(solution);
    setRoot(std::move(root), std::move(solutionFiles));
    if (m_services.sessions != nullptr) {
        if (auto session = m_services.sessions->load(m_root); session) {
            applySession(*session);
        }
        if (auto stored = m_services.sessions->setLastProject(m_root); !stored) {
            log::warn("ui", "cannot record the last project: {}", stored.error().message());
        }
    }
    return {};
}

void Workbench::setRoot(std::string root, std::vector<std::string> solutionFiles) {
    stopWatching();
    ++m_projectGeneration;
    m_scanStop.request_stop();
    m_initialStop.request_stop();
    m_todoStop.request_stop();
    m_todoRescanQueued = false;
    m_scanPublished = false;
    m_root = std::move(root);
    loadDirectoryRoles();
    auto oldSolutionFiles = std::move(m_solutionFiles);
    m_solutionFiles = std::make_shared<const std::vector<std::string>>(std::move(solutionFiles));
    auto oldFiles = m_files;
    auto files = std::make_shared<nav::ProjectFiles>(m_root);
    if (!m_services.scheduler)
        files->setSolutionFiles(*m_solutionFiles);
    m_files = std::move(files);
    m_history.clear();
    m_recentFiles.clear();
    m_recentLocations.clear();
    m_lastEditPlace.reset();
    m_bookmarks.clear();
    m_numberedBookmarks.fill(std::nullopt);
    m_cmakeTargets.clear();
    m_cmakeArguments.clear();
    if (m_fileSystem != nullptr) {
        m_fileSystem->setFiles(m_files);
    }
    if (m_todoList != nullptr) {
        m_todoList->clear();
    }
    retireSnapshot(m_services.scheduler, std::move(oldFiles));
    retireSolutionSnapshot(m_services.scheduler, std::move(oldSolutionFiles));
    const std::string_view name = m_solution.empty() ? nav::baseName(m_root) : nav::baseName(m_solution);
    m_shell.setProject(fromUtf8(name));
    startInitialFiles();
    refresh();
    startWatching();
}

bool Workbench::closeProject() {
    static_cast<void>(saveSession());
    m_shell.tabs().closeAll();
    if (m_shell.tabs().totalCount() > 0) {
        return false; // A tab whose save failed was kept by the user; so is the project.
    }
    stopWatching();
    ++m_projectGeneration;
    m_scanStop.request_stop();
    m_initialStop.request_stop();
    m_todoStop.request_stop();
    m_todoRescanQueued = false;
    m_scanPublished = false;
    m_root.clear();
    m_roles.reset();
    m_solution.clear();
    m_lastEditPlace.reset();
    m_bookmarks.clear();
    m_numberedBookmarks.fill(std::nullopt);
    auto oldSolutionFiles = std::move(m_solutionFiles);
    m_solutionFiles = std::make_shared<const std::vector<std::string>>();
    auto oldFiles = std::move(m_files);
    m_files = std::make_shared<nav::ProjectFiles>();
    if (m_fileSystem != nullptr) {
        m_fileSystem->setFiles(m_files);
    }
    if (m_todoList != nullptr) {
        m_todoList->clear();
    }
    retireSnapshot(m_services.scheduler, std::move(oldFiles));
    retireSolutionSnapshot(m_services.scheduler, std::move(oldSolutionFiles));
    m_shell.setProject({});
    return true;
}

void Workbench::refresh() {
    if (m_root.empty()) {
        return;
    }
    if (m_services.scheduler == nullptr) {
        sched::SimExecutor sim;
        auto scan = sim.spawn(sched::Lane::Background,
                              prepareProjectFiles(m_services.fs, sim, m_root, m_solutionFiles, m_files, {}, m_roles))
                        .join();
        applyScan(std::move(scan));
        return;
    }
    if (m_scanPending) {
        m_rescanQueued = true;
        return;
    }
    m_scanStop = std::stop_source();
    m_scanGeneration = m_projectGeneration;
    m_scan.emplace(m_services.scheduler->spawn(
        sched::Lane::Background, prepareProjectFiles(m_services.fs, *m_services.scheduler, m_root, m_solutionFiles,
                                                     m_files, m_scanStop.get_token(), m_roles)));
    m_scanPending = true;
    m_scanTimer->start();
}

void Workbench::startInitialFiles() {
    if (!m_services.scheduler || m_initialFiles || m_root.empty() || m_solutionFiles->empty() || m_scanPublished)
        return;
    m_initialStop = std::stop_source();
    m_initialGeneration = m_projectGeneration;
    m_initialFiles.emplace(m_services.scheduler->spawn(
        sched::Lane::Background, prepareInitialFiles(m_root, m_solutionFiles, m_initialStop.get_token())));
    m_scanTimer->start();
}

void Workbench::pollInitialFiles() {
    if (!m_initialFiles || !m_initialFiles->ready())
        return;
    auto files = m_initialFiles->join();
    m_initialFiles.reset();
    if (m_initialGeneration == m_projectGeneration && !m_scanPublished && !m_initialStop.stop_requested()) {
        auto old = std::move(m_files);
        m_files = std::make_shared<nav::ProjectFiles>(std::move(files));
        if (m_fileSystem)
            m_fileSystem->setFiles(m_files);
        if (m_todoList != nullptr && (m_shell.toolWindows().isVisible(kTodoToolWindow) || m_todoList->isVisible())) {
            refreshTodoItems();
        }
        retireSnapshot(m_services.scheduler, std::move(old));
    } else {
        retireSnapshot(m_services.scheduler, std::make_shared<nav::ProjectFiles>(std::move(files)));
    }
    if (m_initialGeneration != m_projectGeneration)
        startInitialFiles();
}

void Workbench::pollScan() {
    if (m_scan && m_scan->ready()) {
        auto scan = m_scan->join();
        m_scan.reset();
        m_scanPending = false;
        if (m_scanGeneration == m_projectGeneration) {
            if (scan) {
                m_scanPublished = true;
                m_initialStop.request_stop();
            }
            applyScan(std::move(scan));
        } else {
            if (scan)
                retireSnapshot(m_services.scheduler, std::move(*scan));
            m_rescanQueued = true;
        }
        if (m_rescanQueued) {
            m_rescanQueued = false;
            refresh();
        }
    }
    pollInitialFiles();
    if (m_todoScan && m_todoScan->ready()) {
        auto entries = m_todoScan->join();
        m_todoScan.reset();
        if (m_todoScanGeneration == m_projectGeneration && !m_todoStop.stop_requested()) {
            applyTodoEntries(entries);
        }
        if (m_todoRescanQueued) {
            m_todoRescanQueued = false;
            refreshTodoItems();
        }
    }
    if (!m_scan && !m_initialFiles && !m_todoScan)
        m_scanTimer->stop();
}

void Workbench::applyScan(Result<std::shared_ptr<const nav::ProjectFiles>> scan) {
    if (!scan) {
        notifyError(text("Cannot list %1").arg(nativePath(m_root)), scan.error());
        return;
    }
    m_scanPublished = true;
    if (*scan == m_files) {
        if (m_todoList != nullptr && (m_shell.toolWindows().isVisible(kTodoToolWindow) || m_todoList->isVisible()) &&
            m_todoList->count() == 0) {
            refreshTodoItems();
        }
        return;
    }
    auto oldFiles = std::move(m_files);
    m_files = std::move(*scan);
    if (m_fileSystem != nullptr) {
        m_fileSystem->setFiles(m_files);
    }
    if (m_todoList != nullptr && (m_shell.toolWindows().isVisible(kTodoToolWindow) || m_todoList->isVisible())) {
        refreshTodoItems();
    }
    retireSnapshot(m_services.scheduler, std::move(oldFiles));
}

void Workbench::startWatching() {
    if (m_services.watcher == nullptr || m_services.scheduler == nullptr || m_root.empty()) {
        return;
    }
    m_watch = std::make_unique<WatchState>();
    m_watch->mailbox->target = this;
    m_watch->handle.emplace(
        m_services.scheduler->spawn(sched::Lane::Background, pumpWatch(*m_services.watcher, m_root, m_watch->mailbox,
                                                                       m_roles, m_watch->stop.get_token())));
}

void Workbench::loadDirectoryRoles() {
    m_roles.reset();
    if (m_root.empty())
        return;
    std::vector<project::DirectoryRoleOverride> overrides;
    if (m_services.directoryRoles != nullptr)
        overrides = m_services.directoryRoles->overridesFor(m_root);
    auto roles = project::DirectoryRoles::load(m_services.fs, m_root, std::move(overrides));
    if (!roles) {
        // The project tree falls back to the default excludes (projectScanOptions without roles).
        log::warn("ui", "no directory roles for {}: {}", m_root, roles.error().describe());
        return;
    }
    if (!(*roles)->presetsProblem().empty())
        log::info("ui", "directory roles without CMake preset build folders: {}", (*roles)->presetsProblem());
    m_roles = std::move(*roles);
}

Result<void> Workbench::setDirectoryRole(const std::string& directory, std::optional<project::DirectoryRole> role) {
    if (m_root.empty() || m_services.directoryRoles == nullptr)
        return fail(ErrorCode::InvalidArgument, "no project with a directory role store is open");
    // Normalised first, so "<root>/../outside" is judged where it points, not by its spelling. POSIX
    // normalisation keeps ".." (a symlink makes it ambiguous), so a path still holding one is refused.
    auto normalized = io::normalizePath(directory);
    const auto hasDotDot = [](std::string_view path) {
        for (std::size_t at = path.find(".."); at != std::string_view::npos; at = path.find("..", at + 1)) {
            const bool starts = at == 0 || path[at - 1] == '/' || path[at - 1] == '\\';
            const bool ends = at + 2 == path.size() || path[at + 2] == '/' || path[at + 2] == '\\';
            if (starts && ends)
                return true;
        }
        return false;
    };
    if (!normalized || hasDotDot(*normalized) || !isUnder(m_root, *normalized))
        return fail(ErrorCode::InvalidArgument, std::format("'{}' is not in the project", directory));
    if (auto saved = m_services.directoryRoles->set(*normalized, role); !saved)
        return saved;
    loadDirectoryRoles();
    if (m_watch) {
        stopWatching();
        startWatching();
    }
    refresh();
    return {};
}

void Workbench::stopWatching() {
    if (!m_watch) {
        return;
    }
    {
        const std::scoped_lock lock(m_watch->mailbox->mutex);
        m_watch->mailbox->target = nullptr;
    }
    m_watch->stop.request_stop();
    if (m_watch->handle) {
        // The pump ends with Cancelled (or the watch's own error); either way it is over.
        static_cast<void>(m_watch->handle->join());
    }
    m_watch.reset();
}

void Workbench::openSettingsFile(const std::string& path) {
    if (path.empty()) {
        return;
    }
    if (auto exists = m_services.fs.exists(path); exists && !*exists) {
        // A settings file starts as an empty object.
        static_cast<void>(m_services.fs.createDirectories(nav::parentOf(path)));
        if (auto written = m_services.fs.writeFileAtomic(path, "{}\n"); !written) {
            notifyError(text("Cannot create %1").arg(nativePath(path)), written.error());
            return;
        }
    }
    static_cast<void>(openFile(path));
}

std::optional<std::string> Workbench::projectSettingsFile(bool personal) const {
    if (m_root.empty()) {
        return std::nullopt;
    }
    auto layout = settings::ProjectLayout::forRoot(m_root, nav::styleOf(m_root));
    if (!layout) {
        return std::nullopt;
    }
    if (auto ensured = settings::ensureProjectLayout(m_services.fs, *layout); !ensured) {
        log::warn("ui", "cannot create {}: {}", layout->frostDirectory, ensured.error().message());
    }
    return personal ? layout->personalSettings : layout->sharedSettings;
}

void Workbench::notifyError(const QString& title, const Error& error) {
    m_shell.notifications().notify({title, fromUtf8(error.message()), NotificationKind::Error, std::nullopt, {}});
}

void Workbench::notifySaveWarning(const std::string& path, const Error& warning) {
    m_saveWarningNeedsReview = true;
    m_shell.notifications().notify({text("Saved %1 with a warning").arg(nativePath(path)),
                                    QString::fromStdString(warning.describe()),
                                    NotificationKind::Warning,
                                    std::nullopt,
                                    {}});
}

// --- Autosave and events ---

void Workbench::pollAutosave() {
    if (m_services.autosave == nullptr) {
        return;
    }
    const std::optional<IClock::SteadyTime> deadline = m_services.autosave->nextDeadline();
    if (!deadline || *deadline > m_services.clock.steadyNow()) {
        return;
    }
    const document::AutosaveController::PollResult result = m_services.autosave->poll();
    if (result.autosave) {
        reportSaveAll(*result.autosave, true);
    }
    if (result.journal) {
        for (const Error& error : result.journal->errors) {
            log::warn("ui", "hot-exit journal: {}", error.describe());
        }
    }
}

bool Workbench::eventFilter(QObject* watched, QEvent* event) {
    if (watched == &m_shell && event->type() == QEvent::Close) {
        // Save while the window and its notification center can still tell the user about warnings.
        frameDeactivated();
        if (m_saveWarningNeedsReview) {
            m_saveWarningNeedsReview = false;
            m_shell.notifications().notify({text("Review save warnings before closing"),
                                            text("Files were saved, but follow-up work needs attention. "
                                                 "Review the save warnings in Notifications, then close again."),
                                            NotificationKind::Warning,
                                            true,
                                            {}});
            static_cast<QCloseEvent*>(event)->ignore();
            return true;
        }
    }
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::Wheel:
    case QEvent::InputMethod:
        if (m_services.autosave != nullptr) {
            m_services.autosave->noteActivity();
        }
        break;
    default: break;
    }
    if (event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const auto* widget = qobject_cast<QWidget*>(watched);
        if ((mouse->button() == Qt::BackButton || mouse->button() == Qt::ForwardButton) && widget != nullptr &&
            widget->window() == &m_shell) {
            if (mouse->button() == Qt::BackButton) {
                goBack();
            } else {
                goForward();
            }
            return true;
        }
    }
    // Rider's frame hooks: autosave when the IDE loses focus, the disk check when it gets it back.
    if (watched == &m_shell && event->type() == QEvent::WindowDeactivate) {
        frameDeactivated();
    } else if (watched == &m_shell && event->type() == QEvent::WindowActivate) {
        frameActivated();
    }
    return QObject::eventFilter(watched, event);
}

} // namespace frost::ui
