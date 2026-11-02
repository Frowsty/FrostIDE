// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp), the document side: the editor views' lifecycle, the views
// following the registry's state changes (saves, reloads, merges), save on close, the build's Save
// All, the delegations to the navigation workbench (Save All, autosave, external changes, the
// restore prompt) and the build's gutter markers.

#include "frost/app/workbench.hpp"
#include "frost/build/build_diagnostics.hpp"
#include "frost/core/log.hpp"
#include "frost/lang/uri.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/tr.hpp"

#include "language_bridge.hpp"
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>

namespace frost::app {

namespace {

constexpr std::string_view kLog = "workbench";

QString text(const char* source) { return ui::qtr("frost::app::Workbench", source); }

QString qs(std::string_view utf8) { return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size())); }

QString displayName(std::string_view path) { return QFileInfo(qs(path)).fileName(); }

} // namespace

// --- Editor views ---

DocumentEditorView* Workbench::currentView() const {
    if (!m_window) {
        return nullptr;
    }
    const ui::IEditorView* tab = m_window->tabs().currentEditor();
    return tab != nullptr ? m_editors->find(tab->path()) : nullptr;
}

void Workbench::editorOpened(DocumentEditorView& view) {
    refreshMarkers(view);
    syncClangdDocument(view);
    syncLuaDocument(view);
    if (isLuaSource(view.path()) && !m_lua)
        offerLuaTrust();
    if (m_clangd && isCFamilySource(view.path()) && currentView() == &view) {
        auto* binding = new ui::lang::EditorInsightBinding(view.editor(), {*m_bridge, *m_pool});
        binding->attach(*m_clangd);
        refreshMarkers(view);
    }
    // The language features follow the caret of the active editor and its Ctrl+clicks.
    ui::EditorView* editor = &view.editor();
    connect(editor, &ui::EditorView::caretPositionChanged, this, [this, editor](int, int) {
        if (DocumentEditorView* current = currentView(); current != nullptr && &current->editor() == editor) {
            m_language->caretMoved();
        }
    });
    connect(editor, &ui::EditorView::ctrlClicked, this, [this, editor](ByteOffset offset) {
        if (DocumentEditorView* current = currentView(); current != nullptr && &current->editor() == editor) {
            m_language->gotoDeclarationAt(offset);
        }
    });
}

void Workbench::editorClosed(DocumentEditorView& view) {
    const std::string path = view.path();
    if (m_clangd && isCFamilySource(path))
        (void)m_clangd->closeDocument(path);
    if (m_lua && isLuaSource(path))
        (void)m_lua->closeDocument(path);
    m_language->editorChanged();
    m_liveMarkers.erase(lang::documentKey(path));
    // A dirty document whose close the user confirmed closes unsaved; otherwise the registry closes
    // a clean document when its last view goes and keeps a dirty one journaled.
    if (const auto it = std::ranges::find(m_discarded, path); it != m_discarded.end()) {
        m_discarded.erase(it);
        if (m_documentRegistry->viewCount(path) <= 1) {
            if (document::FileDocument* document = m_documents->find(path); document != nullptr) {
                if (auto closed = m_documents->close(*document); !closed) {
                    log::warn(kLog, "closing {}: {}", path, closed.error().describe());
                }
            }
        }
    }
}

void Workbench::editorTextChanged(DocumentEditorView& view, const text::ChangeSet& change) {
    if (m_clangd && m_clangd->state() != lang::ClangdState::Stopped && isCFamilySource(view.path())) {
        (void)m_clangd->changeDocument(view.path(), change, view.document().document().text());
    }
    if (m_lua && isLuaSource(view.path()))
        (void)m_lua->changeDocument(view.path(), change, view.document().document().text());
    // What the user typed, now in the document and with clangd: completion and parameter info.
    if (!m_typed.empty()) {
        const std::string typed = std::exchange(m_typed, {});
        if (DocumentEditorView* current = currentView(); current == &view) {
            m_language->textTyped(typed);
        }
    }
}

bool Workbench::editorCloseRequested(DocumentEditorView& view) {
    if (!view.document().dirty()) {
        return true;
    }
    // Rider's autosave: a dirty tab is saved as it closes; the user is asked only when that fails.
    Result<void> saved = saveQuietly(view.document());
    if (saved) {
        return true;
    }
    if (!m_services.prompts.confirmDiscard(m_window, displayName(view.path()), qs(saved.error().message()))) {
        return false;
    }
    m_discarded.push_back(view.path());
    return true;
}

void Workbench::documentChanged(const std::string& path) {
    DocumentEditorView* view = m_editors->find(path);
    if (view == nullptr) {
        return;
    }
    switch (view->followDocument()) {
    case DocumentEditorView::Followed::Reloaded:
        // The editor keeps its live diagnostics and usages (clamped) until clangd's next answer.
        syncClangdDocument(*view);
        syncLuaDocument(*view);
        if (auto* binding = view->editor().findChild<ui::lang::EditorInsightBinding*>()) {
            binding->refreshSemanticTokens();
        }
        break;
    case DocumentEditorView::Followed::Saved:
        if (m_lua && isLuaSource(view->path()))
            (void)m_lua->saveDocument(view->path());
        if (m_clangd && isCFamilySource(view->path())) {
            (void)m_clangd->saveDocument(view->path());
        }
        break;
    case DocumentEditorView::Followed::Nothing: break;
    }
}

// --- Saving ---

Result<void> Workbench::saveQuietly(document::FileDocument& document) {
    Result<document::SaveOutcome> outcome = m_documents->save(document);
    if (!outcome) {
        return std::unexpected(std::move(outcome.error()));
    }
    for (const Error& warning : outcome->warnings) {
        log::warn(kLog, "after saving {}: {}", document.path(), warning.describe());
    }
    m_tabs->refreshBanners(document.path());
    m_documentRegistry->notifyChanged(document.path());
    return {};
}

Result<void> Workbench::save(DocumentEditorView& view) {
    Result<void> saved = saveQuietly(view.document());
    if (!saved) {
        notify(text("Cannot save %1").arg(displayName(view.path())), qs(saved.error().message()),
               ui::NotificationKind::Error);
    }
    return saved;
}

document::SaveAllResult Workbench::saveAllDocuments() {
    if (m_nav) {
        return m_nav->saveAll();
    }
    document::SaveAllResult result = m_documents->saveAll(document::SaveReason::SaveAll);
    for (document::FileDocument* document : result.saved) {
        m_documentRegistry->notifyChanged(document->path());
    }
    return result;
}

void Workbench::saveAll(Completion done) {
    // The build's (and so the run's) Save All (ADR 0039's hook); build.saveAllBeforeBuild turns it off.
    if (!m_services.settings.snapshot()->get(settings::key::buildSaveAllBeforeBuild)) {
        done({});
        return;
    }
    const document::SaveAllResult result = saveAllDocuments();
    if (!result.failed.empty()) {
        done(std::unexpected(result.failed.front().error));
        return;
    }
    done({});
}

void Workbench::frameDeactivated() {
    if (!m_shutDown && m_nav) {
        m_nav->frameDeactivated();
    }
}

void Workbench::frameActivated() {
    if (!m_shutDown && m_nav) {
        m_nav->frameActivated();
    }
}

void Workbench::checkExternalChanges() {
    if (m_nav) {
        m_nav->reloadAllFromDisk();
    }
}

// --- Recovery ---

std::size_t Workbench::offerRecovery() {
    if (m_recoveryOffered || m_options.journalDirectory.empty() || !m_nav) {
        return 0;
    }
    m_recoveryOffered = true;
    // After the first frame: the scan waits for the lock of every journal a live instance holds.
    Result<storage::RecoveryScan> found = storage::findRecoverableJournals(
        m_options.journalDirectory, std::chrono::milliseconds(500), m_journal ? m_journal->path() : std::string());
    if (!found) {
        log::warn(kLog, "recovery scan failed: {}", found.error().describe());
        return 0;
    }
    // The navigation workbench's one restore prompt (Restore, Discard), which opens the tabs.
    return m_nav->offerRestore(std::move(*found));
}

// --- Markers ---

void Workbench::refreshMarkers(DocumentEditorView& view) {
    std::vector<ui::LineMarker> markers;
    std::vector<build::BuildDiagnostic> buildDiagnostics;
    const std::string buildKey = build::documentKey(view.path());
    for (const build::BuildDiagnostic& d : m_build->diagnostics()) {
        if (d.jumpable && build::documentKey(d.path) == buildKey) {
            buildDiagnostics.push_back(d);
        }
        if (!d.jumpable || d.diagnostic.line <= 0 || d.diagnostic.severity == build::Severity::Message ||
            build::documentKey(d.path) != buildKey) {
            continue;
        }
        std::string message =
            d.diagnostic.code.empty() ? d.diagnostic.message : d.diagnostic.code + ": " + d.diagnostic.message;
        markers.push_back(ui::LineMarker{LineIndex{static_cast<std::uint32_t>(d.diagnostic.line - 1)},
                                         d.diagnostic.severity == build::Severity::Error ? ui::MarkerSeverity::Error
                                                                                         : ui::MarkerSeverity::Warning,
                                         std::move(message)});
    }
    if (const auto live = m_liveMarkers.find(lang::documentKey(view.path())); live != m_liveMarkers.end()) {
        markers.insert(markers.end(), live->second.begin(), live->second.end());
    }
    view.editor().setLineMarkers(std::move(markers));
    view.editor().setDiagnostics(ui::EditorDiagnosticSource::Build, ui::lang::toEditorDiagnostics(buildDiagnostics));
}

void Workbench::refreshAllMarkers() {
    for (DocumentEditorView* view : m_editors->views()) {
        refreshMarkers(*view);
    }
}

void Workbench::openLocation(const QString& path, int line, int column) {
    (void)openPath(path.toStdString(), FileLocation{line, column});
}

} // namespace frost::app
