// SPDX-License-Identifier: Apache-2.0
//
// AppServices (frost/app/app_services.hpp).

#include "frost/app/app_services.hpp"

#include "frost/core/assert.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/log.hpp"
#include "frost/input/keymap.hpp"
#include "frost/input/layout.hpp"
#include "frost/input/simulated_keyboard.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/settings/project_layout.hpp"
#include "frost/ui/nav/paths.hpp"

#include <QApplication>
#include <QGuiApplication>
#include <QStyleHints>

#include <chrono>
#include <utility>

namespace frost::app {

AppServices::AppServices(AppServicesOptions options) : m_editors(io::realFileSystem()) {
    // The thread's active layout; en-US when it cannot be read (ADR 0016 decision 12).
    Result<input::LayoutTable> active = input::readActiveLayout();
    const input::LayoutTable layout =
        active ? std::move(*active) : input::simulatedLayout(input::SimulatedLayoutId::EnUs);
    const auto interval = std::chrono::milliseconds(QGuiApplication::styleHints()->mouseDoubleClickInterval());
    Result<input::KeymapService> keymap =
        input::KeymapService::create(m_registry, m_keymapSettings, input::currentKeymapPlatform(), layout, interval);
    // create() fails only when the embedded keymap does not parse, which its tests rule out.
    FROST_ASSERT(keymap.has_value());
    m_keymap.emplace(std::move(*keymap));
    if (options.persistent) {
        startPersistence(options);
    }
    document::SaveManagerServices services;
    services.fs = &io::realFileSystem();
    services.clock = &systemClock();
    services.journal = m_journal ? &*m_journal : nullptr;
    m_saveManager = std::make_unique<document::SaveManager>(services);
    m_documents = std::make_unique<ui::nav::DocumentRegistry>(*m_saveManager, io::realFileSystem());
    m_documentEditors = std::make_unique<ui::DocumentEditorFactory>(m_editors, *m_documents);
    m_autosave = std::make_unique<document::AutosaveController>(*m_saveManager, systemClock());
}

void AppServices::startPersistence(const AppServicesOptions& options) {
    std::string data = options.dataDirectory;
    if (data.empty()) {
        auto trust = project::TrustStore::defaultStorePath();
        if (!trust) {
            m_warnings.push_back("no data directory: " + trust.error().message());
            return;
        }
        data = std::string(ui::nav::parentOf(*trust));
    }
    m_sessions = std::make_unique<ui::nav::SessionStore>(io::realFileSystem(), ui::nav::joinIn(data, "sessions"));
    m_journalDirectory = ui::nav::joinIn(data, "hot-exit");
    if (auto journal = storage::HotExitJournal::create(m_journalDirectory, systemClock()); journal) {
        m_journal.emplace(std::move(*journal));
    } else {
        m_warnings.push_back("hot exit is off: " + journal.error().message());
    }
    m_pool = std::make_unique<sched::ThreadPool>();
    if (auto reactor = io::Reactor::create(); reactor) {
        m_reactor = std::move(*reactor);
        if (auto watcher = io::RealFileWatcher::create(*m_reactor, *m_pool); watcher) {
            m_watcher = std::move(*watcher);
        } else {
            m_warnings.push_back("no file watcher: " + watcher.error().message());
        }
    } else {
        m_warnings.push_back("no file watcher: " + reactor.error().message());
    }
    if (auto settingsFile = settings::applicationSettingsFile(); settingsFile) {
        m_applicationSettingsFile = std::move(*settingsFile);
    }
}

AppServices::~AppServices() {
    if (m_journal && m_saveManager) {
        if (auto closed = m_saveManager->closeJournal(); !closed) {
            log::warn("app", "hot-exit journal: {}", closed.error().message());
        }
    }
    // The exit order of frost/io/reactor.hpp: shut the reactor down (pending next() calls end), drop
    // the watcher, shut the pool down, then destroy the reactor. The window stopped its watches.
    if (m_reactor) {
        m_reactor->shutdown();
    }
    m_watcher.reset();
    if (m_pool) {
        m_pool->shutdown();
    }
    m_reactor.reset();
}

Result<ui::InstallReport> AppServices::installAppearance(QApplication& app, std::string resourceDir) {
    if (m_appearance != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the appearance is already installed");
    }
    m_os = std::make_unique<ui::QtOsAppearanceSource>();
    auto manager = std::make_unique<ui::AppearanceManager>(
        ui::AppearanceEnvironment{io::realFileSystem(), std::move(resourceDir), m_appearanceSettings, m_os.get()});
    Result<ui::InstallReport> report = manager->install(app);
    if (report) {
        m_appearance = std::move(manager);
    }
    return report;
}

ui::ShellEnvironment AppServices::environment(bool followActiveLayout) {
    ui::ShellEnvironment environment{m_registry, *m_keymap, m_shellSettings, *m_documentEditors};
    environment.appearance = m_appearance.get();
    environment.appearanceSettings = m_appearance != nullptr ? &m_appearanceSettings : nullptr;
    environment.followActiveLayout = followActiveLayout;
    return environment;
}

ui::WorkbenchServices AppServices::workbenchServices() {
    ui::WorkbenchServices services{io::realFileSystem(), *m_saveManager, *m_documents, *m_documentEditors,
                                   systemClock()};
    services.scheduler = m_pool.get();
    services.watcher = m_watcher.get();
    services.sessions = m_sessions.get();
    services.autosave = m_autosave.get();
    services.applicationSettingsFile = m_applicationSettingsFile;
    return services;
}

Result<storage::RecoveryScan> AppServices::scanRecoverableJournals() {
    if (m_journalDirectory.empty()) {
        return fail(ErrorCode::NotFound, "hot exit is not set up");
    }
    return storage::findRecoverableJournals(m_journalDirectory, std::chrono::milliseconds(500),
                                            m_journal ? m_journal->path() : std::string_view());
}

} // namespace frost::app
