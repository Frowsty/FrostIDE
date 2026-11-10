// SPDX-License-Identifier: Apache-2.0
// Workbench integration for trusted LuaLS projects and Lua document diagnostics: the pinned LuaLS
// (ADR 0079) launched through IProcessLauncher with its log and meta under the user profile, the
// Lua host providers' contributions (LUA-13; FrostEngine's, FEC-10a, is registered here, the
// composition root, so the Lua module holds no FrostEngine code), the live workspace protocol
// (LUA-3: didChangeConfiguration on settings changes, the file watcher's changes for the watchers
// LuaLS registers, $/progress as background tasks with LuaLS's own titles, refresh requests and
// multi-root folders) and a visible notice when LuaLS is missing.
#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/frost_engine_lua.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/sched/lane.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/background_tasks.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/shell_actions.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "language_bridge.hpp"
#include <QMetaObject>

#include <algorithm>
#include <limits>
#include <map>

namespace frost::app {

// The Lua state the workbench keeps across LuaLS starts. GUI thread only.
struct WorkbenchLuaState {
    lang::LuaHostRegistry hosts;
    std::uint64_t generation = 0;        // Increments per LuaLS start and stop; older callbacks are dropped.
    std::vector<std::string> folderKeys; // documentKeys of the running LuaLS's extra folders, sorted.
    std::vector<std::string> inputKeys;  // documentKeys of the host providers' inputs.
    std::map<std::string, std::shared_ptr<ui::BackgroundTask>> tasks; // $/progress token -> its task.
    std::vector<std::string> titles;    // Every progress title LuaLS began for this project, in order.
    std::vector<std::string> refreshes; // Every refresh request handled for this project, in order.
    lang::DiagnosticsStore inspected;   // Batch `lua-language-server --check` results (LUA-6).
    void finishTasks() {
        for (auto& [token, task] : tasks)
            task->finish();
        tasks.clear();
    }
};

namespace {
sched::Task<void> stopOwnedLua(std::shared_ptr<lang::LuaService> server) { co_await server->stop(); }

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

lang::FileDiagnostics emptyFileDiagnostics(std::string file) {
    lang::FileDiagnostics empty;
    empty.file = std::move(file);
    return empty;
}

WorkbenchLuaState& luaState(std::shared_ptr<WorkbenchLuaState>& slot) {
    if (!slot) {
        slot = std::make_shared<WorkbenchLuaState>();
        // The composition root's hosts: FrostEngine (FEC-10a) on the host API (LUA-13).
        slot->hosts.add(std::make_shared<lang::FrostEngineLuaProvider>());
    }
    return *slot;
}

// What LuaLS gets for one project: the root's configuration, the hosts' extra folders with their
// own, and the inputs whose change composes them again.
struct LuaPlan {
    lang::Json configuration;
    std::vector<lang::LuaWorkspaceFolder> folders;
    std::vector<std::string> inputKeys;
    bool hosted = false; // A host provider claimed the root.
};

// The configuration of `folder`: its hosts' contributions, their meta files written below
// `stateDirectory` (never into the workspace), composed over the user's runtime version.
lang::Json composeFolder(io::IFileSystem& fs, const lang::LuaHostRegistry& hosts, const std::string& runtime,
                         const std::string& stateDirectory, const std::string& folder,
                         std::vector<lang::LuaHostContribution>& contributions, std::vector<std::string>& inputKeys) {
    contributions = hosts.contributions(fs, folder);
    std::vector<std::string> metaLibraries;
    for (const lang::LuaHostContribution& contribution : contributions) {
        log::info("workbench", "LuaLS host {} for {}: {}", contribution.provider, folder, contribution.note);
        for (const std::string& input : contribution.inputs)
            inputKeys.push_back(lang::documentKey(input));
        if (contribution.metaFiles.empty())
            continue;
        auto directory = lang::luaHostMetaDirectory(stateDirectory, contribution.provider, folder);
        if (!directory) {
            log::warn("workbench", "LuaLS host {} meta folder: {}", contribution.provider,
                      directory.error().describe());
            continue;
        }
        if (auto written = lang::writeLuaMetaFiles(fs, *directory, contribution.metaFiles); !written) {
            log::warn("workbench", "LuaLS host {} meta files for {}: {}", contribution.provider, folder,
                      written.error().describe());
            continue;
        }
        metaLibraries.push_back(*directory);
    }
    return lang::composeLuaConfiguration(runtime, contributions, metaLibraries);
}

LuaPlan planLua(io::IFileSystem& fs, const lang::LuaHostRegistry& hosts, const std::string& runtime,
                const std::string& stateDirectory, const std::string& root) {
    LuaPlan plan;
    std::vector<lang::LuaHostContribution> contributions;
    plan.configuration = composeFolder(fs, hosts, runtime, stateDirectory, root, contributions, plan.inputKeys);
    plan.hosted = !contributions.empty();
    std::vector<std::string> seen{lang::documentKey(root)};
    for (const lang::LuaHostContribution& contribution : contributions) {
        for (const std::string& folder : contribution.folders) {
            auto normalized = io::normalizePath(folder);
            if (!normalized || !io::isAbsolutePath(*normalized) ||
                std::ranges::contains(seen, lang::documentKey(*normalized)))
                continue;
            seen.push_back(lang::documentKey(*normalized));
            std::vector<lang::LuaHostContribution> inner;
            lang::Json configuration =
                composeFolder(fs, hosts, runtime, stateDirectory, *normalized, inner, plan.inputKeys);
            plan.folders.push_back({*normalized, std::move(configuration)});
        }
    }
    return plan;
}

std::vector<std::string> sortedFolderKeys(const std::vector<lang::LuaWorkspaceFolder>& folders) {
    std::vector<std::string> keys;
    for (const auto& folder : folders)
        keys.push_back(lang::documentKey(folder.path));
    std::ranges::sort(keys);
    return keys;
}

// Whether the path `key` is `input`, lies below it, or is a folder above it (documentKeys).
bool touches(std::string_view key, std::string_view input) {
    const auto below = [](std::string_view inner, std::string_view outer) {
        return inner.size() > outer.size() && inner.starts_with(outer) &&
               (inner[outer.size()] == '/' || inner[outer.size()] == '\\');
    };
    return key == input || below(key, input) || below(input, key);
}
} // namespace

bool isLuaSource(std::string_view path) noexcept {
    if (path.size() < 4)
        return false;
    auto ext = path.substr(path.size() - 4);
    return ext[0] == '.' && (ext[1] == 'l' || ext[1] == 'L') && (ext[2] == 'u' || ext[2] == 'U') &&
           (ext[3] == 'a' || ext[3] == 'A');
}
lang::LuaHostRegistry& Workbench::luaHosts() { return luaState(m_luaState).hosts; }
std::vector<std::string> Workbench::luaRefreshesHandled() const {
    return m_luaState ? m_luaState->refreshes : std::vector<std::string>{};
}
std::vector<std::string> Workbench::luaProgressTitles() const {
    return m_luaState ? m_luaState->titles : std::vector<std::string>{};
}
std::vector<lang::FileDiagnostics> Workbench::luaInspectedDiagnostics() const {
    return m_luaState ? m_luaState->inspected.files() : std::vector<lang::FileDiagnostics>{};
}
void Workbench::updateLanguageTarget() {
    DocumentEditorView* active = currentView();
    m_bridge->setActivePath(active ? std::string_view(active->path()) : std::string_view{});
    // The insight binding asks for capabilities and encoding as well as document requests. Keep
    // it on the active editor so those values always belong to the same document's server.
    for (auto* view : m_editors->views()) {
        const bool isCpp = isCFamilySource(view->path());
        const bool isCs = isManagedSource(view->path());
        const bool isLua = isLuaSource(view->path());
        if (!isCpp && !isCs && !isLua)
            continue;
        auto* binding = view->editor().findChild<ui::lang::EditorInsightBinding*>();
        if (view != active && binding)
            delete binding;
        else if (view == active && !binding) {
            if (isCpp && m_clangd) {
                binding = new ui::lang::EditorInsightBinding(view->editor(), {*m_bridge, *m_pool});
                binding->attach(*m_clangd);
            } else if ((isCs && m_managed) || (isLua && m_lua)) {
                binding = new ui::lang::EditorInsightBinding(view->editor(), {*m_bridge, *m_pool});
                binding->setCodeVisionEnabled(true);
                if (isLua) {
                    if (auto set = m_lua->diagnostics().forFile(view->path()); set && !set->stale)
                        binding->setLiveDiagnostics(*set);
                    else if (m_luaState) {
                        if (auto inspected = m_luaState->inspected.forFile(view->path()))
                            binding->setLiveDiagnostics(*inspected);
                    }
                } else if (auto set = m_managed->diagnostics().forFile(view->path()); set && !set->stale) {
                    binding->setLiveDiagnostics(*set);
                }
                binding->refreshSemanticTokens();
            }
        }
    }
}
void Workbench::offerLuaTrust() { offerProjectTrust(); }
void Workbench::startLua(const std::string& root) {
    if (m_shutDown)
        return;
    stopLua();
    WorkbenchLuaState& state = luaState(m_luaState);
    state.titles.clear();
    state.refreshes.clear();
    m_luaUnavailable.clear();
    m_luaObservedTrust = m_services.trust.isTrusted(root);
    m_luaConfiguredPath = m_services.settings.snapshot()->get(settings::key::toolsLuaLanguageServerPath);
    m_luaRuntimeVersion = m_services.settings.snapshot()->get(settings::key::luaRuntimeVersion);
    if (!m_luaObservedTrust) {
        for (auto* view : m_editors->views())
            if (isLuaSource(view->path())) {
                offerLuaTrust();
                break;
            }
        return;
    }
    auto snapshot = m_services.settings.snapshot();
    lang::LuaSearch search = m_options.luaSearch ? *m_options.luaSearch : lang::defaultLuaSearch();
    search.configuredPath = snapshot->get(settings::key::toolsLuaLanguageServerPath);
    auto found = lang::findLuaLanguageServer(m_services.fs, search);
    // LuaLS writes its log and generated meta files under the user profile, never into its
    // (possibly read-only, staged) installation directory; host meta files go there too.
    std::string stateDirectory = m_options.luaStateDirectory;
    if (stateDirectory.empty()) {
        if (auto trustFile = project::TrustStore::defaultStorePath()) {
            if (auto under = io::joinPath(io::parentPath(*trustFile), "luals"))
                stateDirectory = std::move(*under);
        }
    }
    if (!found) {
        log::info("workbench", "LuaLS unavailable: {}", found.error().describe());
        m_luaUnavailable = found.error().message();
        // Visible, not only logged (LUA-1): when the user asked for a server, a host (FrostEngine)
        // claims the project or a Lua file is open; otherwise when the first Lua file opens
        // (syncLuaDocument). Asking the hosts reads their files, never starts a process.
        const bool luaOpen =
            std::ranges::any_of(m_editors->views(), [](auto* view) { return isLuaSource(view->path()); });
        const bool hosted = !state.hosts.contributions(m_services.fs, root).empty();
        if (!m_luaConfiguredPath.empty())
            m_luaUnavailableNotifiedRoot.clear(); // Each wrong setting is reported, as before.
        if (!m_luaConfiguredPath.empty() || hosted || luaOpen)
            notifyLuaUnavailable();
        return;
    }
    auto arguments = lang::luaLanguageServerArguments(stateDirectory);
    if (!arguments) {
        m_luaUnavailable = "no per-user directory for the LuaLS log and meta files: " + arguments.error().message();
        notifyLuaUnavailable();
        return;
    }
    if (!m_reactor) {
        auto reactor = io::Reactor::create(io::ReactorOptions{1, "frost-lua-io"});
        if (!reactor) {
            log::warn("workbench", "LuaLS reactor: {}", reactor.error().describe());
            return;
        }
        m_reactor = std::move(*reactor);
    }
    // The hosts' contributions: for FrostEngine the globals and Lua version its generated scripting
    // reference declares and its generated definitions folders, and an engine checkout's projects as
    // their own folders. A project's .luarc.json still has priority in LuaLS, key by key.
    LuaPlan plan = planLua(m_services.fs, state.hosts, m_luaRuntimeVersion, stateDirectory, root);
    lang::LuaServiceOptions options;
    options.serverPath = *found;
    options.workspaceRoot = root;
    options.arguments = std::move(*arguments);
    options.trusted = true;
    options.configuration = std::move(plan.configuration);
    options.folders = plan.folders;
    state.folderKeys = sortedFolderKeys(plan.folders);
    state.inputKeys = std::move(plan.inputKeys);
    const std::uint64_t generation = ++state.generation;
    options.onLifecycle = [self = QPointer<Workbench>(this), generation] {
        if (self)
            QMetaObject::invokeMethod(
                self.data(),
                [self, generation] {
                    if (!self)
                        return;
                    // A server that ended mid-progress never sends its end: its tasks end here.
                    if (self->m_luaState && self->m_luaState->generation == generation &&
                        (!self->m_lua || self->m_lua->state() != lang::LuaState::Running))
                        self->m_luaState->finishTasks();
                    // Only the LuaLS's own documents: a lifecycle change must not cancel a
                    // Structure request another server (clangd) is answering for the current file.
                    if (self->m_language && self->currentView() && isLuaSource(self->currentView()->path())) {
                        self->m_language->structureChanged();
                        if (self->m_lua && self->m_lua->state() == lang::LuaState::Running) {
                            if (auto* binding =
                                    self->currentView()->editor().findChild<ui::lang::EditorInsightBinding*>())
                                binding->refreshSemanticTokens();
                        }
                    }
                },
                Qt::QueuedConnection);
    };
    // Both run on the reader thread; the queued calls die with this object.
    options.onProgress = [self = QPointer<Workbench>(this), generation](const lang::LuaProgress& progress) {
        if (self)
            QMetaObject::invokeMethod(
                self.data(),
                [self, generation, progress] {
                    if (self)
                        self->onLuaProgress(generation, progress);
                },
                Qt::QueuedConnection);
    };
    options.onRefresh = [self = QPointer<Workbench>(this), generation](std::string_view method) {
        if (self)
            QMetaObject::invokeMethod(
                self.data(),
                [self, generation, method = std::string(method)] {
                    if (self)
                        self->onLuaRefresh(generation, method);
                },
                Qt::QueuedConnection);
    };
    m_lua = std::make_shared<lang::LuaService>(m_services.launcher, io::AsyncContext{*m_reactor, *m_pool},
                                               std::move(options));
    m_luaListener = m_lua->diagnostics().addListener([this](const lang::FileDiagnostics& data) {
        QMetaObject::invokeMethod(this, [this, file = data.file] { onLuaDiagnostics(file); }, Qt::QueuedConnection);
    });
    if (auto started = m_lua->start(); !started) {
        log::warn("workbench", "LuaLS startup: {}", started.error().describe());
        stopLua();
        m_luaUnavailable = started.error().message();
        notifyLuaUnavailable();
        return;
    }
    m_luaUnavailable.clear();
    m_bridge->setLuaServer(m_lua);
    for (auto* view : m_editors->uniqueDocumentViews())
        syncLuaDocument(*view);
    updateLanguageTarget();
    m_language->structureChanged();
}
void Workbench::refreshLuaConfiguration() {
    if (!m_lua || m_projectRoot.empty())
        return;
    WorkbenchLuaState& state = luaState(m_luaState);
    m_luaRuntimeVersion = m_services.settings.snapshot()->get(settings::key::luaRuntimeVersion);
    std::string stateDirectory = m_options.luaStateDirectory;
    if (stateDirectory.empty()) {
        if (auto trustFile = project::TrustStore::defaultStorePath()) {
            if (auto under = io::joinPath(io::parentPath(*trustFile), "luals"))
                stateDirectory = std::move(*under);
        }
    }
    LuaPlan plan = planLua(m_services.fs, state.hosts, m_luaRuntimeVersion, stateDirectory, m_projectRoot);
    if (sortedFolderKeys(plan.folders) != state.folderKeys) {
        // LuaLS appends folders that arrive later (didChangeWorkspaceFolders) after the root, where
        // a nested folder would lose its files; a new set of folders starts LuaLS again.
        log::info("workbench", "LuaLS workspace folders changed; restarting it");
        startLua(m_projectRoot);
        return;
    }
    state.inputKeys = std::move(plan.inputKeys);
    if (auto sent = m_lua->changeConfiguration(std::move(plan.configuration), std::move(plan.folders)); !sent)
        log::warn("workbench", "LuaLS didChangeConfiguration: {}", sent.error().describe());
}
void Workbench::onLuaSettingsChanged() {
    const auto snapshot = m_services.settings.snapshot();
    const auto luaPath = snapshot->get(settings::key::toolsLuaLanguageServerPath);
    const auto luaRuntime = snapshot->get(settings::key::luaRuntimeVersion);
    if (m_projectRoot.empty() || (luaPath == m_luaConfiguredPath && luaRuntime == m_luaRuntimeVersion))
        return;
    // Another server is a restart; another runtime version reaches the running LuaLS live
    // (didChangeConfiguration, LUA-3), so its diagnostics follow without losing its state.
    if (luaPath != m_luaConfiguredPath || !m_lua)
        startLua(m_projectRoot);
    else
        refreshLuaConfiguration();
}
void Workbench::onLuaFilesChanged(const io::FileChangeBatch& batch) {
    if (!m_lua || !m_luaState)
        return;
    std::vector<lang::LuaFileChange> changes;
    bool hostInputs = false;
    const auto add = [&](const std::string& path, lang::FileChangeType type) {
        changes.push_back({path, type});
        const std::string key = lang::documentKey(path);
        hostInputs = hostInputs || std::ranges::any_of(m_luaState->inputKeys,
                                                       [&](const std::string& input) { return touches(key, input); });
    };
    for (const io::FileChange& change : batch.changes) {
        switch (change.kind) {
        case io::FileChangeKind::Created: add(change.path, lang::FileChangeType::Created); break;
        case io::FileChangeKind::Modified:
        case io::FileChangeKind::Rescan: add(change.path, lang::FileChangeType::Changed); break;
        case io::FileChangeKind::Removed: add(change.path, lang::FileChangeType::Deleted); break;
        case io::FileChangeKind::Renamed:
            add(change.oldPath, lang::FileChangeType::Deleted);
            add(change.path, lang::FileChangeType::Created);
            break;
        }
    }
    // Only what the server registered watchers for (LuaLS 3.19.1 registers none: it watches the
    // workspace itself, so this sends nothing to it).
    if (auto sent = m_lua->notifyFileChanges(changes); !sent)
        log::warn("workbench", "LuaLS didChangeWatchedFiles: {}", sent.error().describe());
    else if (*sent > 0)
        log::debug("workbench", "LuaLS didChangeWatchedFiles: {} changes", *sent);
    // A host's own files changed (the engine wrote its reference or definitions): compose again.
    if (hostInputs)
        refreshLuaConfiguration();
}
void Workbench::onLuaProgress(std::uint64_t generation, const lang::LuaProgress& progress) {
    if (!m_lua || !m_luaState || m_luaState->generation != generation)
        return;
    WorkbenchLuaState& state = *m_luaState;
    // The truth rule (ADR 0078): LuaLS's own title, message and percentage, nothing estimated.
    switch (progress.kind) {
    case lang::LuaProgress::Kind::Begin: {
        state.titles.push_back(progress.title);
        if (auto it = state.tasks.find(progress.token); it != state.tasks.end()) {
            it->second->finish();
            state.tasks.erase(it);
        }
        if (!m_window)
            return;
        auto task = m_window->backgroundTasks().start(QStringLiteral("LuaLS: %1").arg(qs(progress.title)), false);
        if (!progress.message.empty())
            task->setDetail(qs(progress.message));
        task->setProgress(progress.percentage ? std::optional<double>(*progress.percentage / 100.0) : std::nullopt);
        state.tasks.emplace(progress.token, std::move(task));
        return;
    }
    case lang::LuaProgress::Kind::Report:
        if (auto it = state.tasks.find(progress.token); it != state.tasks.end()) {
            if (!progress.message.empty())
                it->second->setDetail(qs(progress.message));
            if (progress.percentage)
                it->second->setProgress(*progress.percentage / 100.0);
        }
        return;
    case lang::LuaProgress::Kind::End:
        if (auto it = state.tasks.find(progress.token); it != state.tasks.end()) {
            it->second->finish();
            state.tasks.erase(it);
        }
        return;
    }
}
void Workbench::onLuaRefresh(std::uint64_t generation, const std::string& method) {
    if (!m_lua || !m_luaState || m_luaState->generation != generation)
        return;
    m_luaState->refreshes.push_back(method);
    log::debug("workbench", "LuaLS asked for {}", method);
    // Diagnostics arrive as publishDiagnostics (FrostIDE announces no pull diagnostics), so only
    // what the active Lua editor shows from requests is asked for again: semantic tokens, and with
    // them inlay hints and Code Vision (EditorInsightBinding::refreshSemanticTokens).
    if (method == "workspace/diagnostic/refresh")
        return;
    DocumentEditorView* view = currentView();
    if (view == nullptr || !isLuaSource(view->path()))
        return;
    if (auto* binding = view->editor().findChild<ui::lang::EditorInsightBinding*>())
        binding->refreshSemanticTokens();
}
void Workbench::stopLua() {
    if (m_luaTrustDialog) {
        m_luaTrustDialog->close();
        m_luaTrustDialog.clear();
    }
    if (m_luaState) {
        ++m_luaState->generation;
        m_luaState->finishTasks();
        m_luaState->inspected.clear();
    }
    if (!m_lua)
        return;
    m_language->editorChanged();
    m_bridge->setLuaServer(nullptr);
    m_lua->diagnostics().removeListener(m_luaListener);
    m_luaListener = 0;
    for (auto* view : m_editors->views()) {
        if (!isLuaSource(view->path()))
            continue;
        delete view->editor().findChild<ui::lang::EditorInsightBinding*>();
        view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, {});
        view->editor().setInsightEnabled(false);
        m_liveMarkers.erase(lang::documentKey(view->path()));
        refreshMarkers(*view);
    }
    auto old = std::move(m_lua);
    m_retiredLua.push_back(m_pool->spawn(sched::Lane::Background, stopOwnedLua(std::move(old))));
    refreshLiveProblems();
}
void Workbench::notifyLuaUnavailable() {
    if (m_luaUnavailable.empty() || m_luaUnavailableNotifiedRoot == m_projectRoot)
        return;
    m_luaUnavailableNotifiedRoot = m_projectRoot;
    // The truth rule (ADR 0078): no Lua results without LuaLS, and the user is told why.
    notify(QStringLiteral("Lua language server unavailable"),
           QStringLiteral("%1. Lua diagnostics, completion and navigation are off until LuaLS %2 is installed "
                          "(tools/bootstrap, bundled LuaLS: LUA-1) or Settings > Tools names a LuaLS executable.")
               .arg(QString::fromStdString(m_luaUnavailable),
                    QString::fromUtf8(lang::pinnedLuaLanguageServerVersion().data(),
                                      static_cast<qsizetype>(lang::pinnedLuaLanguageServerVersion().size()))),
           ui::NotificationKind::Warning);
}
void Workbench::syncLuaDocument(DocumentEditorView& view) {
    if (!isLuaSource(view.path()))
        return;
    if (!m_lua) {
        notifyLuaUnavailable(); // A Lua file opened in a trusted project whose LuaLS is missing.
        return;
    }
    const auto& document = view.document().document();
    if (m_lua->openDocument(view.path(), document.text())) {
        view.editor().setInsightEnabled(true);
        return;
    }
    text::ChangeSet whole;
    whole.baseVersion = std::numeric_limits<std::uint64_t>::max();
    whole.version = document.version();
    (void)m_lua->changeDocument(view.path(), whole, document.text());
}
void Workbench::onLuaDiagnostics(const std::string& file) {
    if (!m_lua || !isLuaSource(file))
        return;
    auto set = m_lua->diagnostics().forFile(file);
    if (set && set->stale)
        return;
    if (m_luaState)
        m_luaState->inspected.publish(emptyFileDiagnostics(file));
    std::vector<ui::LineMarker> markers;
    if (set)
        for (const auto& d : set->diagnostics) {
            if (d.line <= 0 ||
                (d.severity != lang::DiagnosticSeverity::Error && d.severity != lang::DiagnosticSeverity::Warning))
                continue;
            markers.push_back({LineIndex{static_cast<std::uint32_t>(d.line - 1)},
                               d.severity == lang::DiagnosticSeverity::Error ? ui::MarkerSeverity::Error
                                                                             : ui::MarkerSeverity::Warning,
                               d.message});
        }
    const auto key = lang::documentKey(file);
    if (markers.empty())
        m_liveMarkers.erase(key);
    else
        m_liveMarkers[key] = std::move(markers);
    for (DocumentEditorView* view : m_editors->views()) {
        if (lang::documentKey(view->path()) != key)
            continue;
        if (auto* binding = view->editor().findChild<ui::lang::EditorInsightBinding*>())
            binding->setLiveDiagnostics(set.value_or(emptyFileDiagnostics(file)));
        else
            view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, set ? ui::lang::toEditorDiagnostics(*set)
                                                                                : std::vector<ui::EditorDiagnostic>{});
        refreshMarkers(*view);
        // What the editor now shows, for the staged-build exit test (tests/tools/test_staged_luals.py,
        // run with FROST_LOG_LEVEL=debug). Every value is LuaLS's own.
        if (set)
            for (const auto& d : set->diagnostics)
                log::debug("workbench", "Lua diagnostic shown: {}:{} [{}] {}", file, d.line, d.code, d.message);
    }
    refreshLiveProblems();
}
Result<std::vector<lang::FileDiagnostics>> Workbench::inspectLuaCode(std::string_view scopeDirectory) {
    const std::string target = !scopeDirectory.empty() ? std::string(scopeDirectory) : m_projectRoot;
    if (target.empty())
        return fail(ErrorCode::InvalidArgument, "no Lua workspace or directory is open to inspect");
    const std::string trustRoot = !m_projectRoot.empty() ? m_projectRoot : target;
    if (!m_services.trust.isTrusted(trustRoot)) {
        offerLuaTrust();
        return fail(ErrorCode::Untrusted, "refusing to inspect an untrusted Lua workspace");
    }
    const auto snapshot = m_services.settings.snapshot();
    lang::LuaSearch search = m_options.luaSearch ? *m_options.luaSearch : lang::defaultLuaSearch();
    search.configuredPath = snapshot->get(settings::key::toolsLuaLanguageServerPath);
    std::string stateDirectory = m_options.luaStateDirectory;
    if (stateDirectory.empty()) {
        if (auto trustFile = project::TrustStore::defaultStorePath()) {
            if (auto under = io::joinPath(io::parentPath(*trustFile), "luals"))
                stateDirectory = std::move(*under);
        }
    }
    lang::LuaInspectOptions options;
    options.directory = target;
    options.stateDirectory = stateDirectory;
    options.search = std::move(search);
    options.trusted = true;
    auto result = lang::inspectLuaDirectory(m_services.launcher, m_services.fs, options);
    if (!result) {
        if (auto found = lang::findLuaLanguageServer(m_services.fs, options.search); !found) {
            m_luaUnavailable = found.error().message();
            if (!options.search.configuredPath.empty())
                m_luaUnavailableNotifiedRoot.clear();
            notifyLuaUnavailable();
        }
        return result;
    }
    WorkbenchLuaState& state = luaState(m_luaState);
    const std::string scopeKey = lang::documentKey(target);
    const auto applyFileToViews = [this](const std::string& file, const std::optional<lang::FileDiagnostics>& set) {
        if (m_lua && m_lua->diagnostics().forFile(file))
            return;
        std::vector<ui::LineMarker> markers;
        if (set) {
            for (const auto& d : set->diagnostics) {
                if (d.line <= 0 ||
                    (d.severity != lang::DiagnosticSeverity::Error && d.severity != lang::DiagnosticSeverity::Warning))
                    continue;
                markers.push_back({LineIndex{static_cast<std::uint32_t>(d.line - 1)},
                                   d.severity == lang::DiagnosticSeverity::Error ? ui::MarkerSeverity::Error
                                                                                 : ui::MarkerSeverity::Warning,
                                   d.message});
            }
        }
        const std::string key = lang::documentKey(file);
        if (markers.empty())
            m_liveMarkers.erase(key);
        else
            m_liveMarkers[key] = std::move(markers);
        for (DocumentEditorView* view : m_editors->views()) {
            if (lang::documentKey(view->path()) != key)
                continue;
            if (auto* binding = view->editor().findChild<ui::lang::EditorInsightBinding*>())
                binding->setLiveDiagnostics(set.value_or(emptyFileDiagnostics(file)));
            else
                view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live,
                                              set ? ui::lang::toEditorDiagnostics(*set)
                                                  : std::vector<ui::EditorDiagnostic>{});
            refreshMarkers(*view);
        }
    };
    for (const lang::FileDiagnostics& previous : state.inspected.files()) {
        if (touches(lang::documentKey(previous.file), scopeKey)) {
            state.inspected.publish(emptyFileDiagnostics(previous.file));
            applyFileToViews(previous.file, std::nullopt);
        }
    }
    for (const lang::FileDiagnostics& file : *result) {
        state.inspected.publish(file);
        applyFileToViews(file.file, file);
    }
    refreshLiveProblems();
    if (m_window)
        m_window->toolWindows().show(ui::tool_windows::kProblems, false);
    return result;
}
} // namespace frost::app
