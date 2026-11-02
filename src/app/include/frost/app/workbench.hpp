// SPDX-License-Identifier: Apache-2.0
//
// Workbench: FrostIDE's composition root for one IDE window (W1, ADR 0044). It wires the shell
// (frost::ui_shell), the editor (frost::ui_editor), the build tool windows (frost::ui_toolwindows),
// the save manager (frost::document), the build service (frost::build), clangd (frost::lang) and the
// settings store (frost::settings) into the window the user works in.
/*
 * Lifecycle. Construct the workbench first (it owns the registry, the keymap, the settings
 * adapters, the save manager and the editor factory the window is built on), install the
 * appearance, build the window from environment(), then attach() it. offerRecovery() after the
 * window is shown asks about unsaved files an earlier session left. shutdown() (also run by the
 * destructor) autosaves, writes and closes the hot-exit journal and stops clangd; the window must
 * be destroyed (or torn down) before the workbench, because its tabs hold views of the workbench's
 * documents.
 *
 * Opening (openPath). A .sln or .slnx opens as the project: the solution model fills the Solution
 * Explorer, the build service opens it (the configuration combo sits in the main toolbar) and
 * clangd starts on it (compile commands: an existing compile_commands.json under the solution
 * directory, else the ones synthesized from the projects). A folder opens the one solution in it,
 * else the folder itself as the project (clangd on its compile_commands.json). Any other file opens
 * in an editor tab through the one open-file path (OpenFileService). Opening a project closes the
 * previous one; the window title is "<project> - FrostIDE".
 *
 * Saving (ADR 0044). Ctrl+S is IntelliJ's Save All (file.saveAll): every modified document, through
 * the save manager; file.saveDocument saves the current one. Closing a dirty tab saves it first and
 * asks only when that save fails. Autosave runs on frame deactivation and after the idle delay
 * (save.autosave.*), the hot-exit journal shortly after input stops and at exit. Every build saves
 * all first (the build tool windows' Save-All hook).
 *
 * External changes are checked when the window becomes active and by Reload All from Disk: a
 * clean buffer reloads silently, a dirty one gets a notification offering "Keep my version" and
 * "Take disk version".
 *
 * Auto-show. The Build tool window (bottom) shows when a build starts; Problems shows after a build
 * that reported errors. Neither takes focus.
 *
 * Diagnostics. Build diagnostics become gutter markers of the open editors (refreshed on every
 * build and when a document opens); clangd's live diagnostics go to the Problems window's Live rows
 * and to the editor as squiggles (range decorations) and gutter markers.
 *
 * Threads: GUI thread only. clangd's and the settings store's callbacks are marshalled onto it.
 * TODO(M1): the save manager's disk work (saves, journal writes) still runs on the GUI thread.
 */
#pragma once

#include "frost/app/document_editor.hpp"
#include "frost/app/open_file.hpp"
#include "frost/app/settings_adapters.hpp"
#include "frost/app/welcome_screen.hpp"
#include "frost/build/build_service.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/document/autosave.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/input/action.hpp"
#include "frost/input/keymap_service.hpp"
#include "frost/input/layout.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/clangd.hpp"
#include "frost/lang/diagnostics.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/settings/store.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/storage/local_history.hpp"
#include "frost/syntax/language.hpp"
#include "frost/ui/appearance_manager.hpp"
#include "frost/ui/editor/editor_actions.hpp"
#include "frost/ui/qt_os_appearance.hpp"
#include "frost/ui/shell/background_tasks.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/toolwindows/build_actions.hpp"
#include "frost/ui/toolwindows/build_controller.hpp"
#include "frost/ui/toolwindows/build_output.hpp"
#include "frost/ui/toolwindows/problems.hpp"
#include "frost/ui/toolwindows/save_all_handler.hpp"
#include "frost/ui/toolwindows/solution_explorer.hpp"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QApplication;
class QWidget;

namespace frost::ui {
class TrustPrompter;
} // namespace frost::ui

namespace frost::app {

// The workbench's own action ids (the shell, editor and build slices own the others).
namespace workbench_actions {
inline constexpr std::string_view kSaveAll = "file.saveAll";
inline constexpr std::string_view kSaveDocument = "file.saveDocument";
inline constexpr std::string_view kReloadFromDisk = "file.reloadFromDisk";
inline constexpr std::string_view kOpenFolder = "file.openFolder";
inline constexpr std::string_view kOpenRecent = "file.openRecent";
inline constexpr std::string_view kCloseProject = "file.closeProject";
} // namespace workbench_actions

// The id of the Build output tool window (bottom).
inline constexpr std::string_view kBuildToolWindow = "build";

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
    // A dirty tab is closing and saving it failed with `error`: true closes it, losing the changes.
    [[nodiscard]] virtual bool confirmDiscard(QWidget* parent, const QString& file, const QString& error) = 0;
    // `count` unsaved files of an earlier session were found: true restores them, false discards them.
    [[nodiscard]] virtual bool confirmRestore(QWidget* parent, int count) = 0;

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
    [[nodiscard]] bool confirmDiscard(QWidget* parent, const QString& file, const QString& error) override;
    [[nodiscard]] bool confirmRestore(QWidget* parent, int count) override;
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

struct WorkbenchOptions {
    // This process's hot-exit journal and the recovery scan (absolute); "" disables hot exit.
    std::string journalDirectory;
    // The local history database file (absolute); "" disables local history.
    std::string historyPath;
    // Where synthesized compile_commands.json files go (absolute; one directory per solution); "" uses
    // only existing databases.
    std::string compileCommandsCache;
    // The build service's persisted active configurations (absolute); "" persists nothing.
    std::string buildStatePath;
    // MSBuild.exe (absolute); "" uses tools.msbuild.path, else vswhere at the first build.
    std::string msbuildPath;
    // Replaces MSBuild (tests); empty runs MSBuild.
    build::BuildStarter startBuild;
    // Starts clangd for an opened project when it is found (tools.clangd.path, then the SDK, PATH).
    bool clangd = true;
    // clangd's parallel jobs (0: its default).
    unsigned clangdJobs = 0;
    // The keyboard layout of the keymap; nullopt reads the active one (en-US when it cannot).
    std::optional<input::LayoutTable> layout;
    // Re-read the OS keyboard layout when it changes (off in tests).
    bool followActiveLayout = true;
    // Syntax highlighting and outline workers (0: two).
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
    // Wires the window built from environment(): tool windows, actions, toolbar, Welcome screen,
    // autosave and the external-change checks. Call once.
    void attach(ui::ShellWindow& window);

    // Scans the journal directory for the unsaved files of earlier sessions (exited or crashed
    // processes), asks about them (if any) and restores them into tabs or discards them. Call once,
    // after the window is shown (the scan waits up to 500 ms for each journal a live instance holds;
    // budget B1). Returns how many documents were opened in tabs; later calls return 0.
    int offerRecovery();
    // Autosaves (save.autosave.onFrameDeactivation), closes the journal and stops clangd. Idempotent.
    void shutdown();

    // --- Projects and files ---

    // Opens a solution, folder or file (see the header comment). Errors: the loader's, shown as
    // notifications too.
    Result<void> openPath(std::string_view path, FileLocation at = {});
    // Closes the project: saves and closes every tab, closes the solution, stops clangd.
    void closeProject();
    // Shows the File > Open (or Open Folder) prompt and opens the choice.
    void showOpenDialog(IWorkbenchPrompts::OpenKind kind);
    [[nodiscard]] bool hasProject() const noexcept { return !m_projectRoot.empty(); }
    // The open solution, or nullptr (no project, or a folder project).
    [[nodiscard]] std::shared_ptr<const project::SolutionModel> solution() const { return m_solution; }
    [[nodiscard]] OpenFileService& files() noexcept { return *m_files; }

    // --- Saving and external changes ---

    // Saves `view`'s document. Errors: SaveManager::save's (also shown as a notification).
    Result<void> save(DocumentEditorView& view);
    // Save All; failures are shown as notifications.
    document::SaveAllResult saveAllDocuments(document::SaveReason reason = document::SaveReason::SaveAll);
    // The IDE frame lost focus to another application (autosave) or got it back (external changes).
    void frameDeactivated();
    void frameActivated();
    // checkDisk() for every open document, reacting as the header comment says.
    void checkExternalChanges();

    // --- Services, for the window and tests ---

    [[nodiscard]] input::ActionRegistry& registry() noexcept { return m_registry; }
    [[nodiscard]] input::KeymapService& keymap() noexcept { return *m_keymap; }
    [[nodiscard]] document::SaveManager& documents() noexcept { return *m_documents; }
    [[nodiscard]] DocumentEditorFactory& editors() noexcept { return *m_editors; }
    [[nodiscard]] ui::BuildController& build() noexcept { return *m_build; }
    [[nodiscard]] ui::AppearanceManager* appearance() noexcept { return m_appearance.get(); }
    [[nodiscard]] lang::ClangdService* clangd() noexcept { return m_clangd.get(); }
    [[nodiscard]] ui::SolutionExplorer* solutionExplorer() const noexcept { return m_explorer.data(); }
    [[nodiscard]] ui::ProblemsView* problems() const noexcept { return m_problems.data(); }
    [[nodiscard]] ui::BuildOutputPane* buildOutput() const noexcept { return m_output.data(); }
    [[nodiscard]] ui::ConfigurationCombo* configurationCombo() const noexcept { return m_configurations.data(); }
    [[nodiscard]] WelcomeScreen* welcomeScreen() const noexcept { return m_welcome.data(); }
    [[nodiscard]] ui::ShellWindow* window() const noexcept { return m_window.data(); }

signals:
    // A project opened (`name` non-empty) or closed (empty).
    void projectChanged(const QString& name);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // IDocumentEditorListener.
    void editorOpened(DocumentEditorView& view) override;
    void editorClosed(DocumentEditorView& view) override;
    void editorTextChanged(DocumentEditorView& view, const text::ChangeSet& change) override;
    [[nodiscard]] bool editorCloseRequested(DocumentEditorView& view) override;
    // ISaveAllHandler (the build's Save All).
    void saveAll(Completion done) override;

    // Construction steps.
    void registerActions();
    void createBuildService();
    void attachToolWindows();
    void attachToolbar();

    // Projects.
    Result<void> openSolution(const std::string& path);
    Result<void> openFolder(const std::string& path);
    void setProject(std::string root, QString name, std::string recentPath);
    void refreshRecent();
    void showOpenRecent();

    // clangd.
    void startClangd(const std::string& root);
    void stopClangd();
    void onLiveDiagnostics(const std::string& file);
    void onSettingsChanged();
    void onClangdProgress(const lang::ClangdProgress& progress);
    void syncClangdDocument(DocumentEditorView& view);

    // Diagnostics in the editors.
    void refreshMarkers(DocumentEditorView& view);
    void refreshAllMarkers();
    void refreshLiveProblems();

    // Saving.
    void afterSaved(document::FileDocument& document);
    void reportSaveAll(const document::SaveAllResult& result);
    void scheduleAutosave();
    void pollAutosave();
    void handleExternalChange(const document::ExternalChange& change);
    void openLocation(const QString& path, int line, int column);
    void notify(const QString& title, const QString& text, ui::NotificationKind kind);
    // Saves without notifying (the close path shows the error in its prompt).
    [[nodiscard]] Result<void> saveQuietly(document::FileDocument& document);
    [[nodiscard]] DocumentEditorView* currentView() const;

    WorkbenchServices m_services;
    WorkbenchOptions m_options;

    // Declaration order is destruction order reversed: the scheduler and the registry the views,
    // sessions and clangd use outlive them.
    syntax::LanguageRegistry m_languages;
    std::unique_ptr<sched::ThreadPool> m_pool;
    std::unique_ptr<io::Reactor> m_reactor;

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
    QTimer m_autosaveTimer;

    ui::EditorActionHost m_actionHost;
    std::unique_ptr<DocumentEditorFactory> m_editors;

    std::unique_ptr<ui::BuildController> m_build;
    std::unique_ptr<build::BuildService> m_buildService;
    std::unique_ptr<ui::BuildServiceBackend> m_buildBackend;
    ui::BuildActions* m_buildActions = nullptr; // Child of this.

    std::shared_ptr<lang::ICompileCommandsProvider> m_compileCommands;
    std::unique_ptr<lang::ClangdService> m_clangd;
    std::uint64_t m_clangdListener = 0;
    std::shared_ptr<ui::BackgroundTask> m_indexing;
    std::string m_indexToken;

    settings::Subscription m_settingsSubscription;

    QPointer<ui::ShellWindow> m_window;
    std::unique_ptr<OpenFileService> m_files;
    QPointer<ui::SolutionExplorer> m_explorer;
    QPointer<ui::ProblemsView> m_problems;
    QPointer<ui::BuildOutputPane> m_output;
    QPointer<ui::ConfigurationCombo> m_configurations;
    QPointer<WelcomeScreen> m_welcome;
    ui::TrustPrompter* m_trustPrompter = nullptr; // Child of this.

    std::shared_ptr<const project::SolutionModel> m_solution;
    std::string m_projectRoot;
    // Live gutter markers per document key (merged with the build's in refreshMarkers).
    std::map<std::string, std::vector<ui::LineMarker>> m_liveMarkers;
    bool m_shutDown = false;
};

} // namespace frost::app
