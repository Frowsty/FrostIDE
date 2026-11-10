// SPDX-License-Identifier: Apache-2.0
//
// Workbench: the shell's navigation slice (PLAN.md section 7 M1 "shell navigation", ADR 0042),
// attached to a ShellWindow. It owns the open project (a folder, or a solution's folder plus its
// files), the File System tool window, Find Action, Go to File, Search Everywhere, Recent Files and
// Recent Locations, back/forward over the navigation history, per-project session restore, the
// hot-exit restore prompt, external-change banners and the merge view, the large-file guard's
// notifications, Save All / Save Document and the autosave hooks. Since the W1 unification (ADR 0044,
// "Unified with shell-nav") it is the IDE's one owner of these: the composition root
// (frost::app::Workbench) creates it in attach() and routes the project commands through its hooks.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/document/autosave.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/input/find_action.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/project/directory_roles.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/ui/nav/document_registry.hpp"
#include "frost/ui/nav/navigation_history.hpp"
#include "frost/ui/nav/project_files.hpp"
#include "frost/ui/nav/session.hpp"
#include "frost/ui/shell/popup.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/workbench/document_editor.hpp"
#include "frost/ui/workbench/file_system_view.hpp"
#include "frost/ui/workbench/merge_view.hpp"

#include <QListWidget>
#include <QObject>
#include <QPointer>
#include <QString>

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class QTimer;

namespace frost::ui {

// The ids the workbench registers (none is reserved by another slice, ADR 0042).
namespace workbench_actions {
inline constexpr std::string_view kNewFile = "file.new";
inline constexpr std::string_view kNewFolder = "file.newFolder";
inline constexpr std::string_view kRename = "file.rename";
inline constexpr std::string_view kDelete = "file.delete";
inline constexpr std::string_view kFindAction = "search.findAction";
inline constexpr std::string_view kSearchEverywhere = "search.everywhere";
inline constexpr std::string_view kGoToFile = "navigate.gotoFile";
inline constexpr std::string_view kRecentFiles = "navigate.recentFiles";
inline constexpr std::string_view kRecentLocations = "navigate.recentLocations";
inline constexpr std::string_view kLastEditLocation = "navigate.lastEditLocation";
inline constexpr std::string_view kBack = "navigate.back";
inline constexpr std::string_view kForward = "navigate.forward";
inline constexpr std::string_view kSelectIn = "navigate.selectIn";
inline constexpr std::string_view kToggleBookmark = "bookmarks.toggle";
inline constexpr std::string_view kToggleMnemonicBookmark = "bookmarks.toggleMnemonic";
inline constexpr std::string_view kToggleBookmark1 = "bookmarks.toggle1";
inline constexpr std::string_view kToggleBookmark2 = "bookmarks.toggle2";
inline constexpr std::string_view kToggleBookmark3 = "bookmarks.toggle3";
inline constexpr std::string_view kToggleBookmark4 = "bookmarks.toggle4";
inline constexpr std::string_view kToggleBookmark5 = "bookmarks.toggle5";
inline constexpr std::string_view kToggleBookmark6 = "bookmarks.toggle6";
inline constexpr std::string_view kToggleBookmark7 = "bookmarks.toggle7";
inline constexpr std::string_view kToggleBookmark8 = "bookmarks.toggle8";
inline constexpr std::string_view kToggleBookmark9 = "bookmarks.toggle9";
inline constexpr std::string_view kGotoBookmark1 = "bookmarks.goto1";
inline constexpr std::string_view kGotoBookmark2 = "bookmarks.goto2";
inline constexpr std::string_view kGotoBookmark3 = "bookmarks.goto3";
inline constexpr std::string_view kGotoBookmark4 = "bookmarks.goto4";
inline constexpr std::string_view kGotoBookmark5 = "bookmarks.goto5";
inline constexpr std::string_view kGotoBookmark6 = "bookmarks.goto6";
inline constexpr std::string_view kGotoBookmark7 = "bookmarks.goto7";
inline constexpr std::string_view kGotoBookmark8 = "bookmarks.goto8";
inline constexpr std::string_view kGotoBookmark9 = "bookmarks.goto9";
inline constexpr std::string_view kShowBookmarks = "bookmarks.show";
inline constexpr std::string_view kBookmarksWindow = "window.bookmarks";
inline constexpr std::string_view kNextBookmark = "bookmarks.next";
inline constexpr std::string_view kPreviousBookmark = "bookmarks.previous";
inline constexpr std::string_view kSaveAll = "file.saveAll";
inline constexpr std::string_view kSaveDocument = "file.saveDocument";
inline constexpr std::string_view kSaveAs = "file.saveAs";
inline constexpr std::string_view kReloadFromDisk = "file.reloadFromDisk";
inline constexpr std::string_view kCloseProject = "file.closeProject";
inline constexpr std::string_view kFileSystemWindow = "fileSystem.show";
inline constexpr std::string_view kTodoWindow = "window.todo";
inline constexpr std::string_view kOpenApplicationSettings = "settings.openApplicationFile";
inline constexpr std::string_view kOpenProjectSettings = "settings.openProjectFile";
inline constexpr std::string_view kOpenPersonalProjectSettings = "settings.openPersonalProjectFile";
} // namespace workbench_actions

// The File System and TODO tool windows' ids.
inline constexpr std::string_view kFileSystemToolWindow = "fileSystem";
inline constexpr std::string_view kTodoToolWindow = "todo";

enum class SearchEverywhereCategory {
    All = 0,
    Classes = 1,
    Files = 2,
    Symbols = 3,
    Actions = 4,
};

struct WorkbenchSymbol {
    std::string name;
    std::string container;
    std::string path;
    int line = 1;
    int column = 1;
    bool isClassLike = false;
};

/*
 * What the workbench works with. References outlive the workbench; pointers may be null.
 * `editors` must be the factory the shell's ShellEnvironment uses.
 */
struct WorkbenchServices {
    io::IFileSystem& fs;
    document::SaveManager& saveManager;
    nav::DocumentRegistry& documents;
    DocumentEditorFactory& editors;
    const IClock& clock;
    // Runs project scans and the watcher pump off the UI thread; null: scans run on the UI thread
    // (tests) and there is no watcher.
    sched::IScheduler* scheduler = nullptr;
    // Watches the project folder (needs `scheduler`); null: refresh() only.
    io::IFileWatcher* watcher = nullptr;
    // Session restore per project; null: none.
    nav::SessionStore* sessions = nullptr;
    // The persisted directory role overrides ("Mark Directory as", UX-23); null: the defaults only
    // and the menu is hidden.
    project::DirectoryRoleStore* directoryRoles = nullptr;
    // Fed by focus loss, input and a timer; null: no autosave.
    document::AutosaveController* autosave = nullptr;
    // settings.openApplicationFile's target; empty: the action is disabled.
    std::string applicationSettingsFile{};
    // The composition root's project commands (ADR 0044); empty: the workbench's own. file.closeProject
    // runs closeProjectHook, which closes through closeProject() itself; restoreLastProject() opens the
    // last root through openProjectHook (which may map it to the solution the user opened).
    std::function<void()> closeProjectHook{};
    std::function<Result<void>(const std::string& root)> openProjectHook{};
    // Receives the same watcher batches as the document and project views, on the GUI thread.
    std::function<void(const io::FileChangeBatch&)> filesChanged{};
    // Prompts for a relative new file or folder name under `directory`; "" cancels.
    std::function<std::string(const std::string& directory, bool folder)> promptNewItem{};
    // Optional override for determining the default directory when creating a file/folder.
    std::function<std::string()> defaultNewItemDirectory{};
    // Notified after a new file has been created on disk and opened in an editor tab.
    std::function<void(const std::string& path)> fileCreated{};
    // Prompts for a Save As target path given `currentPath`; "" cancels.
    std::function<std::string(const std::string& currentPath)> promptSaveAsPath{};
    // Optional composition-root override for Save As (runs LSP/editor rebinding too).
    std::function<void()> saveAsHook{};
    // Prompts for a new name when renaming `path`; "" cancels.
    std::function<std::string(const std::string& path, bool folder)> promptRename{};
    // Prompts for confirmation when deleting `path`; false cancels.
    std::function<bool(const std::string& path, bool folder)> confirmDelete{};
    // Optional composition-root overrides for Rename / Delete (also update CMakeLists/vcxproj/csproj and LSP).
    std::function<void(const std::string& path)> renamePathHook{};
    std::function<void(const std::string& path)> deletePathHook{};
    // Async workspace symbol provider for Search Everywhere (Classes / Symbols / All tabs).
    std::function<void(std::string_view query, bool classesOnly,
                       std::function<void(std::vector<WorkbenchSymbol>)> done)>
        queryWorkspaceSymbols{};
};

/*
 * The workbench. GUI thread only; construct it after the shell and destroy it before the shell
 * tears down: it is not a QObject child of the shell (its destructor saves the session and stops the
 * watcher through the shell and the services), and its actions' handlers check that it is alive.
 * Every open entry point (File System view, Go to File, Search Everywhere, Ctrl+E, Ctrl+Shift+E,
 * back/forward, session restore, the command line) goes through openFile(), so the large-file
 * guard's refusal is one "... is too large to open" notification on each and the path is
 * canonicalized once.
 */
class Workbench final : public QObject {
    Q_OBJECT

public:
    Workbench(ShellWindow& shell, WorkbenchServices services);
    ~Workbench() override;
    Workbench(const Workbench&) = delete ("the shell's actions refer to the workbench");
    Workbench& operator=(const Workbench&) = delete ("the shell's actions refer to the workbench");

    // --- Project ---

    /*
     * Opens a folder, or a .sln/.slnx (its folder is the root, its files join Go to File). Saves the
     * current project's session and closes its tabs first, then restores the new one's (when a store
     * is set). Errors: NotFound, InvalidArgument (not a folder or solution), the solution parser's,
     * Cancelled (a tab of the current project refused to close: its save failed and the user kept it;
     * the current project stays open).
     */
    [[nodiscard]] Result<void> openProject(const std::string& path);
    // As openProject() for a solution the caller has loaded already (the composition root's).
    [[nodiscard]] Result<void> openSolution(const project::SolutionModel& solution);
    // Refreshes navigation for the already-open solution without closing or saving tabs.
    [[nodiscard]] Result<void> refreshSolution(const project::SolutionModel& solution);
    /*
     * Saves the session and closes every tab; no project afterwards. Returns false, keeping the
     * project and the tabs that refused, when a tab refused to close (a dirty tab whose save failed
     * and the user cancelled), as Rider keeps the project open then.
     */
    bool closeProject();
    [[nodiscard]] const std::string& projectRoot() const noexcept { return m_root; }
    [[nodiscard]] const nav::ProjectFiles& projectFiles() const noexcept { return *m_files; }
    // Rescans the project folder (on the scheduler when set, else now).
    void refresh();
    // True while a background scan runs.
    [[nodiscard]] bool scanPending() const noexcept { return m_scanPending; }
    // True once a full scan for the current project has been published.
    [[nodiscard]] bool initialScanReady() const noexcept { return !m_root.empty() && m_scanPublished; }
    /*
     * The open project's directory roles (frost/project/directory_roles.hpp, UX-23): the project
     * scan (the tree and Go to File), the file watcher and Find in Files traverse only the folders
     * they mark Source or Library. Built when a project opens (reads its CMake presets and the
     * overrides) and again when CMakePresets.json, CMakeUserPresets.json or a CMakeCache.txt
     * changes or an override is set; null without a project. Shared, immutable apart from its
     * internal cache; safe to use on any thread.
     */
    [[nodiscard]] std::shared_ptr<const project::DirectoryRoles> directoryRoles() const noexcept { return m_roles; }
    /*
     * Sets (or with nullopt removes) the user's role for the project folder `directory` and saves
     * it in the directory role store, then rebuilds the roles, restarts the watcher and rescans.
     * Errors: InvalidArgument (no project, no store, or a folder outside the project), the store's.
     */
    [[nodiscard]] Result<void> setDirectoryRole(const std::string& directory,
                                                std::optional<project::DirectoryRole> role);

    // --- Opening and navigation ---

    // Opens `path` in a tab (through ShellWindow::openFile), moves to `at` when given, and records
    // the place left as a jump plus the recent file. Errors: the shell's (already notified).
    Result<int> openFile(const std::string& path, std::optional<LinePosition> at = std::nullopt,
                         bool statusColumn = false);
    // The active editor's place, or an empty place.
    [[nodiscard]] nav::Place currentPlace() const;
    bool goBack();
    bool goForward();
    [[nodiscard]] nav::NavigationHistory& history() noexcept { return m_history; }
    [[nodiscard]] nav::RecentFiles& recentFiles() noexcept { return m_recentFiles; }
    [[nodiscard]] nav::RecentLocations& recentLocations() noexcept { return m_recentLocations; }

    // The popups; each returns the open popup (owned by the shell) or nullptr when it does not apply.
    ListPopup* showFindAction(const QString& query = {});
    ListPopup* showGoToFile(const QString& query = {});
    ListPopup* showSearchEverywhere(const QString& query = {},
                                    SearchEverywhereCategory category = SearchEverywhereCategory::All);
    ListPopup* showRecentFiles();
    ListPopup* showRecentLocations();

    // Last Edit Location (Ctrl+Shift+Backspace).
    void recordEditLocation(const std::string& path, int line, int column);
    bool goToLastEditLocation();
    [[nodiscard]] std::optional<nav::Place> lastEditLocation() const noexcept { return m_lastEditPlace; }

    // Bookmarks (F11 / Shift+F11 / Ctrl+Shift+1..9 / Ctrl+1..9).
    bool toggleBookmark();
    bool toggleBookmarkAt(const std::string& path, int line);
    bool toggleNumberedBookmark(int digit);
    bool toggleNumberedBookmarkAt(int digit, const std::string& path, int line);
    bool goToNumberedBookmark(int digit);
    [[nodiscard]] std::optional<nav::Place> numberedBookmark(int digit) const noexcept;
    ListPopup* showMnemonicBookmarkPopup();
    bool goToNextBookmark();
    bool goToPreviousBookmark();
    ListPopup* showBookmarksPopup();
    [[nodiscard]] std::vector<nav::Place> bookmarks() const;

    // Select active file in project tree (Alt+F1).
    bool selectInProjectView();

    // Task comment list tool window.
    void refreshTodoItems();
    [[nodiscard]] QListWidget* todoListWidget() const noexcept { return m_todoList; }

    // --- Sessions ---

    [[nodiscard]] nav::SessionState captureSession() const;
    [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& cmakeTargets() const noexcept {
        return m_cmakeTargets;
    }
    void setCMakeTargets(std::vector<std::pair<std::string, std::string>> targets) {
        m_cmakeTargets = std::move(targets);
    }
    [[nodiscard]] const std::vector<nav::SessionCMakeArguments>& cmakeArguments() const noexcept {
        return m_cmakeArguments;
    }
    void setCMakeArguments(std::vector<nav::SessionCMakeArguments> arguments) {
        m_cmakeArguments = std::move(arguments);
    }
    // Reopens the tabs (order, pinned, active, caret and scroll), the tool-window layout and the
    // recents. Files that fail to open are skipped (each notified).
    void applySession(const nav::SessionState& state);
    // Saves the current project's session (no store or no project: nothing). Errors: the store's.
    [[nodiscard]] Result<void> saveSession();
    // Opens the store's last project (through openProjectHook when set), if there is one and it still
    // exists. Returns whether it did.
    bool restoreLastProject();

    // --- Hot exit ---

    /*
     * Offers the scan's unsaved buffers: a sticky notification "FrostIDE closed unexpectedly.
     * Restore N unsaved files?" with Restore (restoreRecovered()) and Discard. Returns N (0: nothing
     * to offer, no notification; the scan's empty journals are discarded). The IDE's one restore
     * prompt: the composition root scans once per start and calls this.
     */
    std::size_t offerRestore(storage::RecoveryScan scan);
    // Runs SaveManager::restore on the offered scan and opens the restored documents in tabs.
    document::RestoreReport restoreRecovered();
    // Discards the offered journals.
    void discardRecovered();
    [[nodiscard]] bool restorePending() const noexcept { return m_recovery.has_value(); }

    // --- Disk changes and saving ---

    // SaveManager::applyChanges for a watcher batch, then the banners, notifications and (for
    // changes under the project folder) a rescan.
    void applyFileChanges(const io::FileChangeBatch& batch);
    /*
     * Shows what one external change means: banners and registry listeners for every change (a
     * clean buffer was reloaded silently); for a dirty buffer (MergePending) a sticky notification
     * with "Keep my version", "Take disk version" and "Compare..." (the merge view), as Rider asks.
     */
    void handleExternalChange(const document::ExternalChange& change);
    // Resolves `path`'s pending merge with `decision` (the notification's first two actions).
    void resolveExternalChange(const std::string& path, document::MergeDecision decision);
    // Reload All from Disk (file.reloadFromDisk, Ctrl+Alt+Y): checkDisk for every open file document.
    void reloadAllFromDisk();
    // The IDE frame lost focus (autosave on frame deactivation) or got it back (the disk check).
    void frameDeactivated();
    void frameActivated();
    // The merge view for `path`'s pending merge (one per document), or nullptr with a notification.
    MergeView* showMerge(const std::string& path);
    // The OutOfDate banner's actions: check the disk again / save over it.
    void reloadFromDisk(const std::string& path);
    void overwriteDisk(const std::string& path);
    // Save All (Ctrl+S): saved, skipped and failed documents are reported in one notification.
    document::SaveAllResult saveAll();
    // Saves the active editor's document. Errors: the save's (also notified).
    [[nodiscard]] Result<void> saveDocument();
    // Saves the active editor's document to `targetPath` and updates its tab/registry identity.
    [[nodiscard]] Result<void> saveDocumentAs(std::string_view targetPath);
    // Updates navigation state (active path, recent files, places, and folder scan) after Save As or Rename.
    void notifyPathRenamed(const std::string& oldPath, const std::string& newPath);
    // Purges navigation state (recent files, places, bookmarks, history) and refreshes after Delete.
    void notifyPathDeleted(const std::string& deletedPath, bool directory);

    // --- File and folder creation, rename, and deletion ---

    // Default directory for New File / New Folder (selected tree directory, active file's parent, or root).
    [[nodiscard]] std::string defaultNewItemDirectory() const;
    // Creates a new empty file at `directory`/`relativeName`, selects it in File System, and opens it.
    [[nodiscard]] Result<std::string> createNewFile(std::string_view directory, std::string_view relativeName);
    // Creates a new directory at `directory`/`relativeName` and selects it in File System.
    [[nodiscard]] Result<std::string> createNewFolder(std::string_view directory, std::string_view relativeName);
    // Prompts for a name and creates a new file or folder in `directory` (empty: defaultNewItemDirectory()).
    void promptAndCreateNewFile(std::string_view directory = {});
    void promptAndCreateNewFolder(std::string_view directory = {});
    // Renames `oldPath` to `newRelativeName` in the same parent folder and rebinds open documents/navigation.
    [[nodiscard]] Result<std::string> renamePath(std::string_view oldPath, std::string_view newRelativeName);
    // Deletes `path` (file or folder) on disk, closes any open tabs under it, and updates navigation.
    [[nodiscard]] Result<void> deletePath(std::string_view path);
    // Prompts and renames/deletes `path` (empty: currently selected File System path or active editor path).
    void promptAndRenamePath(std::string_view path = {});
    void promptAndDeletePath(std::string_view path = {});

    [[nodiscard]] FileSystemView* fileSystemView() const noexcept { return m_fileSystem; }
    [[nodiscard]] QTimer* autosaveTimer() const noexcept { return m_autosaveTimer; }
    [[nodiscard]] input::FindActionModel& findActions() noexcept { return m_findActions; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct WatchState;
    struct TodoEntry {
        std::string path;
        std::string relativePath;
        std::string snippet;
        int line = 1;
        int column = 1;
    };

    void registerActions();
    void addAction(std::string_view id, std::string_view name, std::string_view group, std::function<void()> run,
                   std::function<bool()> enabled = {}, std::vector<std::string> synonyms = {});
    void addToolWindow();
    void addTodoToolWindow();
    void applyTodoEntries(const std::vector<TodoEntry>& entries);
    void syncEditorBookmarks(const std::string& path);
    [[nodiscard]] std::string defaultTargetPathForModify() const;
    [[nodiscard]] Result<void> openRoot(std::string root, std::string solution, std::vector<std::string> solutionFiles);
    void setRoot(std::string root, std::vector<std::string> solutionFiles);
    void applyScan(Result<std::shared_ptr<const nav::ProjectFiles>> scan);
    void pollScan();
    void startInitialFiles();
    void pollInitialFiles();
    void startWatching();
    void stopWatching();
    // Rebuilds m_roles for m_root (directoryRoles()); does not rescan or restart the watcher.
    void loadDirectoryRoles();
    void onEditorStatus(IEditorView* editor);
    void onCurrentEditorChanged(IEditorView* editor);
    void openSettingsFile(const std::string& path);
    [[nodiscard]] std::optional<std::string> projectSettingsFile(bool personal) const;
    void notifyError(const QString& title, const Error& error);
    void notifySaveWarning(const std::string& path, const Error& warning);
    void armAutosave();
    void pollAutosave();
    [[nodiscard]] QString shortcutText(std::string_view actionId) const;
    [[nodiscard]] std::vector<ListPopupItem> fileItems(const std::string& pattern, std::size_t limit) const;
    [[nodiscard]] std::vector<ListPopupItem> actionItems(const std::string& query, const input::ActionContext& context,
                                                         std::size_t limit) const;
    void closeOpenPopup();
    void acceptItem(const QVariant& data, const input::ActionContext& captured);
    bool openPlace(const nav::Place& place);
    void reportSaveAll(const document::SaveAllResult& result, bool quiet);

    ShellWindow& m_shell;
    WorkbenchServices m_services;
    bool m_saveWarningNeedsReview = false;
    std::vector<std::pair<std::string, std::string>> m_cmakeTargets;
    std::vector<nav::SessionCMakeArguments> m_cmakeArguments;
    input::FindActionModel m_findActions;
    std::string m_root;
    std::string m_solution;
    std::shared_ptr<const project::DirectoryRoles> m_roles;
    std::shared_ptr<const nav::ProjectFiles> m_files = std::make_shared<nav::ProjectFiles>();
    nav::NavigationHistory m_history;
    nav::RecentFiles m_recentFiles;
    nav::RecentLocations m_recentLocations;
    std::optional<nav::Place> m_lastEditPlace;
    std::map<std::string, std::set<int>, std::less<>> m_bookmarks;
    std::array<std::optional<nav::Place>, 9> m_numberedBookmarks{};
    QPointer<FileSystemView> m_fileSystem;
    QPointer<QListWidget> m_todoList;
    std::map<std::string, QPointer<MergeView>, std::less<>> m_merges;
    std::optional<storage::RecoveryScan> m_recovery;
    std::uint64_t m_recoveryNotification = 0;
    // The caret of every open document at its last status (by path), for big-move detection.
    std::map<std::string, nav::Place, std::less<>> m_lastPlaces;
    std::string m_activePath;
    std::shared_ptr<const std::vector<std::string>> m_solutionFiles =
        std::make_shared<const std::vector<std::string>>();
    std::uint64_t m_projectGeneration = 0;
    std::uint64_t m_scanGeneration = 0;
    // A background scan.
    std::optional<sched::JoinHandle<Result<std::shared_ptr<const nav::ProjectFiles>>>> m_scan;
    std::stop_source m_scanStop;
    std::optional<sched::JoinHandle<nav::ProjectFiles>> m_initialFiles;
    std::stop_source m_initialStop;
    std::uint64_t m_initialGeneration = 0;
    bool m_scanPublished = false;
    QTimer* m_scanTimer = nullptr;
    bool m_scanPending = false;
    bool m_rescanQueued = false;
    std::optional<sched::JoinHandle<std::vector<TodoEntry>>> m_todoScan;
    std::stop_source m_todoStop;
    std::uint64_t m_todoScanGeneration = 0;
    bool m_todoRescanQueued = false;
    std::unique_ptr<WatchState> m_watch;
    QTimer* m_autosaveTimer = nullptr;
    bool m_restoring = false;
    bool m_navigating = false;
};

} // namespace frost::ui
