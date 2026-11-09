// SPDX-License-Identifier: Apache-2.0
//
// MainWindow (frost/app/main_window.hpp). The owning constructor builds the in-memory stores and
// the workbench before the base class (whose environment comes from the workbench), then attaches
// the workbench; ~MainWindow shuts the workbench down, tears the shell down (the workbench's
// teardown hook detaches it first) and only then destroys the owned workbench.

#include "frost/app/main_window.hpp"

#include "frost/app/workbench.hpp"
#include "frost/core/assert.hpp"
#include "frost/core/clock.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/process.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/settings/registry.hpp"
#include "frost/settings/store.hpp"

#include <utility>

namespace frost::app {

// The default window's composition root: settings in memory, the trust list on a MemFs.
struct MainWindow::Owned {
    Owned() : trustFs(systemClock()) {
        auto store = settings::SettingsStore::open(settings::builtinRegistry(), io::realFileSystem(), {});
        FROST_ASSERT(store.has_value()); // No file to read: open cannot fail.
        settings = std::move(*store);
        auto trusted = project::TrustStore::open(trustFs, io::kNativePathStyle == io::PathStyle::Windows
                                                              ? "C:\\FrostIDE\\trusted-roots.txt"
                                                              : "/FrostIDE/trusted-roots.txt");
        FROST_ASSERT(trusted.has_value()); // A missing file opens as an empty list.
        trust = std::move(*trusted);
        WorkbenchOptions options;
        options.clangd = false;
        options.projectHost = false; // Like clangd: no helper for the default window.
        workbench = std::make_unique<Workbench>(WorkbenchServices{io::realFileSystem(), io::realProcessLauncher(),
                                                                  systemClock(), *settings, *trust, prompts},
                                                std::move(options));
    }

    io::MemFs trustFs;
    std::unique_ptr<settings::SettingsStore> settings;
    std::unique_ptr<project::TrustStore> trust;
    DialogWorkbenchPrompts prompts;
    std::unique_ptr<Workbench> workbench;
};

MainWindow::MainWindow(QWidget* parent) : MainWindow(std::make_unique<Owned>(), parent) {}

MainWindow::MainWindow(ui::ShellEnvironment environment, QWidget* parent) : ui::ShellWindow(environment, parent) {}

MainWindow::MainWindow(std::unique_ptr<Owned> owned, QWidget* parent)
    : ui::ShellWindow(owned->workbench->environment(), parent), m_owned(std::move(owned)) {
    m_owned->workbench->attach(*this);
}

MainWindow::~MainWindow() {
    if (m_owned) {
        // As main() does: autosave and the journal while the window's documents are open.
        m_owned->workbench->shutdown();
    }
    // The workbench's teardown hook detaches the navigation workbench, then the editors and the
    // dispatcher, which refer to the workbench's services, go.
    teardown();
}

ui::Workbench* MainWindow::workbench() const noexcept { return m_owned ? m_owned->workbench->navigation() : nullptr; }

Workbench* MainWindow::ownedWorkbench() const noexcept { return m_owned ? m_owned->workbench.get() : nullptr; }

} // namespace frost::app
