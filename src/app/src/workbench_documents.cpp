// SPDX-License-Identifier: Apache-2.0
//
// Workbench (frost/app/workbench.hpp), the document side: the editor views' lifecycle, the views
// following the registry's state changes (saves, reloads, merges), save on close, the build's Save
// All, the delegations to the navigation workbench (Save All, autosave, external changes, the
// restore prompt) and the build's gutter markers.

#include "frost/app/workbench.hpp"
#include "frost/build/build_diagnostics.hpp"
#include "frost/core/log.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/settings/keys.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/lang/language_controller.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/tr.hpp"
#include "frost/ui/vcs/change_markers.hpp"

#include "git_integration.hpp"
#include "language_bridge.hpp"
#include "project_properties_dialog.hpp"
#include <QDir>
#include <QFileInfo>
#include <QLineEdit>
#include <QPushButton>

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
    if (tab == nullptr)
        return nullptr;
    QWidget* container = const_cast<ui::IEditorView*>(tab)->widget();
    for (DocumentEditorView* view : m_editors->views()) {
        if (container == &view->editor() || container->isAncestorOf(&view->editor()))
            return view;
    }
    return nullptr;
}

void Workbench::editorOpened(DocumentEditorView& view) {
    connect(&view.editor(), &ui::EditorView::textChanged, this, [this] { invalidateManagedDebug(); });
    if (m_git)
        m_git->attachEditor(view);
    if (m_debug)
        m_debug->attachEditor(&view.editor());
    refreshMarkers(view);
    if (m_documentRegistry->viewCount(view.path()) == 1) {
        syncClangdDocument(view);
        syncLuaDocument(view);
        syncManagedDocument(view);
    }
    if (isManagedSource(view.path()) && !m_managed)
        offerManagedTrust();
    if (isLuaSource(view.path()) && !m_lua)
        offerLuaTrust();
    if (m_clangd && isCFamilySource(view.path()) && currentView() == &view) {
        auto* binding = new ui::lang::EditorInsightBinding(view.editor(), {*m_bridge, *m_pool});
        binding->setCodeVisionEnabled(true);
        binding->attach(*m_clangd);
        refreshMarkers(view);
    } else if (((m_managed && isManagedSource(view.path())) || (m_lua && isLuaSource(view.path()))) &&
               currentView() == &view) {
        auto* binding = new ui::lang::EditorInsightBinding(view.editor(), {*m_bridge, *m_pool});
        binding->setCodeVisionEnabled(true);
        binding->refreshSemanticTokens();
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
    connect(editor, &ui::EditorView::contextActionRequested, this, [this, editor](const QString& actionId) {
        editor->setFocus();
        if (DocumentEditorView* current = currentView(); current != nullptr && &current->editor() == editor) {
            if (actionId == QStringLiteral("code.showIntentions")) {
                m_language->showIntentions();
                return;
            }
            if (actionId == QStringLiteral("navigate.declaration")) {
                m_language->gotoDeclaration();
                return;
            }
            if (actionId == QStringLiteral("navigate.implementation")) {
                m_language->gotoImplementation();
                return;
            }
            if (actionId == QStringLiteral("navigate.findUsages")) {
                m_language->findUsages();
                return;
            }
            if (actionId == QStringLiteral("navigate.relatedFiles")) {
                m_language->gotoRelatedFile();
                return;
            }
            if (actionId == QStringLiteral("refactor.rename")) {
                m_language->rename();
                return;
            }
            if (actionId == QStringLiteral("code.reformat")) {
                m_language->reformat();
                return;
            }
        }
        if (m_window != nullptr) {
            (void)m_window->invoke(actionId.toStdString());
        }
    });
    connect(editor, &ui::EditorView::gutterRunMarkerClicked, this,
            [this, editor](int lineZeroBased, QPoint) {
                DocumentEditorView* targetView = nullptr;
                for (DocumentEditorView* v : m_editors->views()) {
                    if (&v->editor() == editor) {
                        targetView = v;
                        break;
                    }
                }
                if (!targetView)
                    return;
                const auto entries =
                    discoverTestsAndEntryPointsInFile(targetView->path(), targetView->document().document().text().text());
                const int targetLine = lineZeroBased + 1;
                for (const auto& entry : entries) {
                    if (entry.line == targetLine) {
                        if (entry.isEntryPoint) {
                            if (m_window)
                                (void)m_window->invoke("run.run");
                            return;
                        }
                        if (m_window) {
                            m_window->toolWindows().show(kUnitTestsToolWindow, false);
                            if (QWidget* content = m_window->toolWindows().content(kUnitTestsToolWindow)) {
                                if (auto* refreshBtn =
                                        content->findChild<QPushButton*>(QStringLiteral("unitTestsRefreshButton"))) {
                                    refreshBtn->click();
                                }
                                if (auto* filterEdit =
                                        content->findChild<QLineEdit*>(QStringLiteral("unitTestsFilterEdit"))) {
                                    filterEdit->setText(qs(entry.testName));
                                }
                                if (auto* runAllBtn =
                                        content->findChild<QPushButton*>(QStringLiteral("unitTestsRunAllButton"))) {
                                    runAllBtn->click();
                                }
                            }
                        }
                        return;
                    }
                }
            });
    transitionLauncherToMainWindow();
}

void Workbench::editorClosing(DocumentEditorView& view) {
    if (m_debug)
        m_debug->detachEditor(&view.editor());
}

void Workbench::editorClosed(DocumentEditorView& view) {
    const std::string path = view.path();
    if (m_documentRegistry->viewCount(path) > 1)
        return;
    if (m_clangd && isCFamilySource(path))
        (void)m_clangd->closeDocument(path);
    if (m_managed && isManagedSource(path))
        (void)m_managed->closeDocument(path);
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
    if (m_nav && !change.edits.empty()) {
        const ui::EditorViewStatus st = view.editor().status();
        m_nav->recordEditLocation(view.path(), st.line, st.column);
    }
    refreshMarkers(view);
    for (DocumentEditorView* peer : m_editors->views()) {
        if (peer == &view || &peer->document() != &view.document())
            continue;
        if (auto followed = peer->editor().followExternalChange(change, false); !followed) {
            log::warn(kLog, "peer editor synchronization: {}", followed.error().describe());
            peer->reloadFromDocument();
        }
    }
    if (m_git)
        m_git->editorChanged();
    if (m_clangd && m_clangd->state() != lang::ClangdState::Stopped && isCFamilySource(view.path())) {
        (void)m_clangd->changeDocument(view.path(), change, view.document().document().text());
    }
    if (m_managed && isManagedSource(view.path()))
        (void)m_managed->changeDocument(view.path(), change, view.document().document().text());
    if (m_lua && isLuaSource(view.path()))
        (void)m_lua->changeDocument(view.path(), change, view.document().document().text());
    if (DocumentEditorView* current = currentView(); current && &current->document() == &view.document())
        m_language->structureChanged();
    // What the user typed, now in the document and with clangd: completion and parameter info.
    if (!m_typed.empty()) {
        const std::string typed = std::exchange(m_typed, {});
        if (DocumentEditorView* current = currentView(); current == &view) {
            m_language->textTyped(typed);
        }
    }
}

bool Workbench::editorCloseRequested(DocumentEditorView& view) {
    if (std::ranges::contains(m_discarded, view.path()))
        return true;
    if (m_documentRegistry->viewCount(view.path()) > 1)
        return true;
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
    invalidateManagedDebug();
    if (m_git)
        m_git->documentChanged();
    DocumentEditorView* view = m_editors->find(path);
    if (view == nullptr) {
        return;
    }
    bool reloaded = false;
    bool saved = false;
    for (DocumentEditorView* peer : m_editors->views()) {
        if (&peer->document() != &view->document())
            continue;
        switch (peer->followDocument()) {
        case DocumentEditorView::Followed::Reloaded: reloaded = true; break;
        case DocumentEditorView::Followed::Saved: saved = true; break;
        case DocumentEditorView::Followed::Nothing: break;
        }
    }
    if (reloaded) {
        syncClangdDocument(*view);
        syncLuaDocument(*view);
        syncManagedDocument(*view);
        for (DocumentEditorView* peer : m_editors->views()) {
            if (&peer->document() == &view->document()) {
                if (auto* binding = peer->editor().findChild<ui::lang::EditorInsightBinding*>())
                    binding->refreshSemanticTokens();
            }
        }
        // A reload replaced the whole text: the old Structure tree describes text that is gone.
        if (DocumentEditorView* current = currentView(); current && &current->document() == &view->document())
            m_language->structureReplaced();
    } else if (saved) {
        if (m_managed && isManagedSource(view->path()))
            (void)m_managed->saveDocument(view->path());
        if (m_lua && isLuaSource(view->path()))
            (void)m_lua->saveDocument(view->path());
        if (m_clangd && isCFamilySource(view->path()))
            (void)m_clangd->saveDocument(view->path());
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
        notify(text("Saved %1 with a warning").arg(displayName(document.path())), qs(warning.describe()),
               ui::NotificationKind::Warning);
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

Result<void> Workbench::saveAs(DocumentEditorView& view, std::string_view targetPath) {
    if (targetPath.empty()) {
        return fail(ErrorCode::InvalidArgument, "Save As requires a destination path");
    }
    Result<std::string> normalized = io::normalizePath(targetPath);
    if (!normalized) {
        notify(text("Cannot save %1").arg(qs(targetPath)), qs(normalized.error().message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(normalized.error()));
    }
    std::string candidate = m_documentRegistry->canonicalize(*normalized);
    if (!m_services.fs.exists(candidate).value_or(false)) {
        const std::string_view parent = io::parentPath(*normalized);
        if (!parent.empty()) {
            if (Result<std::string> canonicalParent = m_services.fs.canonicalPath(parent); canonicalParent) {
                if (Result<std::string> joined = io::joinPath(*canonicalParent, io::fileName(*normalized)); joined) {
                    candidate = std::move(*joined);
                }
            }
        }
    }

    document::FileDocument& document = view.document();
    const std::string oldPath = document.path();
    if (lang::documentKey(candidate) == lang::documentKey(oldPath)) {
        return save(view);
    }

    Result<document::SaveOutcome> outcome = m_documents->saveAs(document, candidate);
    if (!outcome) {
        m_tabs->refreshBanners(oldPath);
        notify(text("Cannot save %1").arg(displayName(candidate)), qs(outcome.error().message()),
               ui::NotificationKind::Error);
        return std::unexpected(std::move(outcome.error()));
    }

    const std::string newPath = document.path();
    for (const Error& warning : outcome->warnings) {
        log::warn(kLog, "after saving {}: {}", newPath, warning.describe());
        notify(text("Saved %1 with a warning").arg(displayName(newPath)), qs(warning.describe()),
               ui::NotificationKind::Warning);
    }

    if (m_clangd && isCFamilySource(oldPath))
        (void)m_clangd->closeDocument(oldPath);
    if (m_managed && isManagedSource(oldPath))
        (void)m_managed->closeDocument(oldPath);
    if (m_lua && isLuaSource(oldPath))
        (void)m_lua->closeDocument(oldPath);
    m_liveMarkers.erase(lang::documentKey(oldPath));
    std::erase(m_discarded, oldPath);

    for (DocumentEditorView* peer : m_editors->views()) {
        if (&peer->document() != &document)
            continue;
        peer->setPath(newPath);
        delete peer->editor().findChild<ui::EditorChangeMarkers*>();
        if (m_git)
            m_git->attachEditor(*peer);
        delete peer->editor().findChild<ui::lang::EditorInsightBinding*>();
        if (m_clangd && isCFamilySource(newPath) && peer == currentView()) {
            auto* binding = new ui::lang::EditorInsightBinding(peer->editor(), {*m_bridge, *m_pool});
            binding->setCodeVisionEnabled(true);
            binding->attach(*m_clangd);
        }
        refreshMarkers(*peer);
    }
    m_documentRegistry->renamePath(oldPath, newPath);
    m_tabs->renamePath(oldPath, newPath);
    if (m_nav) {
        m_nav->notifyPathRenamed(oldPath, newPath);
    }

    if (DocumentEditorView* updated = m_editors->find(newPath); updated != nullptr) {
        syncClangdDocument(*updated);
        syncLuaDocument(*updated);
        syncManagedDocument(*updated);
    }
    if (isManagedSource(newPath) && !m_managed)
        offerManagedTrust();
    if (isLuaSource(newPath) && !m_lua)
        offerLuaTrust();

    m_documentRegistry->notifyChanged(newPath);
    if (m_language) {
        m_language->editorChanged();
    }
    updateLanguageTarget();
    return {};
}

void Workbench::showSaveAsDialog() {
    DocumentEditorView* view = currentView();
    if (view == nullptr) {
        return;
    }
    const std::string chosen = m_services.prompts.chooseSaveAsPath(m_window, view->path());
    if (chosen.empty()) {
        return;
    }
    (void)saveAs(*view, chosen);
}

void Workbench::rebindRenamedDocument(const std::string& oldPath, const std::string& newPath) {
    document::FileDocument* doc = m_documents->find(oldPath);
    if (doc == nullptr) {
        if (m_nav) {
            m_nav->notifyPathRenamed(oldPath, newPath);
        }
        return;
    }

    if (m_clangd && isCFamilySource(oldPath))
        (void)m_clangd->closeDocument(oldPath);
    if (m_managed && isManagedSource(oldPath))
        (void)m_managed->closeDocument(oldPath);
    if (m_lua && isLuaSource(oldPath))
        (void)m_lua->closeDocument(oldPath);
    m_liveMarkers.erase(lang::documentKey(oldPath));
    std::erase(m_discarded, oldPath);

    (void)m_documents->renameDocument(*doc, newPath);
    m_editors->renamePath(oldPath, newPath);

    for (DocumentEditorView* peer : m_editors->views()) {
        if (&peer->document() != doc)
            continue;
        delete peer->editor().findChild<ui::EditorChangeMarkers*>();
        if (m_git)
            m_git->attachEditor(*peer);
        delete peer->editor().findChild<ui::lang::EditorInsightBinding*>();
        if (m_clangd && isCFamilySource(newPath) && peer == currentView()) {
            auto* binding = new ui::lang::EditorInsightBinding(peer->editor(), {*m_bridge, *m_pool});
            binding->setCodeVisionEnabled(true);
            binding->attach(*m_clangd);
        }
        refreshMarkers(*peer);
    }

    m_documentRegistry->renamePath(oldPath, newPath);
    m_tabs->renamePath(oldPath, newPath);
    if (m_nav) {
        m_nav->notifyPathRenamed(oldPath, newPath);
    }

    if (DocumentEditorView* updated = m_editors->find(newPath); updated != nullptr) {
        syncClangdDocument(*updated);
        syncLuaDocument(*updated);
        syncManagedDocument(*updated);
    }
    if (isManagedSource(newPath) && !m_managed)
        offerManagedTrust();
    if (isLuaSource(newPath) && !m_lua)
        offerLuaTrust();

    m_documentRegistry->notifyChanged(newPath);
    if (m_language) {
        m_language->editorChanged();
    }
    updateLanguageTarget();
}

void Workbench::closeDeletedDocuments(const std::string& deletedPath, bool directory) {
    const std::string targetKey = lang::documentKey(deletedPath);
    const auto matches = [&](std::string_view p) {
        if (lang::documentKey(p) == targetKey)
            return true;
        if (!directory || p.size() <= deletedPath.size())
            return false;
        const std::string pKey = lang::documentKey(p);
        if (!pKey.starts_with(targetKey) || pKey.size() <= targetKey.size())
            return false;
        const char sep = pKey[targetKey.size()];
        return sep == '/' || sep == '\\' || targetKey.back() == '/' || targetKey.back() == '\\';
    };

    std::vector<std::string> affected;
    for (DocumentEditorView* view : m_editors->views()) {
        if (matches(view->path()) && !std::ranges::contains(affected, view->path())) {
            affected.push_back(view->path());
        }
    }
    for (const auto& doc : m_documents->documents()) {
        if (!doc->untitled() && matches(doc->path()) && !std::ranges::contains(affected, doc->path())) {
            affected.push_back(doc->path());
        }
    }

    for (const std::string& p : affected) {
        if (!std::ranges::contains(m_discarded, p)) {
            m_discarded.push_back(p);
        }
        if (m_window) {
            for (ui::EditorTabGroup* group : m_window->tabs().groups()) {
                for (int idx = group->indexOf(p); idx >= 0; idx = group->indexOf(p)) {
                    if (!group->close(idx))
                        break;
                }
            }
        }
        while (m_documentRegistry->viewCount(p) > 0) {
            m_documentRegistry->release(p);
        }
        if (document::FileDocument* doc = m_documents->find(p); doc != nullptr) {
            (void)m_documents->close(*doc);
        }
        std::erase(m_discarded, p);
    }

    if (m_nav) {
        m_nav->notifyPathDeleted(deletedPath, directory);
    }
    if (m_language) {
        m_language->editorChanged();
    }
    updateLanguageTarget();
}

document::SaveAllResult Workbench::saveAllDocuments() {
    if (m_nav) {
        return m_nav->saveAll();
    }
    document::SaveAllResult result = m_documents->saveAll(document::SaveReason::SaveAll);
    for (document::FileDocument* document : result.saved) {
        m_documentRegistry->notifyChanged(document->path());
    }
    for (const document::FailedSave& warning : result.warnings) {
        notify(text("Saved %1 with a warning").arg(displayName(warning.document->path())), qs(warning.error.describe()),
               ui::NotificationKind::Warning);
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
    if (!view.path().empty() && !view.editor().fileInfo().largeFile) {
        const auto runnable = discoverTestsAndEntryPointsInFile(view.path(), view.document().document().text().text());
        for (const auto& entry : runnable) {
            if (entry.line <= 0)
                continue;
            const std::string msg =
                entry.isEntryPoint
                    ? std::format("Run '{}' ({})", entry.testName, entry.suiteName)
                    : std::format("Run Test '{}.{}' / Debug Test", entry.suiteName, entry.testName);
            markers.push_back(ui::LineMarker{LineIndex{static_cast<std::uint32_t>(entry.line - 1)},
                                             ui::MarkerSeverity::RunTest, msg});
        }
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
