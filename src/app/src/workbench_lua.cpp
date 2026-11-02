// SPDX-License-Identifier: Apache-2.0
// Workbench integration for trusted LuaLS projects and Lua document diagnostics.
#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/lang/uri.hpp"
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
    // it on the active C++ editor so those values always belong to the same document's server.
    for (auto* view : m_editors->views()) {
        if (!isCFamilySource(view->path()))
            continue;
        auto* binding = view->editor().findChild<ui::lang::EditorInsightBinding*>();
        if (view != active && binding)
            delete binding;
        else if (view == active && !binding && m_clangd) {
            binding = new ui::lang::EditorInsightBinding(view->editor(), {*m_bridge, *m_pool});
            binding->attach(*m_clangd);
        }
    }
}
void Workbench::offerLuaTrust() {
    if (m_projectRoot.empty() || m_services.trust.isTrusted(m_projectRoot) || !m_window || m_luaTrustDialog)
        return;
    auto* dialog = new ui::TrustDialog(
        *m_build, {QString::fromUtf8(m_projectRoot.data(), static_cast<qsizetype>(m_projectRoot.size()))}, m_window,
        ui::TrustPurpose::LuaLanguageServer);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    m_luaTrustDialog = dialog;
    const std::string root = m_projectRoot;
    connect(dialog, &QDialog::accepted, this, [this, root] {
        m_luaTrustDialog.clear();
        if (m_projectRoot == root && m_services.trust.isTrusted(root))
            startLua(root);
    });
    dialog->open();
}
void Workbench::startLua(const std::string& root) {
    stopLua();
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
    auto found = lang::findLuaLanguageServer(
        m_services.fs, lang::defaultLuaSearch(snapshot->get(settings::key::toolsLuaLanguageServerPath)));
    if (!found) {
        log::info("workbench", "LuaLS unavailable: {}", found.error().describe());
        if (!m_luaConfiguredPath.empty()) {
            notify(QStringLiteral("Lua language server"), QString::fromStdString(found.error().message()),
                   ui::NotificationKind::Warning);
        }
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
    options.trusted = true;
    // The engine's .luarc.json has priority and names its generated definitions.
    options.configuration = lang::luaDefaults(m_luaRuntimeVersion, false);
    m_lua = std::make_shared<lang::LuaService>(m_services.launcher, io::AsyncContext{*m_reactor, *m_pool},
                                               std::move(options));
    m_luaListener = m_lua->diagnostics().addListener([this](const lang::FileDiagnostics& data) {
        QMetaObject::invokeMethod(this, [this, file = data.file] { onLuaDiagnostics(file); }, Qt::QueuedConnection);
    });
    if (auto started = m_lua->start(); !started) {
        log::warn("workbench", "LuaLS startup: {}", started.error().describe());
        stopLua();
        return;
    }
    m_bridge->setLuaServer(m_lua);
    for (auto* view : m_editors->views())
        syncLuaDocument(*view);
    updateLanguageTarget();
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
        view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live, {});
        view->editor().setInsightEnabled(false);
        m_liveMarkers.erase(lang::documentKey(view->path()));
        refreshMarkers(*view);
    }
    auto old = std::move(m_lua);
    m_retiredLua.push_back(m_pool->spawn(sched::Lane::Background, stopOwnedLua(std::move(old))));
    refreshLiveProblems();
}
void Workbench::syncLuaDocument(DocumentEditorView& view) {
    if (!m_lua || !isLuaSource(view.path()))
        return;
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
    if (auto* view = m_editors->find(file)) {
        view->editor().setDiagnostics(ui::EditorDiagnosticSource::Live,
                                      set ? ui::lang::toEditorDiagnostics(*set) : std::vector<ui::EditorDiagnostic>{});
        refreshMarkers(*view);
    }
    refreshLiveProblems();
}
} // namespace frost::app
