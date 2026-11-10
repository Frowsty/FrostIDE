// SPDX-License-Identifier: Apache-2.0
//
// Workbench session persistence, hot-exit recovery, external disk changes, merge view management,
// and document saving.

#include "frost/core/log.hpp"
#include "frost/ui/shell/editor_tabs.hpp"
#include "frost/ui/shell/notifications.hpp"
#include "frost/ui/shell/tool_window_manager.hpp"
#include "frost/ui/workbench/merge_view.hpp"

#include "workbench_internal.hpp"
#include <QListWidget>

#include <set>

namespace frost::ui {

using workbench_detail::fromUtf8;
using workbench_detail::nativePath;
using workbench_detail::text;

// --- Sessions ---

nav::SessionState Workbench::captureSession() const {
    nav::SessionState state;
    state.root = m_root;
    state.cmakeTargets = m_cmakeTargets;
    state.cmakeArguments = m_cmakeArguments;
    const EditorTabs& tabs = m_shell.tabs();
    state.editorLayout = tabs.saveLayout();
    const auto groups = tabs.groups();
    for (std::size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        const EditorTabGroup* group = groups[groupIndex];
        if (group == tabs.activeGroup())
            state.activeGroup = static_cast<int>(groupIndex);
        for (int i = 0; i < group->count(); ++i) {
            const IEditorView* editor = group->editor(i);
            const EditorViewState view = editor->viewState();
            if (group == tabs.activeGroup() && i == group->currentIndex())
                state.active = static_cast<int>(state.tabs.size());
            state.tabs.push_back(nav::SessionTab{editor->path(), group->isPinned(i), view.line, view.column,
                                                 view.topLine, view.foldTextHash, view.folds,
                                                 static_cast<int>(groupIndex), i == group->currentIndex()});
        }
    }
    ToolWindowManager& windows = m_shell.toolWindows();
    windows.syncSizesFromSplitters();
    for (const std::string& id : windows.ids()) {
        state.toolWindows.push_back(nav::SessionToolWindow{id, windows.isVisible(id), windows.size(id)});
    }
    state.recentFiles.assign(m_recentFiles.paths().begin(), m_recentFiles.paths().end());
    state.recentLocations.assign(m_recentLocations.places().begin(), m_recentLocations.places().end());
    return state;
}

void Workbench::applySession(const nav::SessionState& state) {
    m_restoring = true;
    m_cmakeTargets = state.cmakeTargets;
    m_cmakeArguments = state.cmakeArguments;
    EditorTabs& tabs = m_shell.tabs();
    if (!tabs.restoreLayout(state.editorLayout))
        (void)tabs.restoreLayout(R"({"group":0})");
    const auto groups = tabs.groups();
    std::vector<std::string> selected(groups.size());
    int activeGroup =
        state.activeGroup >= 0 && static_cast<std::size_t>(state.activeGroup) < groups.size() ? state.activeGroup : 0;
    std::string activePath;
    for (std::size_t i = 0; i < state.tabs.size(); ++i) {
        const nav::SessionTab& tab = state.tabs[i];
        const int groupIndex = tab.group >= 0 && static_cast<std::size_t>(tab.group) < groups.size() ? tab.group : 0;
        tabs.setActiveGroup(groups[static_cast<std::size_t>(groupIndex)]);
        Result<int> opened = m_shell.openFile(tab.path);
        if (!opened)
            continue;
        IEditorView* editor = tabs.editor(*opened);
        editor->restoreViewState(EditorViewState{tab.line, tab.column, tab.top, tab.foldTextHash, tab.folds});
        m_lastPlaces[editor->path()] = nav::Place{editor->path(), tab.line, tab.column};
        if (tab.pinned)
            tabs.setPinned(tabs.indexOf(editor->path()), true);
        if (tab.groupActive)
            selected[static_cast<std::size_t>(groupIndex)] = editor->path();
        if (static_cast<int>(i) == state.active) {
            activeGroup = groupIndex;
            activePath = editor->path();
        }
    }
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (!selected[i].empty()) {
            tabs.setActiveGroup(groups[i]);
            tabs.activate(tabs.indexOf(selected[i]));
        }
    }
    tabs.setActiveGroup(groups[static_cast<std::size_t>(activeGroup)]);
    if (!activePath.empty()) {
        tabs.activate(tabs.indexOf(activePath));
        m_activePath = activePath;
    }
    ToolWindowManager& windows = m_shell.toolWindows();
    for (const nav::SessionToolWindow& window : state.toolWindows) {
        if (!windows.contains(window.id)) {
            continue;
        }
        if (window.size > 0) {
            windows.setSize(window.id, window.size);
        }
        if (window.visible) {
            windows.show(window.id, false);
        } else {
            windows.hide(window.id);
        }
    }
    m_recentFiles.assign(state.recentFiles);
    m_recentLocations.assign(state.recentLocations);
    m_restoring = false;
}

Result<void> Workbench::saveSession() {
    if (m_services.sessions == nullptr || m_root.empty()) {
        return {};
    }
    return m_services.sessions->save(captureSession());
}

bool Workbench::restoreLastProject() {
    if (m_services.sessions == nullptr) {
        return false;
    }
    const std::optional<std::string> last = m_services.sessions->lastProject();
    if (!last) {
        return false;
    }
    if (auto exists = m_services.fs.exists(*last); !exists || !*exists) {
        return false;
    }
    if (auto opened = m_services.openProjectHook ? m_services.openProjectHook(*last) : openProject(*last); !opened) {
        notifyError(text("Cannot reopen %1").arg(nativePath(*last)), opened.error());
        return false;
    }
    return true;
}

// --- Hot exit ---

std::size_t Workbench::offerRestore(storage::RecoveryScan scan) {
    std::set<std::string> keys;
    bool crashed = false;
    for (storage::RecoveredJournal& journal : scan.journals) {
        crashed = crashed || !journal.endedCleanly();
        if (auto entries = journal.entries(); entries) {
            for (const storage::JournalEntry& entry : *entries) {
                keys.insert(entry.state.documentKey);
            }
        } else {
            keys.insert(journal.path()); // Unreadable here; restore() reports it.
        }
    }
    if (keys.empty()) {
        for (storage::RecoveredJournal& journal : scan.journals) {
            static_cast<void>(std::move(journal).discard());
        }
        return 0;
    }
    m_recovery.emplace(std::move(scan));
    const auto count = static_cast<int>(keys.size());
    Notification notification;
    notification.title = crashed ? text("FrostIDE closed unexpectedly. Restore %1 unsaved files?").arg(count)
                                 : text("Restore %1 unsaved files from the last session?").arg(count);
    notification.text = text("Their unsaved changes were kept in the hot-exit journal.");
    notification.kind = NotificationKind::Warning;
    notification.sticky = true;
    const QPointer<Workbench> self(this);
    notification.actions.push_back(NotificationAction{text("Restore"), [self] {
                                                          if (self != nullptr) {
                                                              static_cast<void>(self->restoreRecovered());
                                                          }
                                                      }});
    notification.actions.push_back(NotificationAction{text("Discard"), [self] {
                                                          if (self != nullptr) {
                                                              self->discardRecovered();
                                                          }
                                                      }});
    m_recoveryNotification = m_shell.notifications().notify(std::move(notification));
    return keys.size();
}

document::RestoreReport Workbench::restoreRecovered() {
    if (!m_recovery) {
        return {};
    }
    document::RestoreReport report = m_services.saveManager.restore(*m_recovery);
    m_recovery.reset();
    int untitled = 0;
    for (document::FileDocument* document : report.restored) {
        if (document->untitled()) {
            ++untitled; // TODO(M2): untitled buffers get their own tabs with the real editor.
            continue;
        }
        static_cast<void>(openFile(document->path()));
    }
    QString summary = text("%1 files were restored.").arg(report.restored.size());
    if (!report.diskChanged.empty()) {
        summary += QStringLiteral(" ") +
                   text("%1 changed on disk since; compare before saving.").arg(report.diskChanged.size());
    }
    if (!report.deferred.empty()) {
        summary += QStringLiteral(" ") + text("%1 could not be reopened and were kept.").arg(report.deferred.size());
    }
    if (untitled > 0) {
        summary += QStringLiteral(" ") + text("%1 untitled buffers were restored without a tab.").arg(untitled);
    }
    m_shell.notifications().notify({text("Unsaved files restored"),
                                    summary,
                                    report.errors.empty() ? NotificationKind::Info : NotificationKind::Warning,
                                    std::nullopt,
                                    {}});
    return report;
}

void Workbench::discardRecovered() {
    if (!m_recovery) {
        return;
    }
    for (storage::RecoveredJournal& journal : m_recovery->journals) {
        if (auto discarded = std::move(journal).discard(); !discarded) {
            log::warn("ui", "cannot discard a hot-exit journal: {}", discarded.error().message());
        }
    }
    m_recovery.reset();
}

// --- Disk changes, merging and saving ---

void Workbench::applyFileChanges(const io::FileChangeBatch& batch) {
    if (m_services.filesChanged) {
        m_services.filesChanged(batch);
    }
    // The listener (handleExternalChange) reports each change the manager finds.
    static_cast<void>(m_services.saveManager.applyChanges(batch));
    // The CMake state the directory roles read (rules 3 and 5): rebuild them and the watch.
    const bool rolesChanged =
        !m_root.empty() && std::ranges::any_of(batch.changes, [this](const io::FileChange& change) {
            const std::string_view name = nav::baseName(change.path);
            return name == "CMakeCache.txt" || ((name == "CMakePresets.json" || name == "CMakeUserPresets.json") &&
                                                nav::parentOf(change.path) == m_root);
        });
    if (rolesChanged) {
        loadDirectoryRoles();
        if (m_watch) {
            stopWatching();
            startWatching();
        }
    }
    bool projectChanged = false;
    for (const io::FileChange& change : batch.changes) {
        if (isUnder(m_root, change.path) && !nav::relativeTo(m_root, change.path).starts_with(".git/")) {
            projectChanged = true;
            break;
        }
    }
    if (projectChanged) {
        refresh();
    }
}

void Workbench::handleExternalChange(const document::ExternalChange& change) {
    if (change.document == nullptr) {
        return;
    }
    const std::string path = change.document->path();
    m_services.editors.refreshBanners(path);
    m_services.documents.notifyChanged(path);
    const QString name = nativePath(path);
    switch (change.kind) {
    case document::ExternalChangeKind::MergePending: {
        Notification notification;
        notification.title = text("%1 changed on disk").arg(name);
        notification.text = text("It has unsaved changes here and was changed by another program. Keep your "
                                 "version, take the one on disk, or compare them to merge.");
        notification.kind = NotificationKind::Warning;
        notification.sticky = true;
        // The document may be closed before the user answers: each action looks it up again.
        const QPointer<Workbench> self(this);
        notification.actions.push_back(NotificationAction{text("Keep my version"), [self, path] {
                                                              if (self != nullptr) {
                                                                  self->resolveExternalChange(
                                                                      path, document::MergeDecision::KeepBuffer);
                                                              }
                                                          }});
        notification.actions.push_back(NotificationAction{text("Take disk version"), [self, path] {
                                                              if (self != nullptr) {
                                                                  self->resolveExternalChange(
                                                                      path, document::MergeDecision::TakeDisk);
                                                              }
                                                          }});
        notification.actions.push_back(NotificationAction{text("Compare..."), [self, path] {
                                                              if (self != nullptr) {
                                                                  static_cast<void>(self->showMerge(path));
                                                              }
                                                          }});
        m_shell.notifications().notify(std::move(notification));
        break;
    }
    case document::ExternalChangeKind::OutOfDate: {
        const bool tooLarge = change.error && change.error->code() == ErrorCode::OutOfRange;
        m_shell.notifications().notify(
            {tooLarge ? text("%1 is too large to reload").arg(name) : text("Cannot reload %1").arg(name),
             change.error ? fromUtf8(change.error->message()) : QString(),
             NotificationKind::Warning,
             std::nullopt,
             {}});
        break;
    }
    case document::ExternalChangeKind::Deleted:
        m_shell.notifications().notify({text("%1 was deleted on disk").arg(name),
                                        text("Saving creates it again."),
                                        NotificationKind::Info,
                                        std::nullopt,
                                        {}});
        break;
    case document::ExternalChangeKind::Failed:
        if (change.error) {
            notifyError(text("Cannot check %1 on disk").arg(name), *change.error);
        }
        break;
    case document::ExternalChangeKind::None:
    case document::ExternalChangeKind::Touched:
    case document::ExternalChangeKind::Reloaded: break;
    }
}

MergeView* Workbench::showMerge(const std::string& path) {
    if (const auto it = m_merges.find(path); it != m_merges.end() && it->second != nullptr) {
        it->second->raise();
        it->second->activateWindow();
        return it->second;
    }
    document::FileDocument* document = m_services.documents.find(path);
    if (document == nullptr || document->pendingMerge() == nullptr) {
        m_shell.notifications().notify({text("Nothing to compare"),
                                        text("%1 has no pending change from disk.").arg(nativePath(path)),
                                        NotificationKind::Info,
                                        std::nullopt,
                                        {}});
        return nullptr;
    }
    auto created = MergeView::create(m_services.saveManager, *document, &m_shell);
    if (!created) {
        notifyError(text("Cannot compare %1").arg(nativePath(path)), created.error());
        return nullptr;
    }
    MergeView* view = *created;
    m_merges[document->path()] = view;
    const std::string key = document->path();
    connect(view, &MergeView::resolved, this, [this, key](const QString&, int) {
        m_services.editors.refreshBanners(key);
        m_services.documents.notifyChanged(key);
    });
    connect(view, &QObject::destroyed, this, [this, key] { m_merges.erase(key); });
    view->show();
    return view;
}

void Workbench::resolveExternalChange(const std::string& path, document::MergeDecision decision) {
    document::FileDocument* document = m_services.documents.find(path);
    if (document == nullptr || document->pendingMerge() == nullptr) {
        return; // Closed, or decided elsewhere (the merge view, another notification).
    }
    if (auto resolved = m_services.saveManager.resolveExternalChange(*document, decision); !resolved) {
        notifyError(text("Cannot resolve the change of %1").arg(nativePath(path)), resolved.error());
    }
    if (const auto it = m_merges.find(document->path()); it != m_merges.end() && it->second != nullptr) {
        it->second->close();
    }
    m_services.editors.refreshBanners(document->path());
    m_services.documents.notifyChanged(document->path());
}

void Workbench::reloadAllFromDisk() {
    // The paths first: the listeners of each change run before the next check.
    std::vector<std::string> paths;
    for (const auto& document : m_services.saveManager.documents()) {
        if (!document->untitled()) {
            paths.push_back(document->path());
        }
    }
    for (const std::string& path : paths) {
        if (document::FileDocument* document = m_services.saveManager.find(path); document != nullptr) {
            handleExternalChange(m_services.saveManager.checkDisk(*document));
        }
    }
}

void Workbench::frameDeactivated() {
    if (m_services.autosave == nullptr) {
        return;
    }
    if (auto saved = m_services.autosave->frameDeactivated(); saved) {
        reportSaveAll(*saved, true);
    }
}

void Workbench::frameActivated() { reloadAllFromDisk(); }

void Workbench::reloadFromDisk(const std::string& path) {
    document::FileDocument* document = m_services.documents.find(path);
    if (document == nullptr) {
        return;
    }
    handleExternalChange(m_services.saveManager.checkDisk(*document));
    m_services.editors.refreshBanners(path);
}

void Workbench::overwriteDisk(const std::string& path) {
    document::FileDocument* document = m_services.documents.find(path);
    if (document == nullptr) {
        return;
    }
    document::SaveOptions options;
    options.overwriteExternalChanges = true;
    if (auto saved = m_services.saveManager.save(*document, options); !saved) {
        notifyError(text("Cannot save %1").arg(nativePath(path)), saved.error());
    } else {
        for (const Error& warning : saved->warnings)
            notifySaveWarning(path, warning);
    }
    m_services.editors.refreshBanners(path);
    m_services.documents.notifyChanged(path);
}

void Workbench::reportSaveAll(const document::SaveAllResult& result, bool quiet) {
    for (document::FileDocument* document : result.saved) {
        m_services.editors.refreshBanners(document->path());
        m_services.documents.notifyChanged(document->path());
    }
    if (!result.saved.empty() && m_todoList != nullptr &&
        (m_shell.toolWindows().isVisible(kTodoToolWindow) || m_todoList->isVisible())) {
        refreshTodoItems();
    }
    for (const document::FailedSave& failed : result.failed) {
        notifyError(text("Cannot save %1").arg(nativePath(failed.document->path())), failed.error);
    }
    for (const document::FailedSave& warning : result.warnings) {
        notifySaveWarning(warning.document->path(), warning.error);
    }
    if (quiet) {
        return;
    }
    for (const document::SkippedDocument& skipped : result.skipped) {
        const std::string path = skipped.document->path();
        const QString name = skipped.document->untitled() ? text("An untitled buffer") : nativePath(path);
        switch (skipped.reason) {
        case document::SkipReason::MergePending: {
            Notification notification;
            notification.title = text("%1 was not saved").arg(name);
            notification.text = text("It changed on disk; compare the versions first.");
            notification.kind = NotificationKind::Warning;
            const QPointer<Workbench> self(this);
            notification.actions.push_back(NotificationAction{text("Compare..."), [self, path] {
                                                                  if (self != nullptr) {
                                                                      static_cast<void>(self->showMerge(path));
                                                                  }
                                                              }});
            m_shell.notifications().notify(std::move(notification));
            break;
        }
        case document::SkipReason::OutOfDate:
            m_shell.notifications().notify(
                {text("%1 was not saved").arg(name),
                 text("The file on disk changed; use Overwrite in the editor to save over it."),
                 NotificationKind::Warning,
                 std::nullopt,
                 {}});
            break;
        case document::SkipReason::NeedsConsent:
            m_shell.notifications().notify(
                {text("%1 was not saved").arg(name),
                 text("It was loaded with replacement characters; save it explicitly to accept them."),
                 NotificationKind::Warning,
                 std::nullopt,
                 {}});
            break;
        case document::SkipReason::Untitled:
        case document::SkipReason::Deleted: break;
        }
    }
}

document::SaveAllResult Workbench::saveAll() {
    document::SaveAllResult result = m_services.saveManager.saveAll(document::SaveReason::SaveAll);
    reportSaveAll(result, false);
    return result;
}

Result<void> Workbench::saveDocument() {
    IEditorView* editor = m_shell.tabs().currentEditor();
    if (editor == nullptr) {
        return fail(ErrorCode::NotFound, "no document is active");
    }
    document::FileDocument* document = m_services.documents.find(editor->path());
    if (document == nullptr) {
        return fail(ErrorCode::NotFound, "'" + editor->path() + "' is not open as a document");
    }
    auto saved = m_services.saveManager.save(*document);
    m_services.editors.refreshBanners(document->path());
    m_services.documents.notifyChanged(document->path());
    if (m_todoList != nullptr && (m_shell.toolWindows().isVisible(kTodoToolWindow) || m_todoList->isVisible())) {
        refreshTodoItems();
    }
    if (!saved) {
        notifyError(text("Cannot save %1").arg(nativePath(document->path())), saved.error());
        return std::unexpected(std::move(saved.error()));
    }
    for (const Error& warning : saved->warnings)
        notifySaveWarning(document->path(), warning);
    return {};
}

Result<void> Workbench::saveDocumentAs(std::string_view targetPath) {
    IEditorView* editor = m_shell.tabs().currentEditor();
    if (editor == nullptr) {
        return fail(ErrorCode::NotFound, "no document is active");
    }
    const std::string oldPath = editor->path();
    document::FileDocument* document = m_services.documents.find(oldPath);
    if (document == nullptr) {
        return fail(ErrorCode::NotFound, "'" + oldPath + "' is not open as a document");
    }
    if (targetPath.empty()) {
        return fail(ErrorCode::Cancelled, "Save As was cancelled");
    }
    const io::PathStyle style = nav::styleOf(oldPath.empty() ? targetPath : std::string_view(oldPath));
    auto normalized = io::normalizePath(targetPath, style);
    if (!normalized) {
        notifyError(text("Cannot save %1").arg(fromUtf8(targetPath)), normalized.error());
        return std::unexpected(std::move(normalized.error()));
    }
    std::string candidate = *normalized;
    if (auto existingCanonical = m_services.fs.canonicalPath(candidate); existingCanonical) {
        candidate = std::move(*existingCanonical);
    } else {
        const std::string_view parent = io::parentPath(candidate, style);
        const std::string_view name = io::fileName(candidate, style);
        if (!parent.empty() && !name.empty()) {
            const std::string canonicalParent = m_services.documents.canonicalize(parent);
            if (auto rejoined = io::joinPath(canonicalParent, name, style); rejoined) {
                candidate = std::move(*rejoined);
            }
        }
    }
    if (candidate == oldPath) {
        return saveDocument();
    }
    auto saved = m_services.saveManager.saveAs(*document, candidate);
    if (!saved) {
        m_services.editors.refreshBanners(oldPath);
        notifyError(text("Cannot save %1").arg(nativePath(candidate)), saved.error());
        return std::unexpected(std::move(saved.error()));
    }
    const std::string newPath = document->path();
    m_services.documents.renamePath(oldPath, newPath);
    m_services.editors.renamePath(oldPath, newPath);
    notifyPathRenamed(oldPath, newPath);
    m_services.documents.notifyChanged(newPath);
    for (const Error& warning : saved->warnings) {
        notifySaveWarning(newPath, warning);
    }
    return {};
}

} // namespace frost::ui
