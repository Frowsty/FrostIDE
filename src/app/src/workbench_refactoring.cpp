// SPDX-License-Identifier: Apache-2.0
//
// REF-1: one composition-root transaction owner, global Undo/Redo, and settled view publication.
#include "frost/app/workbench.hpp"
#include "frost/core/log.hpp"
#include "frost/io/file_transaction_replace.hpp"
#include "frost/io/path.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/lang/language_controller.hpp"

#include <QScopeGuard>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

namespace frost::app {
namespace {
QString display(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }
} // namespace

Result<document::RefactoringOutcome> Workbench::applyRefactoring(const document::RefactoringPlan& plan) {
    if (m_shutDown) {
        return fail(ErrorCode::Cancelled, "The workbench is shutting down");
    }
    // A refactoring can edit files only in trusted projects. Producer snapshots/dependencies are
    // rechecked by the coordinator; trust is rechecked here at the mutation boundary.
    const auto trusted = [this](std::string_view path) { return m_services.trust.isTrusted(path); };
    for (const auto& edit : plan.disk)
        if (!trusted(edit.path))
            return fail(ErrorCode::Untrusted, "Trust the project before refactoring: " + edit.path);
    for (const auto& edit : plan.buffers)
        if (!trusted(edit.path))
            return fail(ErrorCode::Untrusted, "Trust the project before refactoring: " + edit.path);
    for (const auto& move : plan.renames)
        if (!trusted(move.from) || !trusted(move.to))
            return fail(ErrorCode::Untrusted, "Trust both paths before renaming: " + move.from);
    for (const auto& creation : plan.creates)
        if (!trusted(creation.path))
            return fail(ErrorCode::Untrusted, "Trust the project before creating: " + creation.path);
    std::vector<std::string> mutationPaths;
    for (const auto& edit : plan.disk)
        mutationPaths.push_back(edit.path);
    for (const auto& edit : plan.buffers)
        mutationPaths.push_back(edit.path);
    for (const auto& move : plan.renames) {
        mutationPaths.push_back(move.from);
        mutationPaths.push_back(move.to);
    }
    for (const auto& creation : plan.creates)
        mutationPaths.push_back(creation.path);
    if (!m_refactoring) {
        std::string target;
        if (!plan.disk.empty())
            target = plan.disk.front().path;
        else if (!plan.buffers.empty())
            target = plan.buffers.front().path;
        else if (!plan.renames.empty())
            target = plan.renames.front().from;
        else if (!plan.creates.empty())
            target = plan.creates.front().path;
        else
            return fail(ErrorCode::InvalidArgument, "The refactoring contains no changes");
        std::string preferred = m_options.refactoringStateDirectory;
        if (preferred.empty()) {
            const auto data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
            if (data.isEmpty())
                return fail(ErrorCode::NotFound, "No refactoring recovery directory is available");
            preferred = data.toStdString() + "/refactoring";
        }
        auto recovery = io::ensurePrivateTransactionStore(target, preferred, m_projectRoot);
        if (!recovery)
            return std::unexpected(std::move(recovery.error()));
        document::RefactoringOptions options;
        options.admit = [this]() -> Result<void> {
            if (m_shutDown)
                return fail(ErrorCode::Cancelled, "The workbench is shutting down");
            if (m_refactoringAdmissionToken.assigned() && !refactoringTrusted(m_refactoringAdmissionToken))
                return fail(ErrorCode::Untrusted, "Project trust changed while preparing refactoring history");
            return {};
        };
        options.recoveryDirectory = *recovery;
        options.privateStoreForPath = [this, preferred](std::string_view path) {
            return io::ensurePrivateTransactionStore(path, preferred, m_projectRoot);
        };
        options.renamed = [this](document::FileDocument& doc, std::string_view oldPath) {
            rebindRenamedDocument(std::string(oldPath), doc.path());
        };
        options.changed = [this](document::FileDocument& doc, const text::ChangeSet& change) {
            for (auto* view : m_editors->views()) {
                if (&view->document() != &doc)
                    continue;
                if (auto followed = view->editor().followExternalChange(change); !followed) {
                    view->editor().reloadFromDocument();
                    syncClangdDocument(*view);
                    syncLuaDocument(*view);
                    syncManagedDocument(*view);
                    log::warn("refactoring", "view resynchronized after refactoring: {}", followed.error().describe());
                }
            }
            m_documentRegistry->notifyChanged(doc.path());
        };
        m_refactoring =
            std::make_unique<document::RefactoringCoordinator>(*m_documents, m_services.fs, std::move(options));
    }
    if (!m_refactoringRecoveryChecked) {
        auto recovered = m_refactoring->recoverPending();
        if (!recovered)
            return std::unexpected(std::move(recovered.error()));
        m_refactoringRecoveryChecked = true;
        for (const auto& outcome : *recovered)
            publishRefactoringOutcome(outcome);
        m_pendingRefactoring = m_refactoring->pendingToken();
    }
    if (m_refactoring->pending())
        return fail(ErrorCode::Busy, "Finish the pending refactoring with Resume Refactoring before starting another");
    auto admittedPlan = plan;
    admittedPlan.revalidateContext = [this, mutationPaths, context = plan.revalidateContext]() -> Result<void> {
        if (m_shutDown)
            return fail(ErrorCode::Cancelled, "The workbench is shutting down");
        for (const auto& path : mutationPaths)
            if (!m_services.trust.isTrusted(path))
                return fail(ErrorCode::Untrusted, "Project trust changed while preparing refactoring: " + path);
        return context ? context() : Result<void>{};
    };
    auto outcome = m_refactoring->apply(admittedPlan);
    if (!outcome)
        return std::unexpected(std::move(outcome.error()));
    m_refactoringPaths.push_back({outcome->token, plan.renames, std::move(mutationPaths), false});
    publishRefactoringOutcome(*outcome);
    return outcome;
}

void Workbench::publishRefactoringOutcome(const document::RefactoringOutcome& outcome) {
    if (outcome.status == document::RefactoringStatus::PendingRecovery) {
        m_pendingRefactoring = outcome.token;
        notify(QStringLiteral("Refactoring needs recovery"),
               QStringLiteral(
                   "The operation was committed but has not finished. Use Resume Refactoring; do not repeat it."),
               ui::NotificationKind::Warning);
        return;
    }
    m_pendingRefactoring = {};
    const bool applied = outcome.status != document::RefactoringStatus::Undone;
    for (auto& paths : m_refactoringPaths) {
        if (paths.token != outcome.token || paths.applied == applied)
            continue;
        for (const auto& move : paths.renames) {
            const auto& from = applied ? move.from : move.to;
            const auto& to = applied ? move.to : move.from;
            // Open documents were rebound by the settled SaveManager notification above.
            if (m_editors->find(to) && !m_editors->find(from))
                continue;
            rebindRenamedDocument(from, to);
        }
        paths.applied = applied;
    }
    refreshSolutionExplorerForFsChange();
    if (m_language)
        m_language->editorChanged();
    for (const auto& warning : outcome.warnings) {
        notify(QStringLiteral("Refactoring completed with a warning"), display(warning.message()),
               ui::NotificationKind::Warning);
    }
    emit refactoringSettled(outcome);
}

bool Workbench::refactoringTrusted(TransactionId token) const {
    for (const auto& entry : m_refactoringPaths) {
        if (entry.token == token)
            return std::ranges::all_of(entry.mutationPaths,
                                       [this](const auto& path) { return m_services.trust.isTrusted(path); });
    }
    return false;
}
bool Workbench::canUndoRefactoring() const noexcept {
    return !m_shutDown && m_refactoring && !m_refactoring->pending() && m_refactoring->latestUndo().assigned() &&
           refactoringTrusted(m_refactoring->latestUndo());
}
bool Workbench::canRedoRefactoring() const noexcept {
    return !m_shutDown && m_refactoring && !m_refactoring->pending() && m_refactoring->latestRedo().assigned() &&
           refactoringTrusted(m_refactoring->latestRedo());
}
bool Workbench::refactoringPending() const noexcept { return !m_shutDown && m_refactoring && m_refactoring->pending(); }

Result<document::RefactoringOutcome> Workbench::undoRefactoring() {
    if (!canUndoRefactoring())
        return fail(ErrorCode::Busy, "No complete refactoring can be undone");
    return runRefactoringHistory(m_refactoring->latestUndo(), false);
}
Result<document::RefactoringOutcome> Workbench::redoRefactoring() {
    if (!canRedoRefactoring())
        return fail(ErrorCode::Busy, "No refactoring can be redone");
    return runRefactoringHistory(m_refactoring->latestRedo(), true);
}
Result<document::RefactoringOutcome> Workbench::runRefactoringHistory(TransactionId token, bool redo) {
    if (m_shutDown || !m_refactoring)
        return fail(ErrorCode::Cancelled, "The refactoring owner is unavailable");
    if (!refactoringTrusted(token))
        return fail(ErrorCode::Untrusted, "Trust the project before changing refactoring history");
    const auto previous = m_refactoringAdmissionToken;
    m_refactoringAdmissionToken = token;
    const auto clear = qScopeGuard([&] { m_refactoringAdmissionToken = previous; });
    auto outcome = redo ? m_refactoring->redo(token) : m_refactoring->undo(token);
    if (outcome)
        publishRefactoringOutcome(*outcome);
    return outcome;
}
Result<document::RefactoringOutcome> Workbench::resumeRefactoring() {
    if (!refactoringPending() || !m_pendingRefactoring.assigned())
        return fail(ErrorCode::Busy, "No refactoring is waiting for recovery");
    auto outcome = m_refactoring->resume(m_pendingRefactoring);
    if (outcome)
        publishRefactoringOutcome(*outcome);
    return outcome;
}

void Workbench::registerRefactoringActions() {
    registerCppRefactoringActions();
    registerLuaRefactoringActions();
    for (const bool redo : {false, true}) {
        input::ActionSpec spec;
        spec.id = redo ? "editor.redo" : "editor.undo";
        spec.name = redo ? "Redo" : "Undo";
        spec.group = "Editor";
        spec.context.all = input::ContextKeys{input::ContextKey::EditorFocus};
        spec.context.none = input::ContextKeys{input::ContextKey::EditorReadOnly};
        spec.handler = [this, redo](const input::ActionContext&, const input::InvokeInfo&) {
            auto* view = m_actionHost.active();
            if (!view)
                return;
            const auto state = view->document().undoState();
            const auto token = redo ? state.externalRedo : state.externalUndo;
            if (!token.assigned()) {
                (void)view->perform(redo ? ui::EditorAction::Redo : ui::EditorAction::Undo);
                return;
            }
            if (!m_refactoring || m_shutDown || !refactoringTrusted(token)) {
                notify(QStringLiteral("Refactoring history unavailable"),
                       QStringLiteral("The refactoring coordinator or project trust is no longer available."),
                       ui::NotificationKind::Warning);
                return;
            }
            auto outcome = runRefactoringHistory(token, redo);
            if (!outcome)
                notify(QStringLiteral("Refactoring could not continue"), display(outcome.error().message()),
                       ui::NotificationKind::Error);
        };
        if (auto added = m_registry.add(std::move(spec)); !added)
            log::warn("refactoring", "history action registration: {}", added.error().describe());
    }
    const auto add = [this](std::string id, std::string name, Result<document::RefactoringOutcome> (Workbench::*run)(),
                            bool (Workbench::*enabled)() const noexcept) {
        input::ActionSpec spec;
        spec.id = std::move(id);
        spec.name = std::move(name);
        spec.group = "Refactor";
        spec.context.none = input::ContextKeys{input::ContextKey::DialogFocus};
        spec.enabled = [this, enabled](const input::ActionContext&) { return (this->*enabled)(); };
        spec.handler = [this, run](const input::ActionContext&, const input::InvokeInfo&) {
            if (auto result = (this->*run)(); !result)
                notify(QStringLiteral("Refactoring could not continue"), display(result.error().message()),
                       ui::NotificationKind::Error);
        };
        if (auto added = m_registry.add(std::move(spec)); !added)
            log::warn("refactoring", "action registration: {}", added.error().describe());
    };
    add("refactor.undo", "Undo Refactoring", &Workbench::undoRefactoring, &Workbench::canUndoRefactoring);
    add("refactor.redo", "Redo Refactoring", &Workbench::redoRefactoring, &Workbench::canRedoRefactoring);
    add("refactor.resume", "Resume Refactoring", &Workbench::resumeRefactoring, &Workbench::refactoringPending);
}
} // namespace frost::app
