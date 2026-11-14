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
#include "frost/engine/engine_lua.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/frost_engine_lua.hpp"
#include "frost/lang/lua_rc.hpp"
#include "frost/lang/lua_input_index.hpp"
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

#include "engine_binding_integration.hpp"
#include "language_bridge.hpp"
#include <QMetaObject>
#include <QTimer>

#include <algorithm>
#include <limits>
#include <map>

namespace frost::app {

// The Lua state the workbench keeps across LuaLS starts. GUI thread only.
struct WorkbenchLuaState {
    lang::LuaHostRegistry hosts;
    std::shared_ptr<engine::EngineLuaHostProvider> engineHost;
    std::uint64_t generation = 0;        // Increments per LuaLS start and stop; older callbacks are dropped.
    std::vector<std::string> folderKeys; // documentKeys of the running LuaLS's extra folders, sorted.
    std::vector<std::string> inputKeys;  // documentKeys of the host providers' inputs.
    lang::LuaInputIndex inputIndex;
    bool configurationRefreshQueued = false;
    void setInputs(std::vector<std::string> inputs) {
        inputKeys = std::move(inputs);
        inputIndex.reset(inputKeys);
    }
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

WorkbenchLuaState& luaState(std::shared_ptr<WorkbenchLuaState>& slot, io::IProcessLauncher& launcher) {
    if (!slot) {
        slot = std::make_shared<WorkbenchLuaState>();
        // The composition root's hosts: FrostEngine (FEC-10a/FEC-10b) on the host API (LUA-13).
        slot->engineHost = std::make_shared<engine::EngineLuaHostProvider>(launcher);
        slot->hosts.add(slot->engineHost);
    }
    return *slot;
}

// What LuaLS gets for one project: the root's configuration, the hosts' extra folders with their
// own, and the inputs whose change composes them again.
struct LuaPlan {
    lang::Json configuration;
    std::vector<lang::LuaWorkspaceFolder> folders;
    std::vector<std::string> inputKeys;
    std::vector<std::string> metaLibraries;
    bool hosted = false; // A host provider claimed the root.
};

// The configuration of `folder`: its hosts' contributions, their meta files written below
// `stateDirectory` (never into the workspace), composed over the user's runtime version.
lang::Json composeFolder(io::IFileSystem& fs, const lang::LuaHostRegistry& hosts, const std::string& runtime,
                         const std::string& stateDirectory, const std::string& folder,
                         std::vector<lang::LuaHostContribution>& contributions, std::vector<std::string>& inputKeys,
                         std::vector<std::string>& outMetaLibraries) {
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
        if (outMetaLibraries.empty())
            outMetaLibraries.push_back(*directory);
    }
    lang::Json composed = lang::composeLuaConfiguration(runtime, contributions, metaLibraries);
    if (!runtime.empty() && runtime != "Lua 5.4") {
        if (lang::Json* rt = composed.find("runtime"))
            rt->set("version", runtime);
    }
    return composed;
}

LuaPlan planLua(io::IFileSystem& fs, const lang::LuaHostRegistry& hosts, const std::string& runtime,
                const std::string& stateDirectory, const std::string& root) {
    LuaPlan plan;
    std::vector<lang::LuaHostContribution> contributions;
    plan.configuration =
        composeFolder(fs, hosts, runtime, stateDirectory, root, contributions, plan.inputKeys, plan.metaLibraries);
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
            lang::Json configuration = composeFolder(fs, hosts, runtime, stateDirectory, *normalized, inner,
                                                     plan.inputKeys, plan.metaLibraries);
            plan.folders.push_back({*normalized, std::move(configuration)});
        }
    }
    return plan;
}

// When a host provider supplies generated `metaFiles` (`plan.metaLibraries`) and a workspace
// folder's `.luarc.json` also sets `workspace.library` (e.g. `[".frost/lua-api"]` in FrostEngine
// samples where `.frost/lua-api` is not generated on disk), LuaLS 3.19.1 replaces the client
// configuration's `Lua.workspace.library` with `.luarc.json`'s list. Writing a single-key override
// (`Lua.workspace.library`) under `stateDirectory` and passing `--configpath` keeps every other
// `.luarc.json` key authoritative while ensuring LuaLS loads the generated host meta library.
std::optional<std::string> writeHostLibraryOverrideIfNeeded(io::IFileSystem& fs, const std::string& stateDirectory,
                                                            const std::string& root, const LuaPlan& plan) {
    if (plan.metaLibraries.empty())
        return std::nullopt;
    std::vector<std::string> foldersToCheck{root};
    for (const lang::LuaWorkspaceFolder& f : plan.folders)
        foldersToCheck.push_back(f.path);
    bool rcHasLibraries = false;
    std::vector<std::string> mergedLibraries;
    for (const std::string& folder : foldersToCheck) {
        auto rc = lang::readLuaRc(fs, folder);
        if (!rc || !rc->exists || rc->settings.libraries.empty())
            continue;
        rcHasLibraries = true;
        for (const std::string& lib : rc->settings.libraries) {
            if (lib == ".frost/lua-api" || lib == ".frost\\lua-api")
                continue;
            if (!std::ranges::contains(mergedLibraries, lib))
                mergedLibraries.push_back(lib);
        }
    }
    if (!rcHasLibraries)
        return std::nullopt;
    for (const std::string& metaLib : plan.metaLibraries) {
        if (!std::ranges::contains(mergedLibraries, metaLib))
            mergedLibraries.push_back(metaLib);
    }
    lang::Json libArray = lang::Json::array();
    for (const std::string& item : mergedLibraries)
        libArray.push(item);
    const lang::Json overrideJson = lang::Json::object().set("Lua.workspace.library", std::move(libArray));
    auto overridePath = io::joinPath(stateDirectory, "host-library-override.json");
    if (!overridePath)
        return std::nullopt;
    if (auto made = fs.createDirectories(stateDirectory); !made)
        return std::nullopt;
    if (auto written = fs.writeFileAtomic(*overridePath, overrideJson.dump() + "\n"); !written)
        return std::nullopt;
    return *overridePath;
}

std::vector<std::string> sortedFolderKeys(const std::vector<lang::LuaWorkspaceFolder>& folders) {
    std::vector<std::string> keys;
    for (const auto& folder : folders)
        keys.push_back(lang::documentKey(folder.path));
    std::ranges::sort(keys);
    return keys;
}

// Whether the path `key` is `input`, lies below it, or is a folder above it (documentKeys).
sched::Task<void> prepareLuaPlanTask(io::IFileSystem* fs, std::shared_ptr<WorkbenchLuaState> luaStatePtr,
                                     std::string runtime, std::string stateDirectory, std::string root,
                                     QPointer<Workbench> self,
                                     std::function<void(LuaPlan, std::optional<std::string>)> onReady) {
    LuaPlan plan = planLua(*fs, luaStatePtr->hosts, runtime, stateDirectory, root);
    std::optional<std::string> overridePath = writeHostLibraryOverrideIfNeeded(*fs, stateDirectory, root, plan);
    if (self) {
        QMetaObject::invokeMethod(
            self.data(),
            [onReady = std::move(onReady), plan = std::move(plan), overridePath = std::move(overridePath)]() mutable {
                onReady(std::move(plan), std::move(overridePath));
            },
            Qt::QueuedConnection);
    }
    co_return;
}

sched::Task<void> runLuaInspectTask(io::IProcessLauncher* launcher, io::IFileSystem* fs,
                                    lang::LuaInspectOptions options, QPointer<Workbench> self,
                                    std::function<void(Result<std::vector<lang::FileDiagnostics>>)> onDone) {
    auto result = lang::inspectLuaDirectory(*launcher, *fs, options);
    if (self) {
        QMetaObject::invokeMethod(
            self.data(),
            [onDone = std::move(onDone), result = std::move(result)]() mutable { onDone(std::move(result)); },
            Qt::QueuedConnection);
    }
    co_return;
}
} // namespace

bool isLuaSource(std::string_view path) noexcept {
    if (path.size() < 4)
        return false;
    auto ext = path.substr(path.size() - 4);
    return ext[0] == '.' && (ext[1] == 'l' || ext[1] == 'L') && (ext[2] == 'u' || ext[2] == 'U') &&
           (ext[3] == 'a' || ext[3] == 'A');
}
lang::LuaHostRegistry& Workbench::luaHosts() { return luaState(m_luaState, m_services.launcher).hosts; }
std::shared_ptr<engine::EngineLuaHostProvider> Workbench::engineLuaHostProvider() const {
    return m_luaState ? m_luaState->engineHost : nullptr;
}
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
    WorkbenchLuaState& state = luaState(m_luaState, m_services.launcher);
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
        const bool hosted = lang::discoverFrostEngineLua(m_services.fs, root).frostEngine;
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
    const std::uint64_t generation = ++state.generation;
    auto launchWithPlan = [this, generation, root, serverPath = *found](std::vector<std::string> args, LuaPlan plan,
                                                                        std::optional<std::string> overridePath) {
        if (m_shutDown || !m_luaState || m_luaState->generation != generation)
            return;
        WorkbenchLuaState& st = *m_luaState;
        if (overridePath)
            args.push_back("--configpath=" + *overridePath);
        lang::LuaServiceOptions options;
        options.serverPath = serverPath;
        options.workspaceRoot = root;
        options.arguments = std::move(args);
        options.trusted = true;
        options.configuration = std::move(plan.configuration);
        options.folders = plan.folders;
        st.folderKeys = sortedFolderKeys(plan.folders);
        st.setInputs(std::move(plan.inputKeys));
        options.onLifecycle = [self = QPointer<Workbench>(this), generation] {
            if (self)
                QMetaObject::invokeMethod(
                    self.data(),
                    [self, generation] {
                        if (!self)
                            return;
                        if (self->m_luaState && self->m_luaState->generation == generation && self->m_engineBindings)
                            self->m_engineBindings->cancel();
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
    };
    // When a built FrostEditor binary's schema.export is not yet cached, warm it and write the
    // generated meta files on a background worker so the GUI thread never stalls waiting for a
    // child process (FND-9b).
    if (state.engineHost && state.engineHost->needsSchemaWarmup(m_services.fs, root)) {
        QPointer<Workbench> self(this);
        std::vector<std::string> args = std::move(*arguments);
        m_retiredLua.push_back(m_pool->spawn(
            sched::Lane::UserVisible,
            prepareLuaPlanTask(&m_services.fs, m_luaState, m_luaRuntimeVersion, stateDirectory, root, self,
                               [self, args = std::move(args), launchWithPlan = std::move(launchWithPlan)](
                                   LuaPlan plan, std::optional<std::string> overridePath) mutable {
                                   if (!self)
                                       return;
                                   launchWithPlan(std::move(args), std::move(plan), std::move(overridePath));
                               })));
        return;
    }
    // The hosts' contributions: for FrostEngine the globals and Lua version its generated scripting
    // reference or schema.export declares, and an engine checkout's projects as their own folders.
    LuaPlan plan = planLua(m_services.fs, state.hosts, m_luaRuntimeVersion, stateDirectory, root);
    std::optional<std::string> overridePath =
        writeHostLibraryOverrideIfNeeded(m_services.fs, stateDirectory, root, plan);
    launchWithPlan(std::move(*arguments), std::move(plan), std::move(overridePath));
}
void Workbench::refreshLuaConfiguration() {
    if (m_engineBindings)
        m_engineBindings->cancel();
    if (!m_lua || m_projectRoot.empty())
        return;
    WorkbenchLuaState& state = luaState(m_luaState, m_services.launcher);
    m_luaRuntimeVersion = m_services.settings.snapshot()->get(settings::key::luaRuntimeVersion);
    std::string stateDirectory = m_options.luaStateDirectory;
    if (stateDirectory.empty()) {
        if (auto trustFile = project::TrustStore::defaultStorePath()) {
            if (auto under = io::joinPath(io::parentPath(*trustFile), "luals"))
                stateDirectory = std::move(*under);
        }
    }
    if (state.engineHost && state.engineHost->needsSchemaWarmup(m_services.fs, m_projectRoot)) {
        const std::uint64_t generation = state.generation;
        const std::string root = m_projectRoot;
        QPointer<Workbench> self(this);
        m_retiredLua.push_back(m_pool->spawn(
            sched::Lane::UserVisible,
            prepareLuaPlanTask(&m_services.fs, m_luaState, m_luaRuntimeVersion, stateDirectory, root, self,
                               [self, generation](LuaPlan plan, std::optional<std::string>&&) {
                                   if (!self || self->m_shutDown || !self->m_lua || !self->m_luaState ||
                                       self->m_luaState->generation != generation)
                                       return;
                                   if (sortedFolderKeys(plan.folders) != self->m_luaState->folderKeys) {
                                       log::info("workbench", "LuaLS workspace folders changed; restarting it");
                                       self->startLua(self->m_projectRoot);
                                       return;
                                   }
                                   self->m_luaState->setInputs(std::move(plan.inputKeys));
                                   if (auto sent = self->m_lua->changeConfiguration(std::move(plan.configuration),
                                                                                    std::move(plan.folders));
                                       !sent)
                                       log::warn("workbench", "LuaLS didChangeConfiguration: {}",
                                                 sent.error().describe());
                               })));
        return;
    }
    LuaPlan plan = planLua(m_services.fs, state.hosts, m_luaRuntimeVersion, stateDirectory, m_projectRoot);
    (void)writeHostLibraryOverrideIfNeeded(m_services.fs, stateDirectory, m_projectRoot, plan);
    if (sortedFolderKeys(plan.folders) != state.folderKeys) {
        // LuaLS appends folders that arrive later (didChangeWorkspaceFolders) after the root, where
        // a nested folder would lose its files; a new set of folders starts LuaLS again.
        log::info("workbench", "LuaLS workspace folders changed; restarting it");
        startLua(m_projectRoot);
        return;
    }
    state.setInputs(std::move(plan.inputKeys));
    if (auto sent = m_lua->changeConfiguration(std::move(plan.configuration), std::move(plan.folders)); !sent)
        log::warn("workbench", "LuaLS didChangeConfiguration: {}", sent.error().describe());
}
void Workbench::onLuaSettingsChanged() {
    if (m_engineBindings)
        m_engineBindings->cancel();
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
        hostInputs = hostInputs || m_luaState->inputIndex.touches(key);
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
    if (hostInputs && !m_luaState->configurationRefreshQueued) {
        auto state=m_luaState;
        state->configurationRefreshQueued=true;
        const auto generation=state->generation;
        QTimer::singleShot(75,this,[this,state,generation] {
            state->configurationRefreshQueued=false;
            if(m_luaState==state && state->generation==generation && m_lua)
                refreshLuaConfiguration();
        });
    }
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
    if (m_engineBindings)
        m_engineBindings->cancel();
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
    WorkbenchLuaState& state = luaState(m_luaState, m_services.launcher);
    const std::string scopeKey = lang::documentKey(target);
    const auto applyFileToViews = [this](const std::string& file, const lang::FileDiagnostics* set) {
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
            if (auto* binding = view->editor().findChild<ui::lang::EditorInsightBinding*>()) {
                if (set)
                    binding->setLiveDiagnostics(*set);
                else
                    binding->setLiveDiagnostics(emptyFileDiagnostics(file));
            } else
                view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live,
                                              set ? ui::lang::toEditorDiagnostics(*set)
                                                  : std::vector<ui::EditorDiagnostic>{});
            refreshMarkers(*view);
        }
    };
    for (const lang::FileDiagnostics& previous : state.inspected.files()) {
        if (touches(lang::documentKey(previous.file), scopeKey)) {
            state.inspected.publish(emptyFileDiagnostics(previous.file));
            applyFileToViews(previous.file, nullptr);
        }
    }
    for (const lang::FileDiagnostics& file : *result) {
        state.inspected.publish(file);
        applyFileToViews(file.file, &file);
    }
    refreshLiveProblems();
    if (m_window)
        m_window->toolWindows().show(ui::tool_windows::kProblems, false);
    return result;
}
void Workbench::inspectLuaCodeAsync(std::string_view scopeDirectory,
                                    std::function<void(Result<std::vector<lang::FileDiagnostics>>)> done) {
    const std::string target = !scopeDirectory.empty() ? std::string(scopeDirectory) : m_projectRoot;
    if (target.empty()) {
        done(fail(ErrorCode::InvalidArgument, "no Lua workspace or directory is open to inspect"));
        return;
    }
    const std::string trustRoot = !m_projectRoot.empty() ? m_projectRoot : target;
    if (!m_services.trust.isTrusted(trustRoot)) {
        offerLuaTrust();
        done(fail(ErrorCode::Untrusted, "refusing to inspect an untrusted Lua workspace"));
        return;
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
    QPointer<Workbench> self(this);
    m_retiredLua.push_back(m_pool->spawn(
        sched::Lane::UserVisible,
        runLuaInspectTask(&m_services.launcher, &m_services.fs, options, self,
                          [self, target, configuredPath = options.search.configuredPath, search = options.search,
                           done = std::move(done)](Result<std::vector<lang::FileDiagnostics>> result) mutable {
                              if (!self || self->m_shutDown) {
                                  done(fail(ErrorCode::Cancelled, "workbench shut down"));
                                  return;
                              }
                              if (!result) {
                                  if (auto found = lang::findLuaLanguageServer(self->m_services.fs, search); !found) {
                                      self->m_luaUnavailable = found.error().message();
                                      if (!configuredPath.empty())
                                          self->m_luaUnavailableNotifiedRoot.clear();
                                      self->notifyLuaUnavailable();
                                  }
                                  done(std::move(result));
                                  return;
                              }
                              WorkbenchLuaState& state = luaState(self->m_luaState, self->m_services.launcher);
                              const std::string scopeKey = lang::documentKey(target);
                              const auto applyFileToViews = [self](const std::string& file,
                                                                   const lang::FileDiagnostics* set) {
                                  if (self->m_lua && self->m_lua->diagnostics().forFile(file))
                                      return;
                                  std::vector<ui::LineMarker> markers;
                                  if (set) {
                                      for (const auto& d : set->diagnostics) {
                                          if (d.line <= 0 || (d.severity != lang::DiagnosticSeverity::Error &&
                                                              d.severity != lang::DiagnosticSeverity::Warning))
                                              continue;
                                          markers.push_back({LineIndex{static_cast<std::uint32_t>(d.line - 1)},
                                                             d.severity == lang::DiagnosticSeverity::Error
                                                                 ? ui::MarkerSeverity::Error
                                                                 : ui::MarkerSeverity::Warning,
                                                             d.message});
                                      }
                                  }
                                  const std::string key = lang::documentKey(file);
                                  if (markers.empty())
                                      self->m_liveMarkers.erase(key);
                                  else
                                      self->m_liveMarkers[key] = std::move(markers);
                                  for (DocumentEditorView* view : self->m_editors->views()) {
                                      if (lang::documentKey(view->path()) != key)
                                          continue;
                                      if (auto* binding = view->editor().findChild<ui::lang::EditorInsightBinding*>()) {
                                          if (set)
                                              binding->setLiveDiagnostics(*set);
                                          else
                                              binding->setLiveDiagnostics(emptyFileDiagnostics(file));
                                      } else
                                          view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live,
                                                                        set ? ui::lang::toEditorDiagnostics(*set)
                                                                            : std::vector<ui::EditorDiagnostic>{});
                                      self->refreshMarkers(*view);
                                  }
                              };
                              for (const lang::FileDiagnostics& previous : state.inspected.files()) {
                                  if (touches(lang::documentKey(previous.file), scopeKey)) {
                                      state.inspected.publish(emptyFileDiagnostics(previous.file));
                                      applyFileToViews(previous.file, nullptr);
                                  }
                              }
                              for (const lang::FileDiagnostics& file : *result) {
                                  state.inspected.publish(file);
                                  applyFileToViews(file.file, &file);
                              }
                              self->refreshLiveProblems();
                              if (self->m_window)
                                  self->m_window->toolWindows().show(ui::tool_windows::kProblems, false);
                              done(std::move(result));
                          })));
}
Result<void> Workbench::setLuaRuntimeVersion(std::string_view version) {
    if (auto set = m_services.settings.set(settings::Scope::Application, settings::key::luaRuntimeVersion,
                                           std::string(version));
        !set)
        return set;
    onLuaSettingsChanged();
    return {};
}
Result<void> Workbench::gotoCppBinding() {
    DocumentEditorView* view = currentView();
    if (view == nullptr || !isLuaSource(view->path()))
        return fail(ErrorCode::InvalidArgument, "no active Lua editor to resolve a C++ binding from");
    const std::string folder = !m_projectRoot.empty() ? m_projectRoot : std::string(io::parentPath(view->path()));
    if (!m_services.trust.isTrusted(folder)) {
        offerLuaTrust();
        return fail(ErrorCode::Untrusted, "refusing to resolve C++ bindings in an untrusted workspace");
    }
    WorkbenchLuaState& state = luaState(m_luaState, m_services.launcher);
    if (!state.engineHost)
        state.engineHost = std::make_shared<engine::EngineLuaHostProvider>(m_services.launcher);
    std::optional<std::string> engineRoot = state.engineHost->engineRootForWorkspace(m_services.fs, folder);
    if (!engineRoot)
        engineRoot = state.engineHost->engineRootForWorkspace(m_services.fs, io::parentPath(view->path()));
    if (!engineRoot)
        return fail(ErrorCode::NotFound, "workspace is not a FrostEngine checkout or project");
    std::optional<engine::SchemaDocument> schema = state.engineHost->schemaForWorkspace(m_services.fs, *engineRoot);
    if (!schema)
        return fail(ErrorCode::NotFound, "no FrostEngine schema.export is available (build FrostEditor first)");
    const std::string text = view->editor().editor().text().text();
    const ByteOffset caret = view->editor().editor().state().primary().head;
    auto resolved = engine::resolveLuaCppBindingSource(m_services.fs, *engineRoot, *schema, text, caret);
    if (!resolved) {
        notify(QStringLiteral("Go to C++ Binding"), qs(resolved.error().message()), ui::NotificationKind::Info);
        return std::unexpected(resolved.error());
    }
    return openPath(resolved->path, FileLocation{static_cast<int>(resolved->line), 1});
}
} // namespace frost::app
