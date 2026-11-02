// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp), the document side: the editor views' lifecycle, saving
// (Save, Save All, save on close, autosave, the build's Save All), external changes, the start-up
// recovery of unsaved files and the build's gutter markers.

#include "frost/app/workbench.hpp"
#include "frost/build/build_diagnostics.hpp"
#include "frost/core/log.hpp"
#include "frost/lang/uri.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/tr.hpp"

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

void Workbench::editorOpened(DocumentEditorView& view) {
    refreshMarkers(view);
    syncClangdDocument(view);
}

void Workbench::editorClosed(DocumentEditorView& view) {
    const std::string path = view.path();
    if (m_clangd) {
        (void)m_clangd->closeDocument(path);
    }
    m_liveMarkers.erase(lang::documentKey(path));
    if (auto closed = m_documents->close(view.document()); !closed) {
        log::warn(kLog, "closing {}: {}", path, closed.error().describe());
    }
}

void Workbench::editorTextChanged(DocumentEditorView& view, const text::ChangeSet& change) {
    if (m_clangd && m_clangd->state() != lang::ClangdState::Stopped && isCFamilySource(view.path())) {
        (void)m_clangd->changeDocument(view.path(), change, view.document().document().text());
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
    return m_services.prompts.confirmDiscard(m_window, displayName(view.path()), qs(saved.error().message()));
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
    afterSaved(document);
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

void Workbench::afterSaved(document::FileDocument& document) {
    if (DocumentEditorView* view = m_editors->find(document); view != nullptr) {
        view->markSaved();
    }
    if (m_clangd && isCFamilySource(document.path())) {
        (void)m_clangd->saveDocument(document.path());
    }
}

document::SaveAllResult Workbench::saveAllDocuments(document::SaveReason reason) {
    document::SaveAllResult result = m_documents->saveAll(reason);
    for (document::FileDocument* document : result.saved) {
        afterSaved(*document);
    }
    reportSaveAll(result);
    return result;
}

void Workbench::reportSaveAll(const document::SaveAllResult& result) {
    for (const document::FailedSave& failed : result.failed) {
        notify(text("Cannot save %1").arg(displayName(failed.document->path())), qs(failed.error.message()),
               ui::NotificationKind::Error);
    }
}

void Workbench::saveAll(Completion done) {
    // The build's Save All (ADR 0039's hook); build.saveAllBeforeBuild turns it off.
    if (!m_services.settings.snapshot()->get(settings::key::buildSaveAllBeforeBuild)) {
        done({});
        return;
    }
    const document::SaveAllResult result = saveAllDocuments(document::SaveReason::SaveAll);
    if (!result.failed.empty()) {
        done(std::unexpected(result.failed.front().error));
        return;
    }
    done({});
}

void Workbench::frameDeactivated() {
    if (m_shutDown) {
        return;
    }
    if (std::optional<document::SaveAllResult> saved = m_autosave->frameDeactivated(); saved) {
        for (document::FileDocument* document : saved->saved) {
            afterSaved(*document);
        }
        reportSaveAll(*saved);
    }
    scheduleAutosave();
}

void Workbench::frameActivated() {
    if (!m_shutDown) {
        checkExternalChanges();
    }
}

void Workbench::scheduleAutosave() {
    if (m_shutDown) {
        return;
    }
    const std::optional<IClock::SteadyTime> deadline = m_autosave->nextDeadline();
    if (!deadline) {
        m_autosaveTimer.stop();
        return;
    }
    const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - m_services.clock.steadyNow());
    m_autosaveTimer.start(static_cast<int>(std::clamp<std::int64_t>(wait.count() + 1, 0, 24 * 60 * 60 * 1000)));
}

void Workbench::pollAutosave() {
    if (m_shutDown) {
        return;
    }
    const document::AutosaveController::PollResult result = m_autosave->poll();
    if (result.autosave) {
        for (document::FileDocument* document : result.autosave->saved) {
            afterSaved(*document);
        }
        reportSaveAll(*result.autosave);
    }
    if (result.journal) {
        for (const Error& error : result.journal->errors) {
            log::warn(kLog, "hot-exit journal: {}", error.describe());
        }
    }
    scheduleAutosave();
}

// --- External changes ---

void Workbench::checkExternalChanges() {
    std::vector<document::FileDocument*> open;
    for (const auto& document : m_documents->documents()) {
        if (!document->untitled()) {
            open.push_back(document.get());
        }
    }
    for (document::FileDocument* document : open) {
        handleExternalChange(m_documents->checkDisk(*document));
    }
}

void Workbench::handleExternalChange(const document::ExternalChange& change) {
    if (change.document == nullptr) {
        return;
    }
    const std::string path = change.document->path();
    DocumentEditorView* view = m_editors->find(*change.document);
    switch (change.kind) {
    case document::ExternalChangeKind::None:
    case document::ExternalChangeKind::Touched: return;
    case document::ExternalChangeKind::Reloaded:
        // A clean buffer follows the disk silently (one undoable step).
        if (view != nullptr) {
            view->reloadFromDocument();
            syncClangdDocument(*view);
        }
        return;
    case document::ExternalChangeKind::MergePending: {
        // The document may be closed before the user answers: look it up again then.
        QPointer<Workbench> self(this);
        const auto decide = [self, path](document::MergeDecision decision) {
            if (!self) {
                return;
            }
            document::FileDocument* document = self->m_documents->find(path);
            if (document == nullptr) {
                return;
            }
            if (auto resolved = self->m_documents->resolveExternalChange(*document, decision); !resolved) {
                self->notify(text("Cannot resolve the change of %1").arg(displayName(path)),
                             qs(resolved.error().message()), ui::NotificationKind::Error);
                return;
            }
            if (DocumentEditorView* current = self->m_editors->find(*document); current != nullptr) {
                current->reloadFromDocument();
                self->syncClangdDocument(*current);
            }
        };
        if (m_window) {
            ui::Notification decision{
                text("%1 changed on disk").arg(displayName(path)),
                text("It has unsaved changes here and was changed by another program. "
                     "Keep your version or take the one on disk?"),
                ui::NotificationKind::Warning,
                true,
                {ui::NotificationAction{text("Keep my version"),
                                        [decide] { decide(document::MergeDecision::KeepBuffer); }},
                 ui::NotificationAction{text("Take disk version"),
                                        [decide] { decide(document::MergeDecision::TakeDisk); }}}};
            m_window->notifications().notify(std::move(decision));
        }
        return;
    }
    case document::ExternalChangeKind::Deleted:
        notify(text("%1 was deleted on disk").arg(displayName(path)),
               text("The editor keeps its text; saving writes the file again."), ui::NotificationKind::Warning);
        return;
    case document::ExternalChangeKind::OutOfDate:
    case document::ExternalChangeKind::Failed:
        notify(text("Cannot reload %1").arg(displayName(path)), change.error ? qs(change.error->message()) : QString(),
               ui::NotificationKind::Warning);
        return;
    }
}

// --- Recovery ---

int Workbench::offerRecovery() {
    if (m_recoveryOffered || m_options.journalDirectory.empty()) {
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
    storage::RecoveryScan scan = std::move(*found);
    std::size_t count = 0;
    for (storage::RecoveredJournal& journal : scan.journals) {
        if (auto entries = journal.entries(); entries) {
            count += entries->size();
        }
    }
    const auto discardAll = [&] {
        for (storage::RecoveredJournal& journal : scan.journals) {
            if (auto discarded = std::move(journal).discard(); !discarded) {
                log::warn(kLog, "discarding a hot-exit journal: {}", discarded.error().describe());
            }
        }
    };
    if (count == 0) {
        discardAll();
        return 0;
    }
    if (!m_services.prompts.confirmRestore(m_window, static_cast<int>(count))) {
        discardAll();
        return 0;
    }
    document::RestoreReport report = m_documents->restore(scan);
    for (const Error& error : report.errors) {
        notify(text("Cannot restore an unsaved file"), qs(error.message()), ui::NotificationKind::Error);
    }
    int restored = 0;
    for (document::FileDocument* document : report.restored) {
        if (document->untitled() || !m_files) {
            continue;
        }
        if (m_files->open(document->path())) {
            ++restored;
        }
    }
    for (document::FileDocument* document : report.diskChanged) {
        notify(text("%1 changed on disk").arg(displayName(document->path())),
               text("The restored text differs from the file on disk; saving overwrites the file."),
               ui::NotificationKind::Warning);
    }
    return restored;
}

// --- Markers ---

void Workbench::refreshMarkers(DocumentEditorView& view) {
    std::vector<ui::LineMarker> markers;
    const std::string buildKey = build::documentKey(view.path());
    for (const build::BuildDiagnostic& d : m_build->diagnostics()) {
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
}

void Workbench::refreshAllMarkers() {
    for (DocumentEditorView* view : m_editors->views()) {
        refreshMarkers(*view);
    }
}

void Workbench::openLocation(const QString& path, int line, int column) {
    if (m_files) {
        (void)m_files->open(path.toStdString(), FileLocation{line, column});
    }
}

} // namespace frost::app
