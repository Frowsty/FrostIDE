// SPDX-License-Identifier: Apache-2.0
// Workbench integration for trusted LuaLS projects and Lua document diagnostics: the pinned LuaLS
// (ADR 0079) launched through IProcessLauncher with its log and meta under the user profile, the
// FrostEngine defaults for a .frostproj workspace, and a visible notice when LuaLS is missing.
#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/sched/lane.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/toolwindows/trust_dialog.hpp"

#include "language_bridge.hpp"
#include <QMetaObject>

#include <algorithm>
#include <limits>

namespace frost::app {
namespace {
sched::Task<void> stopOwnedLua(std::shared_ptr<lang::LuaService> server) { co_await server->stop(); }
} // namespace
bool isLuaSource(std::string_view path) noexcept {
    if (path.size() < 4)
        return false;
    auto ext = path.substr(path.size() - 4);
    return ext[0] == '.' && (ext[1] == 'l' || ext[1] == 'L') && (ext[2] == 'u' || ext[2] == 'U') &&
           (ext[3] == 'a' || ext[3] == 'A');
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
    const bool frostEngine = lang::isFrostEngineLuaWorkspace(m_services.fs, root);
    if (!found) {
        log::info("workbench", "LuaLS unavailable: {}", found.error().describe());
        m_luaUnavailable = found.error().message();
        // Visible, not only logged (LUA-1): when the user asked for a server, works on a FrostEngine
        // project or has a Lua file open; otherwise when the first Lua file opens (syncLuaDocument).
        const bool luaOpen =
            std::ranges::any_of(m_editors->views(), [](auto* view) { return isLuaSource(view->path()); });
        if (!m_luaConfiguredPath.empty())
            m_luaUnavailableNotifiedRoot.clear(); // Each wrong setting is reported, as before.
        if (!m_luaConfiguredPath.empty() || frostEngine || luaOpen)
            notifyLuaUnavailable();
        return;
    }
    // LuaLS writes its log and generated meta files under the user profile, never into its
    // (possibly read-only, staged) installation directory.
    std::string stateDirectory = m_options.luaStateDirectory;
    if (stateDirectory.empty()) {
        if (auto trustFile = project::TrustStore::defaultStorePath()) {
            if (auto under = io::joinPath(io::parentPath(*trustFile), "luals"))
                stateDirectory = std::move(*under);
        }
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
    lang::LuaServiceOptions options;
    options.serverPath = *found;
    options.workspaceRoot = root;
    options.arguments = std::move(*arguments);
    options.trusted = true;
    // A FrostEngine project (a .frostproj in the root) gets the engine's defaults; the engine's
    // .luarc.json still has priority in LuaLS and names its generated definitions.
    options.configuration = lang::luaDefaults(m_luaRuntimeVersion, frostEngine);
    options.onLifecycle = [self = QPointer<Workbench>(this)] {
        if (self)
            QMetaObject::invokeMethod(
                self.data(),
                [self] {
                    if (self && self->m_language)
                        self->m_language->structureChanged();
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
void Workbench::stopLua() {
    if (m_luaTrustDialog) {
        m_luaTrustDialog->close();
        m_luaTrustDialog.clear();
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
        view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live,
                                      set ? ui::lang::toEditorDiagnostics(*set) : std::vector<ui::EditorDiagnostic>{});
        refreshMarkers(*view);
    }
    refreshLiveProblems();
}
} // namespace frost::app
