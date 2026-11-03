// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/ui/workbench/workbench.hpp): actions, popups, project and session handling, hot
// exit, external changes, saving and autosave over the ShellWindow.

#include "frost/ui/workbench/workbench.hpp"

#include "frost/core/log.hpp"
#include "frost/input/keymap.hpp"
#include "frost/io/path.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/search/fuzzy_path.hpp"
#include "frost/settings/project_layout.hpp"
#include "frost/settings/store.hpp"
#include "frost/ui/icon_ids.hpp"
#include "frost/ui/nav/paths.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/workbench/file_system_view.hpp"
#include "frost/ui/workbench/merge_view.hpp"

#include "workbench_util.hpp"
#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <mutex>
#include <set>
#include <utility>

namespace frost::ui {

using workbench_detail::fileIcon;
using workbench_detail::fromUtf8;
using workbench_detail::nativePath;
using workbench_detail::text;
using workbench_detail::themedIcon;
using workbench_detail::toUtf8;

namespace {

// Actions run from the main window, not from popups or dialogs (as the shell's own).
constexpr input::ContextRule kWindowRule{{}, {input::ContextKey::PopupFocus, input::ContextKey::DialogFocus}};
// The i18n context of action names from the registry (the shell's).
constexpr std::string_view kActionsContext = "frost::input::actions";

[[nodiscard]] QString lowerExtension(std::string_view path) {
    const std::string_view name = nav::baseName(path);
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? QString() : fromUtf8(name.substr(dot)).toLower();
}

[[nodiscard]] bool isUnder(std::string_view root, std::string_view path) {
    if (root.empty() || path.size() <= root.size() || !path.starts_with(root)) {
        return false;
    }
    const char next = path[root.size()];
    return next == '/' || next == '\\' || root.back() == '/' || root.back() == '\\';
}

[[nodiscard]] QVariant fileData(const std::string& path, std::optional<int> line = std::nullopt,
                                std::optional<int> column = std::nullopt) {
    QVariantMap map;
    map.insert(QStringLiteral("kind"), QStringLiteral("file"));
    map.insert(QStringLiteral("path"), fromUtf8(path));
    if (line) {
        map.insert(QStringLiteral("line"), *line);
        map.insert(QStringLiteral("column"), column.value_or(1));
    }
    return map;
}

} // namespace

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

sched::Task<Result<void>> pumpWatch(io::IFileWatcher& watcher, std::string root, std::shared_ptr<WatchMailbox> mailbox,
                                    std::stop_token token) {
    auto watch = co_await watcher.watch(std::move(root), io::WatchOptions{}, token);
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
    : m_shell(shell), m_services(std::move(services)), m_findActions(shell.environment().registry) {
    setObjectName(QStringLiteral("workbench"));
    m_scanTimer = new QTimer(this);
    m_scanTimer->setInterval(25);
    connect(m_scanTimer, &QTimer::timeout, this, [this] { pollScan(); });
    addToolWindow();
    registerActions();
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
    m_autosaveTimer = new QTimer(this);
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
    if (m_scan) {
        m_scanStop.request_stop();
        static_cast<void>(m_scan->join());
        m_scan.reset();
    }
    m_services.saveManager.setExternalChangeListener({});
    m_services.editors.setActions({});
    // Each view's destroyed() erases its entry: take the map first.
    const auto merges = std::exchange(m_merges, {});
    for (const auto& [path, view] : merges) {
        delete view.data();
    }
}

// --- Actions ---

void Workbench::addAction(std::string_view id, std::string_view name, std::string_view group, std::function<void()> run,
                          std::function<bool()> enabled, std::vector<std::string> synonyms) {
    input::ActionSpec spec;
    spec.id = std::string(id);
    spec.name = std::string(name);
    spec.group = std::string(group);
    spec.synonyms = std::move(synonyms);
    spec.context = kWindowRule;
    const QPointer<Workbench> self(this);
    spec.handler = [self, run = std::move(run)](const input::ActionContext&, const input::InvokeInfo&) {
        if (self != nullptr && run) {
            run();
        }
    };
    if (enabled) {
        spec.enabled = [self, enabled = std::move(enabled)](const input::ActionContext&) {
            return self != nullptr && enabled();
        };
    } else {
        spec.enabled = [self](const input::ActionContext&) { return self != nullptr; };
    }
    if (Result<input::ActionId> added = m_shell.environment().registry.add(std::move(spec)); !added) {
        log::warn("ui", "workbench action {}: {}", id, added.error().message());
    }
}

void Workbench::registerActions() {
    namespace a = workbench_actions;
    const auto hasEditor = [this] { return m_shell.tabs().currentEditor() != nullptr; };
    const auto hasProject = [this] { return !m_root.empty(); };
    addAction(a::kFindAction, "Find Action...", "Help", [this] { showFindAction(); }, {}, {"command", "action"});
    addAction(a::kSearchEverywhere, "Search Everywhere", "Navigate", [this] { showSearchEverywhere(); }, {},
              {"find", "search"});
    addAction(a::kGoToFile, "Go to File...", "Navigate", [this] { showGoToFile(); }, {}, {"open file", "file"});
    addAction(a::kRecentFiles, "Recent Files", "Navigate", [this] { showRecentFiles(); }, {}, {"switch file"});
    addAction(a::kRecentLocations, "Recent Locations", "Navigate", [this] { showRecentLocations(); }, {},
              {"recent places"});
    addAction(a::kBack, "Back", "Navigate", [this] { goBack(); }, [this] { return m_history.canGoBack(); });
    addAction(a::kForward, "Forward", "Navigate", [this] { goForward(); }, [this] { return m_history.canGoForward(); });
    addAction(a::kSaveAll, "Save All", "File", [this] { saveAll(); }, {}, {"save"});
    addAction(a::kSaveDocument, "Save Document", "File", [this] { static_cast<void>(saveDocument()); }, hasEditor,
              {"save file"});
    // Rider's Reload All from Disk (Ctrl+Alt+Y): every open file, not only the active one.
    addAction(a::kReloadFromDisk, "Reload All from Disk", "File", [this] { reloadAllFromDisk(); }, {},
              {"refresh", "revert", "synchronize"});
    addAction(
        a::kCloseProject, "Close Project", "File",
        [this] {
            if (m_services.closeProjectHook) {
                m_services.closeProjectHook();
            } else {
                static_cast<void>(closeProject());
            }
        },
        hasProject);
    addAction(a::kFileSystemWindow, "File System", "View",
              [this] { m_shell.toolWindows().show(kFileSystemToolWindow); }, {}, {"files", "folder", "tree"});
    // The settings files' stand-in actions, described by frost::settings (settingsActions()).
    for (const settings::SettingsActionInfo& info : settings::settingsActions()) {
        const settings::Scope scope = info.scope;
        std::vector<std::string> synonyms;
        switch (scope) {
        case settings::Scope::ProjectShared: synonyms = {".frost/settings.json", "shared settings"}; break;
        case settings::Scope::ProjectPersonal: synonyms = {".frost/workspace.json", "workspace settings"}; break;
        default: synonyms = {"settings.json", "preferences"}; break;
        }
        std::function<bool()> enabled;
        if (info.needsProject) {
            enabled = hasProject;
        } else {
            enabled = [this] { return !m_services.applicationSettingsFile.empty(); };
        }
        addAction(
            info.id, info.name, info.group,
            [this, scope] {
                if (scope == settings::Scope::Application) {
                    openSettingsFile(m_services.applicationSettingsFile);
                } else if (auto file = projectSettingsFile(scope == settings::Scope::ProjectPersonal); file) {
                    openSettingsFile(*file);
                }
            },
            std::move(enabled), std::move(synonyms));
    }
}

void Workbench::addToolWindow() {
    auto* view = new FileSystemView(&m_shell);
    view->hide();
    m_fileSystem = view;
    connect(view, &FileSystemView::fileActivated, this,
            [this](const QString& path) { static_cast<void>(openFile(toUtf8(path))); });
    ToolWindowSpec spec;
    spec.id = std::string(kFileSystemToolWindow);
    spec.title = text("File System");
    spec.side = ToolWindowSide::Left;
    spec.iconId = std::string(icons::kNodesFolder);
    spec.shortcutText = shortcutText(workbench_actions::kFileSystemWindow);
    const QPointer<FileSystemView> content(view);
    spec.createContent = [content]() -> QWidget* { return content.data(); };
    if (auto added = m_shell.toolWindows().add(std::move(spec)); !added) {
        log::warn("ui", "File System tool window: {}", added.error().message());
    }
}

QString Workbench::shortcutText(std::string_view actionId) const {
    const ShellEnvironment& env = m_shell.environment();
    const std::optional<input::ActionId> id = env.registry.find(actionId);
    if (!id) {
        return {};
    }
    const std::optional<input::Sequence> shortcut = env.keymap.primaryShortcut(*id);
    return shortcut ? fromUtf8(input::toDisplayText(*shortcut, env.keymap.layout(), env.keymap.platform())) : QString();
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
    m_root = std::move(root);
    m_files = nav::ProjectFiles(m_root);
    m_files.setSolutionFiles(std::move(solutionFiles));
    m_history.clear();
    m_recentFiles.clear();
    m_recentLocations.clear();
    if (m_fileSystem != nullptr) {
        m_fileSystem->setFiles(m_files);
    }
    const std::string_view name = m_solution.empty() ? nav::baseName(m_root) : nav::baseName(m_solution);
    m_shell.setProject(fromUtf8(name));
    refresh();
    startWatching();
}

bool Workbench::closeProject() {
    static_cast<void>(saveSession());
    m_shell.tabs().closeAll();
    if (m_shell.tabs().count() > 0) {
        return false; // A tab whose save failed was kept by the user; so is the project.
    }
    stopWatching();
    m_root.clear();
    m_solution.clear();
    m_files = nav::ProjectFiles();
    if (m_fileSystem != nullptr) {
        m_fileSystem->setFiles(m_files);
    }
    m_shell.setProject({});
    return true;
}

void Workbench::refresh() {
    if (m_root.empty()) {
        return;
    }
    if (m_services.scheduler == nullptr) {
        sched::SimExecutor sim;
        auto scan = sim.spawn(sched::Lane::Background, nav::scanProject(m_services.fs, sim, m_root)).join();
        applyScan(std::move(scan));
        return;
    }
    if (m_scanPending) {
        m_rescanQueued = true;
        return;
    }
    m_scanStop = std::stop_source();
    m_scanRoot = m_root;
    m_scan.emplace(
        m_services.scheduler->spawn(sched::Lane::Background, nav::scanProject(m_services.fs, *m_services.scheduler,
                                                                              m_root, m_scanStop.get_token())));
    m_scanPending = true;
    m_scanTimer->start();
}

void Workbench::pollScan() {
    if (!m_scan || !m_scan->ready()) {
        return;
    }
    auto scan = m_scan->join();
    m_scan.reset();
    m_scanPending = false;
    m_scanTimer->stop();
    if (m_scanRoot == m_root) {
        applyScan(std::move(scan));
    } else {
        m_rescanQueued = true;
    }
    if (m_rescanQueued) {
        m_rescanQueued = false;
        refresh();
    }
}

void Workbench::applyScan(Result<io::TreeScan> scan) {
    if (!scan) {
        notifyError(text("Cannot list %1").arg(nativePath(m_root)), scan.error());
        return;
    }
    m_files.setScan(*scan);
    if (m_fileSystem != nullptr) {
        m_fileSystem->setFiles(m_files);
    }
}

void Workbench::startWatching() {
    if (m_services.watcher == nullptr || m_services.scheduler == nullptr || m_root.empty()) {
        return;
    }
    m_watch = std::make_unique<WatchState>();
    m_watch->mailbox->target = this;
    m_watch->handle.emplace(m_services.scheduler->spawn(
        sched::Lane::Background, pumpWatch(*m_services.watcher, m_root, m_watch->mailbox, m_watch->stop.get_token())));
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

// --- Opening and navigation ---

nav::Place Workbench::currentPlace() const {
    const IEditorView* editor = m_shell.tabs().currentEditor();
    if (editor == nullptr) {
        return {};
    }
    const EditorStatus status = editor->status();
    return nav::Place{editor->path(), status.line, status.column};
}

Result<int> Workbench::openFile(const std::string& path, std::optional<LinePosition> at, bool statusColumn) {
    const nav::Place from = currentPlace();
    m_navigating = true;
    Result<int> opened = m_shell.openFile(path);
    if (opened) {
        IEditorView* editor = m_shell.tabs().editor(*opened);
        if (at) {
            if (statusColumn)
                editor->goToStatusPosition(at->line, at->column);
            else
                editor->goToLine(at->line, at->column);
        }
        const EditorStatus status = editor->status();
        const nav::Place to{editor->path(), status.line, status.column};
        if (!from.path.empty() && !nav::isNear(from, to, nav::NavigationHistory::kNearLines)) {
            m_history.recordJump(from);
            m_recentLocations.touch(from);
        }
        m_recentLocations.touch(to);
        m_recentFiles.touch(editor->path());
        m_lastPlaces[editor->path()] = to;
        m_activePath = editor->path();
        m_shell.focusEditor();
    }
    m_navigating = false;
    return opened;
}

bool Workbench::openPlace(const nav::Place& place) {
    m_navigating = true;
    Result<int> opened = m_shell.openFile(place.path);
    if (opened) {
        IEditorView* editor = m_shell.tabs().editor(*opened);
        editor->goToStatusPosition(place.line, place.column);
        m_lastPlaces[editor->path()] = place;
        m_activePath = editor->path();
        m_recentFiles.touch(editor->path());
        m_shell.focusEditor();
    } else {
        m_history.forget(place.path);
    }
    m_navigating = false;
    return opened.has_value();
}

bool Workbench::goBack() {
    const auto target = m_history.back(currentPlace());
    return target && openPlace(*target);
}

bool Workbench::goForward() {
    const auto target = m_history.forward(currentPlace());
    return target && openPlace(*target);
}

void Workbench::onEditorStatus(IEditorView* editor) {
    if (editor == nullptr || m_restoring) {
        return;
    }
    const EditorStatus status = editor->status();
    const nav::Place now{editor->path(), status.line, status.column};
    auto& last = m_lastPlaces[editor->path()];
    if (!m_navigating && !last.path.empty()) {
        if (m_history.noteCaretMove(last, now)) {
            m_recentLocations.touch(last);
        }
    }
    last = now;
}

void Workbench::onCurrentEditorChanged(IEditorView* editor) {
    const std::string next = editor != nullptr ? editor->path() : std::string();
    if (!m_navigating && !m_restoring && !m_activePath.empty() && next != m_activePath && !next.empty()) {
        // A switch through the tab bar or the switcher: the place left is a jump.
        if (const auto it = m_lastPlaces.find(m_activePath); it != m_lastPlaces.end()) {
            m_history.recordJump(it->second);
        }
    }
    if (!next.empty() && !m_restoring) {
        m_recentFiles.touch(next);
        if (!m_lastPlaces.contains(next)) {
            const EditorStatus status = editor->status();
            m_lastPlaces[next] = nav::Place{next, status.line, status.column};
        }
    }
    m_activePath = next;
}

// --- Popups ---

void Workbench::closeOpenPopup() {
    // The context a popup captures must be the main window's, not the popup's being replaced.
    if (Popup* open = m_shell.activePopup(); open != nullptr) {
        open->closePopup(Popup::CloseReason::Dismissed);
    }
}

std::vector<ListPopupItem> Workbench::fileItems(const std::string& pattern, std::size_t limit) const {
    // The project's candidates, then recent files outside it (opened from elsewhere).
    std::vector<std::string> absolute = m_files.candidates();
    std::vector<std::string> display = m_files.displayCandidates();
    const std::set<std::string, std::less<>> known(absolute.begin(), absolute.end());
    for (const std::string& recent : m_recentFiles.paths()) {
        if (!known.contains(recent)) {
            absolute.push_back(recent);
            display.push_back(m_root.empty() ? recent : nav::relativeTo(m_root, recent));
        }
    }
    std::vector<std::size_t> order;
    if (pattern.empty()) {
        // Rider's empty Go to File: the recent files first.
        std::set<std::size_t> taken;
        for (const std::string& recent : m_recentFiles.paths()) {
            if (const auto it = std::ranges::find(absolute, recent); it != absolute.end()) {
                const auto index = static_cast<std::size_t>(it - absolute.begin());
                if (taken.insert(index).second) {
                    order.push_back(index);
                }
            }
        }
        for (std::size_t i = 0; i < absolute.size() && order.size() < limit; ++i) {
            if (!taken.contains(i)) {
                order.push_back(i);
            }
        }
        if (order.size() > limit) {
            order.resize(limit);
        }
    } else {
        for (const search::RankedPath& ranked : search::rankPaths(pattern, display, limit)) {
            order.push_back(ranked.index);
        }
    }
    std::vector<ListPopupItem> items;
    items.reserve(order.size());
    for (const std::size_t index : order) {
        const std::string& path = absolute[index];
        const std::string_view name = nav::baseName(path);
        const std::string_view folder = nav::parentOf(display[index] == path ? path : display[index]);
        ListPopupItem item;
        item.text = fromUtf8(name);
        item.detail = nativePath(folder);
        item.icon = fileIcon(name);
        item.data = fileData(path);
        items.push_back(std::move(item));
    }
    return items;
}

std::vector<ListPopupItem> Workbench::actionItems(const std::string& query, const input::ActionContext& context,
                                                  std::size_t limit) const {
    std::vector<ListPopupItem> items;
    if (QString::fromUtf8(query).trimmed().isEmpty()) {
        return items;
    }
    const ShellEnvironment& env = m_shell.environment();
    input::FindActionOptions options;
    options.limit = limit;
    options.includeDisabled = true;
    for (const input::FindActionHit& hit : m_findActions.query(query, env.keymap.bindings(), context, options)) {
        const input::ActionInfo& info = env.registry.info(hit.id);
        ListPopupItem item;
        item.text = qtr(kActionsContext, info.name);
        const QString shortcut = shortcutText(info.id);
        const QString group = info.group.empty() ? QString() : qtr(kActionsContext, info.group);
        item.detail =
            shortcut.isEmpty() ? group : (group.isEmpty() ? shortcut : shortcut + QStringLiteral("  ") + group);
        item.enabled = hit.enabled;
        QVariantMap data;
        data.insert(QStringLiteral("kind"), QStringLiteral("action"));
        data.insert(QStringLiteral("id"), fromUtf8(info.id));
        item.data = data;
        items.push_back(std::move(item));
    }
    return items;
}

void Workbench::acceptItem(const QVariant& data, const input::ActionContext& captured) {
    const QVariantMap map = data.toMap();
    const QString kind = map.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("file")) {
        std::optional<LinePosition> at;
        if (map.contains(QStringLiteral("line"))) {
            at = LinePosition{map.value(QStringLiteral("line")).toInt(), map.value(QStringLiteral("column")).toInt()};
        }
        static_cast<void>(openFile(toUtf8(map.value(QStringLiteral("path")).toString()), at));
        return;
    }
    if (kind == QStringLiteral("place")) {
        const nav::Place place{toUtf8(map.value(QStringLiteral("path")).toString()),
                               map.value(QStringLiteral("line")).toInt(), map.value(QStringLiteral("column")).toInt()};
        static_cast<void>(openFile(place.path, LinePosition{place.line, place.column}, true));
        return;
    }
    if (kind == QStringLiteral("action")) {
        input::ActionRegistry& registry = m_shell.environment().registry;
        const std::optional<input::ActionId> id = registry.find(toUtf8(map.value(QStringLiteral("id")).toString()));
        if (!id) {
            return;
        }
        // The focus keys from before the popup opened; level and trust as they are now.
        input::ActionContext context = captured;
        const input::ActionContext live = m_shell.actionContext();
        context.level = live.level;
        context.trusted = live.trusted;
        if (registry.tryInvoke(*id, context, input::InvokeInfo{input::InvokeSource::FindAction, false}) ==
            input::InvokeResult::Ran) {
            m_findActions.noteInvoked(*id);
        }
        m_shell.refreshActions();
    }
}

ListPopup* Workbench::showFindAction(const QString& query) {
    closeOpenPopup();
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Find Action"), true);
    popup->setObjectName(QStringLiteral("findActionPopup"));
    popup->setFilterMode(ListPopup::FilterMode::External);
    connect(popup, &ListPopup::filterTextChanged, popup, [this, popup, captured](const QString& value) {
        popup->setItems(actionItems(toUtf8(value), captured, 100));
    });
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    m_shell.openPopup(popup);
    if (!query.isEmpty()) {
        popup->setFilterText(query);
    }
    return popup;
}

ListPopup* Workbench::showGoToFile(const QString& query) {
    closeOpenPopup();
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Go to File"), true);
    popup->setObjectName(QStringLiteral("goToFilePopup"));
    popup->setFilterMode(ListPopup::FilterMode::External);
    const auto update = [this, popup](const QString& value) {
        const search::LocationQuery location = search::parseLocationQuery(toUtf8(value.trimmed()));
        std::vector<ListPopupItem> items = fileItems(location.pattern, 50);
        if (location.line) {
            for (ListPopupItem& item : items) {
                item.data = fileData(toUtf8(item.data.toMap().value(QStringLiteral("path")).toString()), location.line,
                                     location.column);
            }
        }
        popup->setItems(std::move(items));
    };
    connect(popup, &ListPopup::filterTextChanged, popup, update);
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    update(QString());
    m_shell.openPopup(popup);
    if (!query.isEmpty()) {
        popup->setFilterText(query);
    }
    return popup;
}

ListPopup* Workbench::showSearchEverywhere(const QString& query) {
    closeOpenPopup();
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Search Everywhere"), true);
    popup->setObjectName(QStringLiteral("searchEverywherePopup"));
    popup->setFilterMode(ListPopup::FilterMode::External);
    const auto update = [this, popup, captured](const QString& value) {
        const std::string utf8 = toUtf8(value.trimmed());
        const search::LocationQuery location = search::parseLocationQuery(utf8);
        std::vector<ListPopupItem> items;
        std::vector<ListPopupItem> files = fileItems(location.pattern, 8);
        if (!files.empty()) {
            ListPopupItem header;
            header.text = text("Files");
            header.header = true;
            items.push_back(std::move(header));
            for (ListPopupItem& item : files) {
                if (location.line) {
                    item.data = fileData(toUtf8(item.data.toMap().value(QStringLiteral("path")).toString()),
                                         location.line, location.column);
                }
                items.push_back(std::move(item));
            }
        }
        std::vector<ListPopupItem> actions = actionItems(utf8, captured, 8);
        if (!actions.empty()) {
            ListPopupItem header;
            header.text = text("Actions");
            header.header = true;
            items.push_back(std::move(header));
            std::ranges::move(actions, std::back_inserter(items));
        }
        popup->setItems(std::move(items));
    };
    connect(popup, &ListPopup::filterTextChanged, popup, update);
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    update(QString());
    m_shell.openPopup(popup);
    if (!query.isEmpty()) {
        popup->setFilterText(query);
    }
    return popup;
}

ListPopup* Workbench::showRecentFiles() {
    closeOpenPopup();
    const auto paths = m_recentFiles.paths();
    if (paths.empty()) {
        return nullptr;
    }
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Recent Files"), true);
    popup->setObjectName(QStringLiteral("recentFilesPopup"));
    std::vector<ListPopupItem> items;
    for (const std::string& path : paths) {
        ListPopupItem item;
        item.text = fromUtf8(nav::baseName(path));
        item.detail = nativePath(nav::parentOf(m_root.empty() ? path : nav::relativeTo(m_root, path)));
        item.icon = fileIcon(nav::baseName(path));
        item.data = fileData(path);
        items.push_back(std::move(item));
    }
    popup->setItems(std::move(items));
    // The current file is first; the useful default is the one before it.
    const bool currentFirst = paths.front() == m_activePath;
    popup->setCurrentItem(currentFirst && paths.size() > 1 ? 1 : 0);
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    m_shell.openPopup(popup);
    return popup;
}

ListPopup* Workbench::showRecentLocations() {
    closeOpenPopup();
    const auto places = m_recentLocations.places();
    if (places.empty()) {
        return nullptr;
    }
    const input::ActionContext captured = m_shell.actionContext();
    auto* popup = new ListPopup(&m_shell, text("Recent Locations"), true);
    popup->setObjectName(QStringLiteral("recentLocationsPopup"));
    std::vector<ListPopupItem> items;
    for (const nav::Place& place : places) {
        ListPopupItem item;
        item.text = QStringLiteral("%1:%2").arg(fromUtf8(nav::baseName(place.path))).arg(place.line);
        item.detail = nativePath(nav::parentOf(m_root.empty() ? place.path : nav::relativeTo(m_root, place.path)));
        item.icon = fileIcon(nav::baseName(place.path));
        QVariantMap data;
        data.insert(QStringLiteral("kind"), QStringLiteral("place"));
        data.insert(QStringLiteral("path"), fromUtf8(place.path));
        data.insert(QStringLiteral("line"), place.line);
        data.insert(QStringLiteral("column"), place.column);
        item.data = data;
        items.push_back(std::move(item));
    }
    popup->setItems(std::move(items));
    connect(popup, &ListPopup::itemAccepted, this,
            [this, captured](int, const QVariant& data) { acceptItem(data, captured); });
    m_shell.openPopup(popup);
    return popup;
}

// --- Sessions ---

nav::SessionState Workbench::captureSession() const {
    nav::SessionState state;
    state.root = m_root;
    const EditorTabs& tabs = m_shell.tabs();
    for (int i = 0; i < tabs.count(); ++i) {
        const IEditorView* editor = tabs.editor(i);
        const EditorViewState view = editor->viewState();
        state.tabs.push_back(nav::SessionTab{editor->path(), tabs.isPinned(i), view.line, view.column, view.topLine,
                                             view.foldTextHash, view.folds});
    }
    state.active = tabs.currentIndex();
    ToolWindowManager& windows = m_shell.toolWindows();
    windows.syncSizesFromSplitters();
    for (const std::string& id : windows.ids()) {
        state.toolWindows.push_back(nav::SessionToolWindow{id, windows.isVisible(id), windows.size(id)});
    }
    state.recentFiles.assign(m_recentFiles.paths().begin(), m_recentFiles.paths().end());
    state.recentLocations.assign(m_recentLocations.places().begin(), m_recentLocations.places().end());
    return state;
}

void Workbench::applySession(const nav::SessionState& state) {
    m_restoring = true;
    EditorTabs& tabs = m_shell.tabs();
    int active = -1;
    for (std::size_t i = 0; i < state.tabs.size(); ++i) {
        const nav::SessionTab& tab = state.tabs[i];
        // ShellWindow::openFile notifies a failure (a deleted file, the large-file guard).
        Result<int> opened = m_shell.openFile(tab.path);
        if (!opened) {
            continue;
        }
        IEditorView* editor = tabs.editor(*opened);
        editor->restoreViewState(EditorViewState{tab.line, tab.column, tab.top, tab.foldTextHash, tab.folds});
        m_lastPlaces[editor->path()] = nav::Place{editor->path(), tab.line, tab.column};
        if (tab.pinned) {
            tabs.setPinned(tabs.indexOf(editor->path()), true);
        }
        if (static_cast<int>(i) == state.active) {
            active = tabs.indexOf(editor->path());
        }
    }
    if (active < 0 && state.active >= 0 && static_cast<std::size_t>(state.active) < state.tabs.size()) {
        active = tabs.indexOf(state.tabs[static_cast<std::size_t>(state.active)].path);
    }
    if (active >= 0) {
        tabs.activate(active);
        m_activePath = tabs.editor(active)->path();
    }
    ToolWindowManager& windows = m_shell.toolWindows();
    for (const nav::SessionToolWindow& window : state.toolWindows) {
        if (!windows.contains(window.id)) {
            continue;
        }
        if (window.size > 0) {
            windows.setSize(window.id, window.size);
        }
        if (window.visible) {
            windows.show(window.id, false);
        } else {
            windows.hide(window.id);
        }
    }
    m_recentFiles.assign(state.recentFiles);
    m_recentLocations.assign(state.recentLocations);
    m_restoring = false;
}

Result<void> Workbench::saveSession() {
    if (m_services.sessions == nullptr || m_root.empty()) {
        return {};
    }
    return m_services.sessions->save(captureSession());
}

bool Workbench::restoreLastProject() {
    if (m_services.sessions == nullptr) {
        return false;
    }
    const std::optional<std::string> last = m_services.sessions->lastProject();
    if (!last) {
        return false;
    }
    if (auto exists = m_services.fs.exists(*last); !exists || !*exists) {
        return false;
    }
    if (auto opened = m_services.openProjectHook ? m_services.openProjectHook(*last) : openProject(*last); !opened) {
        notifyError(text("Cannot reopen %1").arg(nativePath(*last)), opened.error());
        return false;
    }
    return true;
}

// --- Hot exit ---

std::size_t Workbench::offerRestore(storage::RecoveryScan scan) {
    std::set<std::string> keys;
    bool crashed = false;
    for (storage::RecoveredJournal& journal : scan.journals) {
        crashed = crashed || !journal.endedCleanly();
        if (auto entries = journal.entries(); entries) {
            for (const storage::JournalEntry& entry : *entries) {
                keys.insert(entry.state.documentKey);
            }
        } else {
            keys.insert(journal.path()); // Unreadable here; restore() reports it.
        }
    }
    if (keys.empty()) {
        for (storage::RecoveredJournal& journal : scan.journals) {
            static_cast<void>(std::move(journal).discard());
        }
        return 0;
    }
    m_recovery.emplace(std::move(scan));
    const auto count = static_cast<int>(keys.size());
    Notification notification;
    notification.title = crashed ? text("FrostIDE closed unexpectedly. Restore %1 unsaved files?").arg(count)
                                 : text("Restore %1 unsaved files from the last session?").arg(count);
    notification.text = text("Their unsaved changes were kept in the hot-exit journal.");
    notification.kind = NotificationKind::Warning;
    notification.sticky = true;
    const QPointer<Workbench> self(this);
    notification.actions.push_back(NotificationAction{text("Restore"), [self] {
                                                          if (self != nullptr) {
                                                              static_cast<void>(self->restoreRecovered());
                                                          }
                                                      }});
    notification.actions.push_back(NotificationAction{text("Discard"), [self] {
                                                          if (self != nullptr) {
                                                              self->discardRecovered();
                                                          }
                                                      }});
    m_recoveryNotification = m_shell.notifications().notify(std::move(notification));
    return keys.size();
}

document::RestoreReport Workbench::restoreRecovered() {
    if (!m_recovery) {
        return {};
    }
    document::RestoreReport report = m_services.saveManager.restore(*m_recovery);
    m_recovery.reset();
    int untitled = 0;
    for (document::FileDocument* document : report.restored) {
        if (document->untitled()) {
            ++untitled; // TODO(M2): untitled buffers get their own tabs with the real editor.
            continue;
        }
        static_cast<void>(openFile(document->path()));
    }
    QString summary = text("%1 files were restored.").arg(report.restored.size());
    if (!report.diskChanged.empty()) {
        summary += QStringLiteral(" ") +
                   text("%1 changed on disk since; compare before saving.").arg(report.diskChanged.size());
    }
    if (!report.deferred.empty()) {
        summary += QStringLiteral(" ") + text("%1 could not be reopened and were kept.").arg(report.deferred.size());
    }
    if (untitled > 0) {
        summary += QStringLiteral(" ") + text("%1 untitled buffers were restored without a tab.").arg(untitled);
    }
    m_shell.notifications().notify({text("Unsaved files restored"),
                                    summary,
                                    report.errors.empty() ? NotificationKind::Info : NotificationKind::Warning,
                                    std::nullopt,
                                    {}});
    return report;
}

void Workbench::discardRecovered() {
    if (!m_recovery) {
        return;
    }
    for (storage::RecoveredJournal& journal : m_recovery->journals) {
        if (auto discarded = std::move(journal).discard(); !discarded) {
            log::warn("ui", "cannot discard a hot-exit journal: {}", discarded.error().message());
        }
    }
    m_recovery.reset();
}

// --- Disk changes, merging and saving ---

void Workbench::applyFileChanges(const io::FileChangeBatch& batch) {
    if (m_services.filesChanged) {
        m_services.filesChanged(batch);
    }
    // The listener (handleExternalChange) reports each change the manager finds.
    static_cast<void>(m_services.saveManager.applyChanges(batch));
    bool projectChanged = false;
    for (const io::FileChange& change : batch.changes) {
        if (isUnder(m_root, change.path) && !nav::relativeTo(m_root, change.path).starts_with(".git/")) {
            projectChanged = true;
            break;
        }
    }
    if (projectChanged) {
        refresh();
    }
}

void Workbench::handleExternalChange(const document::ExternalChange& change) {
    if (change.document == nullptr) {
        return;
    }
    const std::string path = change.document->path();
    m_services.editors.refreshBanners(path);
    m_services.documents.notifyChanged(path);
    const QString name = nativePath(path);
    switch (change.kind) {
    case document::ExternalChangeKind::MergePending: {
        Notification notification;
        notification.title = text("%1 changed on disk").arg(name);
        notification.text = text("It has unsaved changes here and was changed by another program. Keep your "
                                 "version, take the one on disk, or compare them to merge.");
        notification.kind = NotificationKind::Warning;
        notification.sticky = true;
        // The document may be closed before the user answers: each action looks it up again.
        const QPointer<Workbench> self(this);
        notification.actions.push_back(NotificationAction{text("Keep my version"), [self, path] {
                                                              if (self != nullptr) {
                                                                  self->resolveExternalChange(
                                                                      path, document::MergeDecision::KeepBuffer);
                                                              }
                                                          }});
        notification.actions.push_back(NotificationAction{text("Take disk version"), [self, path] {
                                                              if (self != nullptr) {
                                                                  self->resolveExternalChange(
                                                                      path, document::MergeDecision::TakeDisk);
                                                              }
                                                          }});
        notification.actions.push_back(NotificationAction{text("Compare..."), [self, path] {
                                                              if (self != nullptr) {
                                                                  static_cast<void>(self->showMerge(path));
                                                              }
                                                          }});
        m_shell.notifications().notify(std::move(notification));
        break;
    }
    case document::ExternalChangeKind::OutOfDate: {
        const bool tooLarge = change.error && change.error->code() == ErrorCode::OutOfRange;
        m_shell.notifications().notify(
            {tooLarge ? text("%1 is too large to reload").arg(name) : text("Cannot reload %1").arg(name),
             change.error ? fromUtf8(change.error->message()) : QString(),
             NotificationKind::Warning,
             std::nullopt,
             {}});
        break;
    }
    case document::ExternalChangeKind::Deleted:
        m_shell.notifications().notify({text("%1 was deleted on disk").arg(name),
                                        text("Saving creates it again."),
                                        NotificationKind::Info,
                                        std::nullopt,
                                        {}});
        break;
    case document::ExternalChangeKind::Failed:
        if (change.error) {
            notifyError(text("Cannot check %1 on disk").arg(name), *change.error);
        }
        break;
    case document::ExternalChangeKind::None:
    case document::ExternalChangeKind::Touched:
    case document::ExternalChangeKind::Reloaded: break;
    }
}

MergeView* Workbench::showMerge(const std::string& path) {
    if (const auto it = m_merges.find(path); it != m_merges.end() && it->second != nullptr) {
        it->second->raise();
        it->second->activateWindow();
        return it->second;
    }
    document::FileDocument* document = m_services.documents.find(path);
    if (document == nullptr || document->pendingMerge() == nullptr) {
        m_shell.notifications().notify({text("Nothing to compare"),
                                        text("%1 has no pending change from disk.").arg(nativePath(path)),
                                        NotificationKind::Info,
                                        std::nullopt,
                                        {}});
        return nullptr;
    }
    auto created = MergeView::create(m_services.saveManager, *document, &m_shell);
    if (!created) {
        notifyError(text("Cannot compare %1").arg(nativePath(path)), created.error());
        return nullptr;
    }
    MergeView* view = *created;
    m_merges[document->path()] = view;
    const std::string key = document->path();
    connect(view, &MergeView::resolved, this, [this, key](const QString&, int) {
        m_services.editors.refreshBanners(key);
        m_services.documents.notifyChanged(key);
    });
    connect(view, &QObject::destroyed, this, [this, key] { m_merges.erase(key); });
    view->show();
    return view;
}

void Workbench::resolveExternalChange(const std::string& path, document::MergeDecision decision) {
    document::FileDocument* document = m_services.documents.find(path);
    if (document == nullptr || document->pendingMerge() == nullptr) {
        return; // Closed, or decided elsewhere (the merge view, another notification).
    }
    if (auto resolved = m_services.saveManager.resolveExternalChange(*document, decision); !resolved) {
        notifyError(text("Cannot resolve the change of %1").arg(nativePath(path)), resolved.error());
    }
    if (const auto it = m_merges.find(document->path()); it != m_merges.end() && it->second != nullptr) {
        it->second->close();
    }
    m_services.editors.refreshBanners(document->path());
    m_services.documents.notifyChanged(document->path());
}

void Workbench::reloadAllFromDisk() {
    // The paths first: the listeners of each change run before the next check.
    std::vector<std::string> paths;
    for (const auto& document : m_services.saveManager.documents()) {
        if (!document->untitled()) {
            paths.push_back(document->path());
        }
    }
    for (const std::string& path : paths) {
        if (document::FileDocument* document = m_services.saveManager.find(path); document != nullptr) {
            handleExternalChange(m_services.saveManager.checkDisk(*document));
        }
    }
}

void Workbench::frameDeactivated() {
    if (m_services.autosave == nullptr) {
        return;
    }
    if (auto saved = m_services.autosave->frameDeactivated(); saved) {
        reportSaveAll(*saved, true);
    }
}

void Workbench::frameActivated() { reloadAllFromDisk(); }

void Workbench::reloadFromDisk(const std::string& path) {
    document::FileDocument* document = m_services.documents.find(path);
    if (document == nullptr) {
        return;
    }
    handleExternalChange(m_services.saveManager.checkDisk(*document));
    m_services.editors.refreshBanners(path);
}

void Workbench::overwriteDisk(const std::string& path) {
    document::FileDocument* document = m_services.documents.find(path);
    if (document == nullptr) {
        return;
    }
    document::SaveOptions options;
    options.overwriteExternalChanges = true;
    if (auto saved = m_services.saveManager.save(*document, options); !saved) {
        notifyError(text("Cannot save %1").arg(nativePath(path)), saved.error());
    }
    m_services.editors.refreshBanners(path);
    m_services.documents.notifyChanged(path);
}

void Workbench::reportSaveAll(const document::SaveAllResult& result, bool quiet) {
    for (document::FileDocument* document : result.saved) {
        m_services.editors.refreshBanners(document->path());
        m_services.documents.notifyChanged(document->path());
    }
    for (const document::FailedSave& failed : result.failed) {
        notifyError(text("Cannot save %1").arg(nativePath(failed.document->path())), failed.error);
    }
    if (quiet) {
        return;
    }
    for (const document::SkippedDocument& skipped : result.skipped) {
        const std::string path = skipped.document->path();
        const QString name = skipped.document->untitled() ? text("An untitled buffer") : nativePath(path);
        switch (skipped.reason) {
        case document::SkipReason::MergePending: {
            Notification notification;
            notification.title = text("%1 was not saved").arg(name);
            notification.text = text("It changed on disk; compare the versions first.");
            notification.kind = NotificationKind::Warning;
            const QPointer<Workbench> self(this);
            notification.actions.push_back(NotificationAction{text("Compare..."), [self, path] {
                                                                  if (self != nullptr) {
                                                                      static_cast<void>(self->showMerge(path));
                                                                  }
                                                              }});
            m_shell.notifications().notify(std::move(notification));
            break;
        }
        case document::SkipReason::OutOfDate:
            m_shell.notifications().notify(
                {text("%1 was not saved").arg(name),
                 text("The file on disk changed; use Overwrite in the editor to save over it."),
                 NotificationKind::Warning,
                 std::nullopt,
                 {}});
            break;
        case document::SkipReason::NeedsConsent:
            m_shell.notifications().notify(
                {text("%1 was not saved").arg(name),
                 text("It was loaded with replacement characters; save it explicitly to accept them."),
                 NotificationKind::Warning,
                 std::nullopt,
                 {}});
            break;
        case document::SkipReason::Untitled:
        case document::SkipReason::Deleted: break;
        }
    }
}

document::SaveAllResult Workbench::saveAll() {
    document::SaveAllResult result = m_services.saveManager.saveAll(document::SaveReason::SaveAll);
    reportSaveAll(result, false);
    return result;
}

Result<void> Workbench::saveDocument() {
    IEditorView* editor = m_shell.tabs().currentEditor();
    if (editor == nullptr) {
        return fail(ErrorCode::NotFound, "no document is active");
    }
    document::FileDocument* document = m_services.documents.find(editor->path());
    if (document == nullptr) {
        return fail(ErrorCode::NotFound, "'" + editor->path() + "' is not open as a document");
    }
    auto saved = m_services.saveManager.save(*document);
    m_services.editors.refreshBanners(document->path());
    m_services.documents.notifyChanged(document->path());
    if (!saved) {
        notifyError(text("Cannot save %1").arg(nativePath(document->path())), saved.error());
        return std::unexpected(std::move(saved.error()));
    }
    return {};
}

void Workbench::openSettingsFile(const std::string& path) {
    if (path.empty()) {
        return;
    }
    if (auto exists = m_services.fs.exists(path); exists && !*exists) {
        // A settings file starts as an empty object.
        static_cast<void>(m_services.fs.createDirectories(std::string(nav::parentOf(path))));
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
