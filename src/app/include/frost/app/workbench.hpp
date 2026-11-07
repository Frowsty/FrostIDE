// SPDX-License-Identifier: Apache-2.0
//
// Workbench: FrostIDE's composition root for one IDE window (W1, ADR 0044, "Unified with
// shell-nav"). It wires the shell (frost::ui_shell), the editor (frost::ui_editor), the navigation
// workbench (frost::ui_workbench: documents, Go to File, sessions, hot exit, external changes,
// saving), the build and run tool windows (frost::ui_toolwindows, frost::ui_run), the language
// features (frost::ui_lang over clangd, frost::lang) and the settings store (frost::settings) into
// the window the user works in. There is one of each: this is the only composition root.
/*
 * Lifecycle. Construct the workbench first (it owns the registry, the keymap, the settings
 * adapters, the save manager, the document registry and the editor factories the window is built
 * on), install the appearance, build the window from environment(), then attach() it; attach()
 * creates the navigation workbench, the tool windows and the language controller and registers a
 * teardown hook that removes them while the window is whole. After the window is shown,
 * restoreLastProject() reopens the last project with its tabs (general.reopenLastProject) and
 * offerRecovery() offers the unsaved files an earlier session left, once. shutdown() (also run by
 * the destructor) saves the session, autosaves, writes and closes the hot-exit journal and stops
 * clangd and the runs; the window must be destroyed before the workbench.
 *
 * Documents (ADR 0042, ADR 0044). One document per file: every path goes through
 * IFileSystem::canonicalPath (the real case on Windows, realpath for ".." on Linux) in the
 * DocumentRegistry, and every caller opens files through the navigation workbench's openFile():
 * the command line, File > Open, the Welcome screen, the Solution Explorer, the File System view,
 * Problems, the Build output, Go to File, Search Everywhere, the recent files, back/forward,
 * session restore, the restore prompt and the language features. A tab is the registry's banner
 * view around the real editor (DocumentEditorView).
 *
 * Opening (openPath). A .sln or .slnx opens as the project: the solution model fills the Solution
 * Explorer, the build and run services open it (the configuration and startup project combos sit
 * in the main toolbar) and clangd starts on it (compile commands: solutionCompileCommandsChain, an
 * existing compile_commands.json, then MSBuild's evaluation when trusted, then the synthesized
 * ones). A folder opens the one solution in it, else the folder itself as the project, shown in the
 * File System view (clangd on its compile_commands.json). Any other file opens in a tab. Opening a
 * project closes the previous one (and is cancelled when one of its tabs refuses to close); the
 * window title is "<project> - FrostIDE". Switching the solution configuration regenerates the
 * compile commands and restarts clangd.
 *
 * Saving (ADR 0044). Ctrl+S is Rider's Save All (file.saveAll, the navigation workbench's one
 * registration). Closing a dirty tab saves it first and asks only when that save fails; Cancel keeps
 * the tab, and Close Project then keeps the project open. Autosave runs on frame deactivation and
 * after the idle delay (save.autosave.*), the hot-exit journal shortly after input stops and at
 * exit. Every build and run saves all first (the build tool windows' Save-All hook).
 *
 * External changes are checked when the window becomes active, by the file watcher and by Reload
 * All from Disk: a clean buffer reloads silently, a dirty one gets "Keep my version", "Take disk
 * version" and "Compare..." (the merge view).
 *
 * Auto-show. The Build tool window (bottom) shows when a build starts; Problems shows after a build
 * that reported errors; Run shows when a run starts. None takes focus.
 *
 * Diagnostics. Build diagnostics become gutter markers of the open editors (refreshed on every
 * build and when a document opens); clangd's live diagnostics go to the Problems window's Live rows
 * and to the editor as gutter markers and as its live diagnostics (EditorView::setDiagnostics with
 * EditorDiagnosticSource::Live: squiggles, error-stripe marks and the inspection widget's counts, ADR
 * 0047); clangd's usage highlights go to EditorView::setUsageHighlights. The editor keeps and maps
 * those ranges itself, so the workbench holds no decorations of its own.
 *
 * Threads: GUI thread only. clangd's, the run service's and the settings store's callbacks are
 * marshalled onto it. TODO(M1): the save manager's disk work still runs on the GUI thread.
 */
#pragma once

#include "frost/app/document_editor.hpp"
#include "frost/app/settings_adapters.hpp"
#include "frost/app/welcome_screen.hpp"
#include "frost/build/build_service.hpp"
#include "frost/build/run_service.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/debug/breakpoints.hpp"
#include "frost/document/autosave.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/input/action.hpp"
#include "frost/input/keymap_service.hpp"
#include "frost/input/layout.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/diagnostics.hpp"
#include "frost/lang/json.hpp"
#include "frost/lang/lua_service.hpp"
#include "frost/lang/managed_service.hpp"
#include "frost/lang/msbuild_compile_commands.hpp"
#include "frost/lang/workspace_edit.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/sched/latest_wins.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/settings/store.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/storage/local_history.hpp"
#include "frost/syntax/language.hpp"
#include "frost/term/pty.hpp"
#include "frost/term/shell.hpp"
#include "frost/ui/appearance_manager.hpp"
#include "frost/ui/debug_controller.hpp"
#include "frost/ui/editor/editor_actions.hpp"
#include "frost/ui/nav/document_registry.hpp"
#include "frost/ui/nav/session.hpp"
#include "frost/ui/qt_os_appearance.hpp"
#include "frost/ui/run_console.hpp"
#include "frost/ui/shell/background_tasks.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/toolwindows/build_actions.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/build_output.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/save_all_handler.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"
#include "frost/ui/workbench/document_editor.hpp"
#include "frost/ui/workbench/workbench.hpp"
#include "frost/update/manifest.hpp"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QApplication;
class QDialog;
class QPlainTextEdit;
class QWidget;

namespace frost::build {
class CMakeService;
}
namespace frost::ui {
class RunController;
class TrustPrompter;
class FindFeature;
class TrustDialog;
class TerminalIntegration;
} // namespace frost::ui

namespace frost::ui::lang {
class LanguageController;
} // namespace frost::ui::lang

class QTemporaryFile;

namespace frost::app {
class ManagedDebugPreparation;
struct ManagedSnapshotJob;
struct ManagedSourceSnapshot;

class LanguageBridge;
class CMakeIntegration;
class ProjectEditProcess;
class EngineIntegration;
class UnityControlIntegration;
class GitIntegration;
struct GitClone;

// The workbench's own action ids (the navigation workbench owns file.saveAll, file.saveDocument,
// file.reloadFromDisk and file.closeProject; the shell, editor, build, run and language slices the
// others).
namespace workbench_actions {
inline constexpr std::string_view kNewProject = "file.newProject";
inline constexpr std::string_view kOpenFolder = "file.openFolder";
inline constexpr std::string_view kOpenRecent = "file.openRecent";
} // namespace workbench_actions

enum class NewProjectTemplate : std::uint8_t {
    CMakeExecutable,
    CMakeStaticLibrary,
    CMakeSharedLibrary,
    VsCppConsole,
    SlnxCppConsole,
    VsCppStaticLibrary,
    VsCppDynamicLibrary,
    DotNetConsole,
    DotNetClassLibrary,
    EmptyFolder,
};

struct NewProjectSpec {
    std::string name;
    std::string location;
    NewProjectTemplate kind = NewProjectTemplate::CMakeExecutable;
};

// The ids of the Build output and Find Usages tool windows (bottom); Run is the shell's "run".
inline constexpr std::string_view kBuildToolWindow = "build";
inline constexpr std::string_view kFindUsagesToolWindow = "findUsages";
inline constexpr std::string_view kNuGetToolWindow = "nuget";
inline constexpr std::string_view kAsmViewerToolWindow = "asmViewer";
inline constexpr std::string_view kUnitTestsToolWindow = "unitTests";

// A place in a file: 1-based line and column; line 0 leaves the caret where it is (a new document:
// at the start), column 0 is column 1.
struct FileLocation {
    int line = 0;
    int column = 0;
};

[[nodiscard]] bool isLuaSource(std::string_view path) noexcept;
[[nodiscard]] bool isManagedSource(std::string_view path) noexcept;

// Whether clangd handles `path`: a C, C++, Objective-C or CUDA source or header by extension (ASCII
// case ignored). O(path length).
[[nodiscard]] bool isCFamilySource(std::string_view path) noexcept;
// Whether `path` names a Visual Studio solution (.sln or .slnx, ASCII case ignored).
[[nodiscard]] bool isSolutionFile(std::string_view path) noexcept;

// The user's answers the workbench needs: dialogs in the GUI, scripted in tests. GUI thread.
class IWorkbenchPrompts {
public:
    enum class OpenKind : std::uint8_t { SolutionOrFile, Folder };
    // The path chosen in File > Open (a .sln, .slnx or any file) or Open Folder; "" when cancelled.
    [[nodiscard]] virtual std::string chooseOpenPath(QWidget* parent, OpenKind kind) = 0;
    // The destination path chosen in File > Save As; "" when cancelled.
    [[nodiscard]] virtual std::string chooseSaveAsPath(QWidget* parent, const std::string& currentPath) {
        (void)parent;
        (void)currentPath;
        return {};
    }
    // The relative file or folder name chosen in New File / New Folder; "" when cancelled.
    [[nodiscard]] virtual std::string chooseNewItemName(QWidget* parent, const std::string& directory, bool folder) {
        (void)parent;
        (void)directory;
        (void)folder;
        return {};
    }
    // The new relative name chosen when renaming `currentPath`; "" when cancelled.
    [[nodiscard]] virtual std::string chooseRenameName(QWidget* parent, const std::string& currentPath, bool folder) {
        (void)parent;
        (void)currentPath;
        (void)folder;
        return {};
    }
    // Confirm deleting `name` (file or folder) from disk: true deletes it.
    [[nodiscard]] virtual bool confirmDelete(QWidget* parent, const QString& name, bool folder) {
        (void)parent;
        (void)name;
        (void)folder;
        return false;
    }
    // The specification chosen in File > New Project; nullopt when cancelled.
    [[nodiscard]] virtual std::optional<NewProjectSpec> chooseNewProject(QWidget* parent,
                                                                         const std::string& defaultLocation) {
        (void)parent;
        (void)defaultLocation;
        return std::nullopt;
    }
    // A dirty tab is closing and saving it failed with `error`: true closes it, losing the changes.
    [[nodiscard]] virtual bool confirmDiscard(QWidget* parent, const QString& file, const QString& error) = 0;
    // `project` is running: true stops and reruns it (Rider's "stop and rerun?").
    [[nodiscard]] virtual bool confirmRerun(QWidget* parent, const QString& project) = 0;
    // Restore the recorded pre-edit definitions after an interrupted owned project edit.
    [[nodiscard]] virtual bool confirmProjectRecovery(QWidget* parent, const QString& project) = 0;
    // The IDE window is closing and general.confirmExit is on: true exits.
    [[nodiscard]] virtual bool confirmExit(QWidget* parent) {
        (void)parent;
        return true;
    }

protected:
    IWorkbenchPrompts() = default;
    IWorkbenchPrompts(const IWorkbenchPrompts&) = default;
    IWorkbenchPrompts& operator=(const IWorkbenchPrompts&) = default;
    ~IWorkbenchPrompts() = default;
};

// The GUI's prompts: QFileDialog and QMessageBox, modal over `parent`.
class DialogWorkbenchPrompts final : public IWorkbenchPrompts {
public:
    [[nodiscard]] std::string chooseOpenPath(QWidget* parent, OpenKind kind) override;
    [[nodiscard]] std::string chooseSaveAsPath(QWidget* parent, const std::string& currentPath) override;
    [[nodiscard]] std::string chooseNewItemName(QWidget* parent, const std::string& directory, bool folder) override;
    [[nodiscard]] std::string chooseRenameName(QWidget* parent, const std::string& currentPath, bool folder) override;
    [[nodiscard]] bool confirmDelete(QWidget* parent, const QString& name, bool folder) override;
    [[nodiscard]] std::optional<NewProjectSpec> chooseNewProject(QWidget* parent,
                                                                 const std::string& defaultLocation) override;
    [[nodiscard]] bool confirmDiscard(QWidget* parent, const QString& file, const QString& error) override;
    [[nodiscard]] bool confirmRerun(QWidget* parent, const QString& project) override;
    [[nodiscard]] bool confirmProjectRecovery(QWidget* parent, const QString& project) override;
    [[nodiscard]] bool confirmExit(QWidget* parent) override;
};

// What the workbench borrows; everything outlives it.
struct WorkbenchServices {
    io::IFileSystem& fs;
    io::IProcessLauncher& launcher;
    IClock& clock;
    settings::SettingsStore& settings;
    project::TrustStore& trust;
    IWorkbenchPrompts& prompts;
};

struct ManagedDebugSelection {
    std::string project;
    std::string dotnet;
    std::string configuration;
    std::string framework;
    std::string runtimeIdentifier;
    std::string adapter;
};

struct WorkbenchOptions {
    // Per-user project edit lineage directory (absolute); empty disables project edits.
    std::string projectEditStateDirectory;
    // Packaged Windows ProjectHost executable; empty uses project-host beside the application.
    std::string projectEditHostPath;
    // This process's hot-exit journal and the recovery scan (absolute); "" disables hot exit.
    std::string journalDirectory;
    // The local history database file (absolute); "" disables local history.
    std::string historyPath;
    // Per-project sessions and the last project (absolute; nav::SessionStore); "" keeps none.
    std::string sessionDirectory;
    // settings.openApplicationFile's target (absolute); "" disables the action.
    std::string applicationSettingsFile;
    // Watches the project folder for external changes (the real file watcher; off in most tests).
    bool watchFiles = false;
    // Where FrostIDE-written compile_commands.json files go (absolute; one directory per solution);
    // "" uses only existing databases.
    std::string compileCommandsCache;
    // The build service's persisted active configurations (absolute); "" persists nothing.
    std::string buildStatePath;
    // The CMake service's selected profiles (absolute); "" persists nothing.
    std::string cmakeStatePath;
    // The run service's persisted startup projects (absolute); "" persists nothing.
    std::string runStatePath;
    // Where LuaLS writes its log and meta files (absolute; --logpath/--metapath, ADR 0079); "" uses
    // <FrostIDE data directory>/luals in the user profile.
    std::string luaStateDirectory;
    // Test seam: the LuaLS search instead of lang::defaultLuaSearch's bundled, SDK and PATH
    // locations (the tools.lua.languageServer.path setting still applies on top).
    std::optional<lang::LuaSearch> luaSearch;
    // Test seam: the FrostIDE executable that provides the isolated discovery helper.
    // Empty uses this application's own executable.
    std::string engineDiscoveryHelperPath;
    // Console programs run in an external console window unless the solution chose the Run console.
    bool runExternalConsole = true;
    // MSBuild.exe (absolute); "" uses tools.msbuild.path, else vswhere at the first build.
    std::string msbuildPath;
    debug::AdapterFactory debugAdapterFactory{}; // Optional adapter override for integration tests.
    std::string managedPdbGuardPath;             // Test override; normally resolved beside the application.
    std::function<std::optional<ManagedDebugSelection>(std::string_view)> selectManagedDebug; // Test seam.
    std::function<QString(const QString&)> selectExistingProjectSource;                       // Test seam.
    // Replaces MSBuild (tests); empty runs MSBuild.
    build::BuildStarter startBuild;
    // Starts clangd for an opened project when it is found (tools.clangd.path, then the SDK, PATH).
    bool clangd = true;
    // Test seam: disable Git discovery when a fixture must not start child processes.
    bool git = true;
    // clangd's parallel jobs (0: its default).
    unsigned clangdJobs = 0;
    // Optional terminal test seam; when absent the workbench owns a real PTY launcher.
    term::IPtyLauncher* terminalLauncher = nullptr;
    std::optional<term::ShellProbe> terminalProbe;
    // The keyboard layout of the keymap; nullopt reads the active one (en-US when it cannot).
    std::optional<input::LayoutTable> layout;
    // Re-read the OS keyboard layout when it changes (off in tests).
    bool followActiveLayout = true;
    // Syntax highlighting, outline, language and scan workers (0: two).
    unsigned workers = 0;
};

class Workbench final : public QObject, private IDocumentEditorListener, private ui::ISaveAllHandler {
    Q_OBJECT

public:
    Workbench(WorkbenchServices services, WorkbenchOptions options = {}, QObject* parent = nullptr);
    ~Workbench() override;
    Workbench(const Workbench&) = delete ("the window refers to the workbench's services");
    Workbench& operator=(const Workbench&) = delete ("the window refers to the workbench's services");

    // Installs the appearance from `resourceDir` (absolute, UTF-8) on `app` (ADR 0015). Errors:
    // those of AppearanceManager::install(); the application keeps Qt's style then.
    [[nodiscard]] Result<ui::InstallReport> installAppearance(QApplication& app, std::string resourceDir);
    // The environment to build the window from (the workbench's registry, keymap, settings, editors).
    [[nodiscard]] ui::ShellEnvironment environment();
    // Wires the window built from environment(): the navigation workbench, tool windows, actions,
    // toolbar, Welcome screen and the language controller. Call once; the window's teardown()
    // detaches them again.
    void attach(ui::ShellWindow& window);

    /*
     * Reopens the last project (the session store's, mapped back to the solution or folder the user
     * opened) with its tabs, when general.reopenLastProject is on and it still exists. Returns
     * whether it did; otherwise the Welcome screen stays. The start-up calls it when no project was
     * named on the command line.
     */
    bool restoreLastProject();
    /*
     * Scans the journal directory for the unsaved files of earlier sessions (exited or crashed
     * processes) and offers them in the navigation workbench's restore notification (Restore,
     * Discard). Call once, after the window is shown (the scan waits up to 500 ms for each journal
     * a live instance holds; budget B1). Returns how many files were offered; later calls return 0.
     */
    std::size_t offerRecovery();
    // Saves the session, autosaves (save.autosave.onFrameDeactivation), closes the journal and stops
    // clangd and every run. Idempotent.
    void shutdown();

    // --- Projects and files ---

    // Opens a solution, folder or file (see the header comment). Errors: the loader's, shown as
    // notifications too; Cancelled when the open project's tabs could not all be closed.
    Result<void> openPath(std::string_view path, FileLocation at = {});
    // Opens the folder containing an existing CMakeLists.txt as a CMake project, without choosing
    // a sibling solution. Cancelled if the current project's documents cannot be closed.
    [[nodiscard]] Result<void> openCMakeProject(std::string_view path);
    // Opens a FrostEngine project file or root without auto-selecting a sibling solution.
    // A directory with multiple .frostproj files is ambiguous and is refused.
    [[nodiscard]] Result<void> openExternalProject(std::string_view path);
    /*
     * Closes the project: saves the session, saves and closes every tab, closes the solution, stops
     * clangd and forgets it as the last project (the next start shows the Welcome screen). Returns
     * false, keeping the project, when a tab refused to close (its save failed and the user
     * cancelled).
     */
    bool closeProject();
    // Shows the File > Open (or Open Folder) prompt and opens the choice.
    void showOpenDialog(IWorkbenchPrompts::OpenKind kind);
    // Shows the File > New Project prompt and creates + opens the project.
    void showNewProjectDialog();
    // Creates a new project from `spec` without overwriting existing non-empty folders, and opens it.
    [[nodiscard]] Result<std::string> createNewProject(const NewProjectSpec& spec);
    // Creates a new file or folder in `directory` (empty: active tree/editor/project directory).
    [[nodiscard]] Result<std::string> createNewFile(std::string_view directory, std::string_view relativeName);
    [[nodiscard]] Result<std::string> createNewFolder(std::string_view directory, std::string_view relativeName);
    // Renames a file or folder on disk and updates open tabs, project files (CMakeLists.txt/.vcxproj/.csproj), and LSP.
    [[nodiscard]] Result<std::string> renameItem(std::string_view oldPath, std::string_view newRelativeName);
    // Deletes a file or folder on disk, closes open tabs under it, and updates project files and LSP.
    [[nodiscard]] Result<void> deleteItem(std::string_view path);
    // Prompts for a new name and renames `path` (empty: active tree selection or editor path).
    void promptAndRenameItem(std::string_view path = {});
    // Prompts for confirmation and deletes `path` (empty: active tree selection or editor path).
    void promptAndDeleteItem(std::string_view path = {});
    // Creates a new C/C++ source file inside `projectPath`'s directory, opens it, and asks the
    // ProjectHost helper to add it to the .vcxproj (asynchronous; a refusal is a notification).
    [[nodiscard]] Result<std::string> createNewProjectSource(std::string_view projectPath,
                                                             std::string_view relativeName);
    // Adds an existing project file to a simple top-level CMake target in CMakeLists.txt.
    [[nodiscard]] Result<void> addSourceToCMakeLists(std::string_view sourcePath, std::string_view targetName = {});
    /*
     * Get from Version Control: clones `url` (a URL or a local path) into `directory` (absolute)
     * with the git that tools.git.path, Git for Windows' install folder or PATH resolves to
     * (vcs::findGit; started through IProcessLauncher, never by bare name), then opens it. Returns
     * once the clone has started: git runs on a worker thread, its --progress lines update a
     * progress dialog with Cancel, and the GUI thread never waits for it (FND-9a). The outcome
     * arrives as a notification with git's own last output lines, or as the opened project.
     * Userinfo in the URL (https://user:token@host/...) is shown as "***@" in every label and
     * notification, including git's quoted lines; git itself gets the URL unchanged.
     * Errors: Busy (a clone is running), InvalidArgument (empty or option-like `url`, relative
     * `directory`), Cancelled (shutting down). GUI thread only.
     */
    [[nodiscard]] Result<void> cloneRepository(const std::string& url, const std::string& directory);
    // Whether a clone started by cloneRepository() is still running.
    [[nodiscard]] bool cloneRunning() const noexcept;
    // Ends the running clone's git process tree (Cancel); the dialog closes when it has stopped.
    void cancelClone();
    [[nodiscard]] bool hasProject() const noexcept { return !m_projectRoot.empty(); }
    // Canonical directory of the current project, empty when none is open.
    [[nodiscard]] const std::string& projectRoot() const noexcept { return m_projectRoot; }
    // Accept an authenticated broker announcement only for this trusted project.
    [[nodiscard]] Result<void> announceUnity(const lang::Json& message);
    // The open solution, or nullptr (no project, or a folder project).
    [[nodiscard]] std::shared_ptr<const project::SolutionModel> solution() const { return m_solution; }

    // --- Saving and external changes ---

    // Saves `view`'s document. Errors: SaveManager::save's (also shown as a notification).
    Result<void> save(DocumentEditorView& view);
    // Saves `view`'s document to `targetPath` and updates its tab, registry and LSP identity.
    [[nodiscard]] Result<void> saveAs(DocumentEditorView& view, std::string_view targetPath);
    // Prompts for a destination path and runs saveAs() on the active editor view.
    void showSaveAsDialog();
    // Save All (the navigation workbench's, file.saveAll); failures are shown as notifications.
    document::SaveAllResult saveAllDocuments();
    // The IDE frame lost focus to another application (autosave) or got it back (external changes).
    void frameDeactivated();
    void frameActivated();
    // Reload All from Disk: checkDisk() for every open document, reacting as the header comment says.
    void checkExternalChanges();

    // --- Services, for the window and tests ---

    [[nodiscard]] input::ActionRegistry& registry() noexcept { return m_registry; }
    [[nodiscard]] input::KeymapService& keymap() noexcept { return *m_keymap; }
    [[nodiscard]] document::SaveManager& documents() noexcept { return *m_documents; }
    [[nodiscard]] ui::nav::DocumentRegistry& documentRegistry() noexcept { return *m_documentRegistry; }
    // The real editors (one view per tab, inside the registry's banner view).
    [[nodiscard]] DocumentEditorFactory& editors() noexcept { return *m_editors; }
    [[nodiscard]] ui::BuildController& build() noexcept { return *m_build; }
    [[nodiscard]] build::BuildService& buildService() noexcept { return *m_buildService; }
    [[nodiscard]] build::CMakeService* cmakeService() noexcept;
    // Whether the CMake target view has received the service's current immutable model.
    [[nodiscard]] bool cmakeTargetsCurrent() const;
    [[nodiscard]] build::RunService* runService() noexcept { return m_runService.get(); }
    [[nodiscard]] ui::DebugController* debugController() noexcept { return m_debug.get(); }
    [[nodiscard]] ui::AppearanceManager* appearance() noexcept { return m_appearance.get(); }
    [[nodiscard]] lang::LuaService* lua() noexcept { return m_lua.get(); }
    [[nodiscard]] lang::ManagedService* managed() noexcept { return m_managed.get(); }
    [[nodiscard]] lang::ClangdService* clangd() noexcept { return m_clangd.get(); }
    // Current generation's completed MSBuild contexts, copied safely from the evaluator thread.
    [[nodiscard]] std::vector<std::shared_ptr<const lang::StreamedProjectContext>> streamedProjectContexts() const {
        const auto trust = m_services.trust.snapshot(m_projectRoot);
        if (!trust.trusted || trust.revision != m_projectTrustRevision)
            return {};
        return m_streamedProjects ? m_streamedProjects->snapshot()
                                  : std::vector<std::shared_ptr<const lang::StreamedProjectContext>>{};
    }
    [[nodiscard]] ui::nav::SessionStore* sessions() noexcept { return m_sessions.get(); }
    // The window's parts (null before attach()).
    [[nodiscard]] ui::Workbench* navigation() const noexcept { return m_nav.get(); }
    // Called after startup project/session/editor restoration has completed.
    void markInteractiveStartupComplete();
    // Live UI, navigation and current-project scan readiness; GUI thread only.
    [[nodiscard]] bool interactiveReady() const;
    [[nodiscard]] ui::lang::LanguageController* language() const noexcept { return m_language.get(); }
    [[nodiscard]] ui::RunController* runController() const noexcept { return m_runController.get(); }
    [[nodiscard]] ui::RunConsole* runConsole() const noexcept { return m_runConsole.data(); }
    [[nodiscard]] ui::SolutionExplorer* solutionExplorer() const noexcept { return m_explorer.data(); }
    [[nodiscard]] ui::ProblemsView* problems() const noexcept { return m_problems.data(); }
    [[nodiscard]] ui::BuildOutputPane* buildOutput() const noexcept { return m_output.data(); }
    [[nodiscard]] ui::ConfigurationCombo* configurationCombo() const noexcept { return m_configurations.data(); }
    [[nodiscard]] WelcomeScreen* welcomeScreen() const noexcept { return m_welcome.data(); }
    [[nodiscard]] LauncherWindow* launcherWindow() const noexcept { return m_launcherWindow.data(); }
    void showLauncherWindow();
    // Startup without project or file arguments: reopens the last project in the IDE window when
    // general.reopenLastProject is on and there is one (true), else shows the launcher (false).
    bool showStartupWindow();
    [[nodiscard]] ui::ShellWindow* window() const noexcept { return m_window.data(); }
    // The view of the active tab, or nullptr.
    [[nodiscard]] DocumentEditorView* currentView() const;

signals:
    // A project opened (`name` non-empty) or closed (empty).
    void projectChanged(const QString& name);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    friend class LanguageBridge;
    friend class EngineIntegration;
    [[nodiscard]] bool canStartLuaDebug() const;
    friend class UnityControlIntegration;
    friend class GitIntegration;

    // IDocumentEditorListener.
    void editorOpened(DocumentEditorView& view) override;
    void editorClosing(DocumentEditorView& view) override;
    void editorClosed(DocumentEditorView& view) override;
    void editorTextChanged(DocumentEditorView& view, const text::ChangeSet& change) override;
    [[nodiscard]] bool editorCloseRequested(DocumentEditorView& view) override;
    // ISaveAllHandler (the build's Save All).
    void saveAll(Completion done) override;

    // Construction steps.
    void registerActions();
    void createBuildService();
    void createRunService();
    void attachToolWindows();
    void attachNavigationBar();
    void attachToolbar();
    void attachRun();
    void attachLanguage();
    void attachTerminal();
    void attachFind();
    void cancelFind();
    void stopTerminal();
    void detach();

    // Projects.
    Result<void> openSolution(const std::string& path);
    void removeProjectFile(const QString& project, const QString& source, bool adding = false);
    Result<void> openFolder(const std::string& path, bool discoverSolution = true, std::string recentPath = {});
    void setProject(std::string root, QString name, std::string recentPath);
    // Closes the open project before another opens; false when a tab refused (nothing changed).
    [[nodiscard]] bool closeForOpen();
    void refreshRecent();
    void showOpenRecent();
    void showSettings();
    void showUpdateCheck();
    void showVersionSwitcher();
    void applyVerifiedUpdate();
    void showAttachToProcess();
    void attachToProcess(std::uint32_t pid, const std::string& exePath, const std::string& exeName,
                         bool attachAsDotNet);
    void launchCompoundConfiguration(const std::vector<project::ProjectId>& targets);
    [[nodiscard]] Result<void> reopenRoot(const std::string& root);
    [[nodiscard]] Result<void> openProjectIdentity(const std::string& path);

    // LuaLS, only in trusted workspaces.
    void startManaged(const std::string& root);
    void checkUnityProjectContext();
    void stopManaged();
    void syncManagedDocument(DocumentEditorView& view);
    void onManagedDiagnostics(const std::string& file);
    void offerManagedTrust();
    void startLua(const std::string& root);
    void stopLua();
    void syncLuaDocument(DocumentEditorView& view);
    void onLuaDiagnostics(const std::string& file);
    void updateLanguageTarget();
    void offerProjectTrust();
    void offerLuaTrust();
    void startManagedDebug();
    void invalidateManagedDebug();
    [[nodiscard]] bool managedDebugFrameAllowed(const debug::DebugFrame& frame) const;
    void validateManagedBreakpoint(std::string path, std::function<void(bool)> done);
    void showManagedDebugSource(const std::string& path, int line);

    // clangd.
    void startClangd(const std::string& root);
    void startProjectEvaluation(const std::string& root);
    void stopProjectEvaluation();
    void stopClangd();
    void restartClangd();
    // A CMake model change, or a trust change, restarts clangd only when the compile-commands
    // location or the trust its launch flags came from changed.
    void restartClangdIfCMakeStateChanged();
    // Starts the clangd that openFolder deferred until its configure finished (FND-9a). With
    // `retryUntilIdle`, a CMake service that is not idle yet is asked again shortly.
    void resumeDeferredClangd(bool retryUntilIdle = false);
    void notifyLuaUnavailable();
    void onLiveDiagnostics(const std::string& file);
    void onSettingsChanged();
    void onClangdProgress(const lang::ClangdProgress& progress);
    void syncClangdDocument(DocumentEditorView& view);

    // Diagnostics and decorations in the editors.
    void refreshMarkers(DocumentEditorView& view);
    void refreshAllMarkers();
    void refreshLiveProblems();

    // Documents.
    // The registry reported a state change of `path` (a save, a reload, a merge): the view follows.
    void documentChanged(const std::string& path);
    void onNewFileCreated(const std::string& path);
    [[nodiscard]] std::string defaultNewItemDirectory() const;
    [[nodiscard]] std::string defaultTargetPathForModify() const;
    void rebindRenamedDocument(const std::string& oldPath, const std::string& newPath);
    void closeDeletedDocuments(const std::string& deletedPath, bool directory);
    void refreshSolutionExplorerForFsChange(std::string_view selectPath = {});
    void renameSourceInCMakeLists(const std::string& oldPath, const std::string& newPath, bool directory);
    void removeSourceFromCMakeLists(const std::string& deletedPath, bool directory);
    void updatePathInVcxproj(const std::string& oldPath, const std::string& newPath, bool directory, bool deleted);
    void queryWorkspaceSymbols(std::string_view query, bool classesOnly,
                               std::function<void(std::vector<ui::WorkbenchSymbol>)> done);
    void openLocation(const QString& path, int line, int column);
    void notify(const QString& title, const QString& text, ui::NotificationKind kind);
    // Saves without notifying (the close path shows the error in its prompt).
    [[nodiscard]] Result<void> saveQuietly(document::FileDocument& document);
    // The pre-save transform for `document`: editor.trimTrailingWhitespace and
    // editor.insertFinalNewline resolved for its file (settings plus .editorconfig), keeping the
    // line each of its views' primary caret is on.
    [[nodiscard]] document::SaveTransform saveTransformFor(const document::FileDocument& document) const;

    WorkbenchServices m_services;
    WorkbenchOptions m_options;

    // Declaration order is destruction order reversed: the scheduler and the registry the views,
    // sessions and clangd use outlive them.
    syntax::LanguageRegistry m_languages;
    std::unique_ptr<sched::ThreadPool> m_pool;
    std::shared_ptr<lang::GeneratedCommandsRetirement> m_commandsRetirement;
    std::unique_ptr<io::Reactor> m_reactor;
    std::unique_ptr<io::RealFileWatcher> m_watcher;

    StoreShellSettings m_shellSettings;
    StoreKeymapSettings m_keymapSettings;
    StoreAppearanceSettings m_appearanceSettings;
    input::ActionRegistry m_registry;
    std::optional<input::KeymapService> m_keymap;
    std::unique_ptr<ui::QtOsAppearanceSource> m_os;
    std::unique_ptr<ui::AppearanceManager> m_appearance;

    bool m_recoveryOffered = false;
    std::optional<storage::HotExitJournal> m_journal;
    std::optional<storage::LocalHistory> m_history;
    std::unique_ptr<document::SaveManager> m_documents;
    std::unique_ptr<document::AutosaveController> m_autosave;
    std::unique_ptr<ui::nav::SessionStore> m_sessions;
    std::unique_ptr<ui::nav::DocumentRegistry> m_documentRegistry;
    std::uint64_t m_registryListener = 0;

    ui::EditorActionHost m_actionHost;
    std::unique_ptr<DocumentEditorFactory> m_editors;
    std::unique_ptr<ui::DocumentEditorFactory> m_tabs; // The registry's banner views around m_editors.

    std::unique_ptr<ui::BuildController> m_build;
    std::unique_ptr<build::BuildService> m_buildService;
    std::unique_ptr<EngineIntegration> m_engine;
    std::unique_ptr<UnityControlIntegration> m_unity;
    std::unique_ptr<GitIntegration> m_git;
    std::unique_ptr<ui::BuildServiceBackend> m_buildBackend;
    std::unique_ptr<CMakeIntegration> m_cmake;
    ui::BuildActions* m_buildActions = nullptr; // Child of this.
    std::unique_ptr<build::IRunBuilder> m_runBuilder;
    std::unique_ptr<ui::RunController> m_runController;
    std::unique_ptr<build::RunService> m_runService;
    debug::BreakpointManager m_breakpoints;
    std::unique_ptr<ui::DebugController> m_debug;
    std::unique_ptr<ManagedDebugPreparation> m_managedDebugPreparation;
    std::shared_ptr<ManagedSourceSnapshot> m_managedDebugSnapshot;
    QPointer<QDialog> m_managedSourceDialog;
    QPointer<QDialog> m_settingsDialog;
    QPointer<QPlainTextEdit> m_managedSourceView;
    std::shared_ptr<ManagedSnapshotJob> m_managedSnapshotJob;
    std::vector<std::shared_ptr<ManagedSnapshotJob>> m_managedBreakpointJobs;
    std::vector<sched::JoinHandle<void>> m_managedSnapshotTasks;
    std::string m_managedDebugRoot;

    std::shared_ptr<lang::ICompileCommandsProvider> m_compileCommands;
    std::shared_ptr<lang::StreamedProjectContexts> m_streamedProjects;
    std::shared_ptr<lang::IndependentProjectEvaluation> m_projectEvaluation;
    bool m_closingSolution = false;
    std::vector<sched::JoinHandle<void>> m_retiredProjectEvaluations;
    std::shared_ptr<lang::ManagedService> m_managed;
    std::uint64_t m_managedListener = 0;
    std::vector<sched::JoinHandle<void>> m_retiredManaged;
    QPointer<ui::TrustDialog> m_projectTrustDialog;
    std::string m_projectTrustPromptedRoot;
    QPointer<ui::TrustDialog> m_managedTrustDialog;
    std::string m_unityRevisionRoot;
    std::string m_unityRevision;
    // "<root>\n<revision>" of the Unity generation whose unmodelled compiler options were last
    // notified; a restart for the same generation (settings, trust) does not notify again.
    std::string m_unityNotifiedGeneration;
    std::string m_managedDotnetPath;
    std::string m_managedServerPath;
    std::shared_ptr<lang::LuaService> m_lua;
    std::uint64_t m_luaListener = 0;
    std::vector<sched::JoinHandle<void>> m_retiredLua;
    QPointer<ui::TrustDialog> m_luaTrustDialog;
    QTimer m_luaTrustWatch;
    bool m_luaObservedTrust = false;
    std::string m_luaConfiguredPath;
    std::string m_luaRuntimeVersion;
    // Why LuaLS could not start for the open project ("" when it runs or was not needed), and the
    // root that was already told so: the notification appears once per project (LUA-1).
    std::string m_luaUnavailable;
    std::string m_luaUnavailableNotifiedRoot;
    std::shared_ptr<lang::ClangdService> m_clangd;
    // The root already told that clangd was not found (the notice appears once per project; ADR 0078).
    std::string m_clangdUnavailableNotifiedRoot;
    std::vector<sched::JoinHandle<void>> m_retiredClangd;
    std::uint64_t m_clangdGeneration = 0;
    // FND-9a, one clangd per project open: the CMake compile-commands state the running clangd was
    // started with (cmakeClangdState), and the root whose start waits for the open's configure.
    std::string m_clangdCMakeState;
    // Whether the running clangd was started trusted (full mode) or restricted (FND-3); a trust
    // grant on a CMake folder must restart it even when the compile commands did not move.
    bool m_clangdTrusted = false;
    std::string m_clangdDeferredRoot;
    QMetaObject::Connection m_clangdDeferral;
    std::uint64_t m_projectTrustRevision = 0;
    std::uint64_t m_clangdListener = 0;
    std::shared_ptr<ui::BackgroundTask> m_indexing;
    std::string m_indexToken;
    sched::LatestWins m_searchSymbolsSlot;
    std::unique_ptr<LanguageBridge> m_bridge;
    std::unique_ptr<ui::lang::LanguageController> m_language;
    // Text the user typed into the current editor during this key press (for textTyped).
    std::string m_typed;

    settings::Subscription m_settingsSubscription;

    std::unique_ptr<term::RealPtyLauncher> m_terminalLauncher;
    std::unique_ptr<ui::TerminalIntegration> m_terminal;
    std::unique_ptr<ui::FindFeature> m_find;
    QPointer<ui::ShellWindow> m_window;
    std::unique_ptr<ui::Workbench> m_nav;
    bool m_interactiveStartupComplete = false;
    bool m_updateChecking = false;
    std::unique_ptr<QTemporaryFile> m_verifiedUpdate;
    std::optional<update::AuthenticatedRelease> m_verifiedRelease;
    QPointer<ui::SolutionExplorer> m_explorer;
    QPointer<ui::ProblemsView> m_problems;
    QPointer<ui::BuildOutputPane> m_output;
    QPointer<ui::RunConsole> m_runConsole;
    QPointer<ui::ConfigurationCombo> m_configurations;
    QPointer<WelcomeScreen> m_welcome;
    QPointer<LauncherWindow> m_launcherWindow;
    bool m_launcherModeEnabled = false;
    project::CppHierarchyMode m_cppHierarchyMode = project::CppHierarchyMode::FiltersAndFolders;
    std::vector<std::string> m_unloadedProjectPaths;
    std::unique_ptr<GitClone> m_clone; // The running cloneRepository(), if any.
    bool m_exitConfirmed = false;      // The IDE closes its own window (restart): no general.confirmExit prompt.
    void cloneProgress(const std::string& line);
    void finishClone(bool ok, const std::string& message);
    std::vector<std::string> m_clipboardPaths;
    bool m_clipboardIsCut = false;
    void transitionLauncherToMainWindow();
    void wireWelcomeScreen(WelcomeScreen* welcome);
    void addPathsToActiveProject(const std::vector<std::string>& paths, std::string_view targetFilterPath = {});
    void copyPathsToClipboard(const std::vector<std::string>& paths);
    void cutPathsToClipboard(const std::vector<std::string>& paths);
    [[nodiscard]] bool canPastePaths() const noexcept { return !m_clipboardPaths.empty(); }
    void pasteClipboardIntoDirectory(const std::string& targetDirectory, const std::string& targetProjectPath = {},
                                     const std::string& targetFilterPath = {});
    void duplicatePathInExplorer(const std::string& sourcePath, bool isDirectory, const std::string& projectPath = {},
                                 const std::string& filterPath = {});
    void movePhysicalPathsToDirectory(const std::vector<std::string>& sourcePaths, const std::string& targetDirectory,
                                      const std::string& projectPath = {});
    [[nodiscard]] QWidget* activeTopWindow() const noexcept;
    ui::TrustPrompter* m_trustPrompter = nullptr; // Child of this.

    std::shared_ptr<const project::SolutionModel> m_solution;
    std::unique_ptr<ProjectEditProcess> m_projectEdit;
    std::string m_projectRoot;
    // The explicitly opened project file or folder, used as the last-session identity.
    std::string m_projectIdentity;
    // Live gutter markers per document key (merged with the build's in refreshMarkers).
    std::map<std::string, std::vector<ui::LineMarker>> m_liveMarkers;
    // Paths whose close the user confirmed after a failed save: the document closes unsaved.
    std::vector<std::string> m_discarded;
    bool m_suppressNewFileNotice = false;
    bool m_shutDown = false;
};

} // namespace frost::app
