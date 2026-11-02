// SPDX-License-Identifier: Apache-2.0
//
// AppServices: the composition root's services for the main window (ADR 0038, ADR 0042): the action
// registry, the keymap service over the active keyboard layout, the settings seams with their
// in-memory stores until m1/settings binds them to the registry, the document stack (SaveManager,
// DocumentRegistry, the document editor factory over the placeholder editor), the workbench's
// services and, in the GUI, the appearance manager. With persistence on (the program), it adds the
// hot-exit journal, per-project sessions, the background thread pool and the file watcher.
// MainWindow owns one when it is default-constructed; main() creates one before the window.
#pragma once

#include "frost/core/error.hpp"
#include "frost/document/autosave.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/input/action.hpp"
#include "frost/input/keymap_service.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/io/reactor.hpp"
#include "frost/sched/thread_pool.hpp"
#include "frost/storage/hot_exit_journal.hpp"
#include "frost/ui/appearance_manager.hpp"
#include "frost/ui/model/appearance_settings.hpp"
#include "frost/ui/nav/document_registry.hpp"
#include "frost/ui/nav/session.hpp"
#include "frost/ui/qt_os_appearance.hpp"
#include "frost/ui/shell/placeholder_editor.hpp"
#include "frost/ui/shell/shell_settings.hpp"
#include "frost/ui/shell/shell_window.hpp"
#include "frost/ui/workbench/document_editor.hpp"
#include "frost/ui/workbench/workbench.hpp"

#include <memory>
#include <optional>
#include <string>

class QApplication;

namespace frost::app {

struct AppServicesOptions {
    // The program's state on disk: the hot-exit journal and sessions under the data directory, a
    // thread pool and a file watcher. Off: everything in memory (tests, the self-test).
    bool persistent = false;
    // The data directory; empty: the parent of TrustStore::defaultStorePath()
    // (%LOCALAPPDATA%\FrostIDE\data, ~/.local/share/FrostIDE).
    std::string dataDirectory{};
};

/*
 * Members are declared in dependency order, so destruction is safe; the window built on them must
 * be destroyed (or torn down) first. GUI thread only; needs a QGuiApplication (the double-tap
 * interval comes from QStyleHints). Persistence problems (no data directory, a journal that cannot
 * be created) never fail construction: the feature is off and startupWarnings() says why.
 */
class AppServices {
public:
    // Creates the registry and the keymap service: the IntelliJ keymap on the active keyboard
    // layout (en-US when it cannot be read).
    explicit AppServices(AppServicesOptions options = {});
    AppServices(const AppServices&) = delete ("the window holds references into the services");
    AppServices& operator=(const AppServices&) = delete ("the window holds references into the services");
    // Closes the hot-exit journal cleanly (journaling the unsaved buffers first), then stops the
    // reactor, the file watcher and the thread pool in the order frost/io/reactor.hpp gives.
    ~AppServices();

    // Installs the appearance from `resourceDir` (absolute, UTF-8) on `app` (ADR 0015). Errors:
    // those of AppearanceManager::install(); the application keeps Qt's style then.
    [[nodiscard]] Result<ui::InstallReport> installAppearance(QApplication& app, std::string resourceDir);

    // The environment for a ShellWindow; `followActiveLayout` re-reads the OS layout on changes.
    [[nodiscard]] ui::ShellEnvironment environment(bool followActiveLayout = true);
    // The services for the window's Workbench (ADR 0042).
    [[nodiscard]] ui::WorkbenchServices workbenchServices();
    /*
     * The journals an earlier run left (hot exit or a crash), for the restore prompt. Call it after
     * the first frame (it waits up to 500 ms per journal another live instance holds). Errors:
     * listing the journal directory; NotFound without persistence.
     */
    [[nodiscard]] Result<storage::RecoveryScan> scanRecoverableJournals();
    // Why a persistent feature is off (one line each).
    [[nodiscard]] const std::vector<std::string>& startupWarnings() const noexcept { return m_warnings; }

    [[nodiscard]] input::ActionRegistry& registry() noexcept { return m_registry; }
    [[nodiscard]] input::KeymapService& keymap() noexcept { return *m_keymap; }
    [[nodiscard]] ui::AppearanceManager* appearance() noexcept { return m_appearance.get(); }
    [[nodiscard]] document::SaveManager& saveManager() noexcept { return *m_saveManager; }
    [[nodiscard]] ui::nav::DocumentRegistry& documents() noexcept { return *m_documents; }

private:
    void startPersistence(const AppServicesOptions& options);

    std::vector<std::string> m_warnings;
    input::ActionRegistry m_registry;
    input::MemoryKeymapSettings m_keymapSettings;
    std::optional<input::KeymapService> m_keymap;
    ui::InMemoryShellSettings m_shellSettings;
    ui::InMemoryAppearanceSettings m_appearanceSettings;
    std::unique_ptr<ui::QtOsAppearanceSource> m_os;
    std::unique_ptr<ui::AppearanceManager> m_appearance;
    // Persistence (null when off): the journal outlives the manager, the pool and reactor the watcher.
    std::string m_journalDirectory;
    std::optional<storage::HotExitJournal> m_journal;
    std::unique_ptr<ui::nav::SessionStore> m_sessions;
    std::unique_ptr<sched::ThreadPool> m_pool;
    std::unique_ptr<io::Reactor> m_reactor;
    std::unique_ptr<io::RealFileWatcher> m_watcher;
    std::string m_applicationSettingsFile;
    // The document stack.
    std::unique_ptr<document::SaveManager> m_saveManager;
    std::unique_ptr<ui::nav::DocumentRegistry> m_documents;
    ui::PlaceholderEditorFactory m_editors;
    std::unique_ptr<ui::DocumentEditorFactory> m_documentEditors;
    std::unique_ptr<document::AutosaveController> m_autosave;
};

} // namespace frost::app
