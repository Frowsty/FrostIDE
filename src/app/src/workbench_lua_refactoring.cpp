// SPDX-License-Identifier: Apache-2.0
// Lua AST proposals use the same REF1 preview, document leases and global history as C++.
#include "frost/app/workbench.hpp"
#include "frost/lang/lua_refactoring.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/trust_store.hpp"
#include "frost/sched/lane.hpp"
#include "frost/syntax/syntax_session.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
namespace frost::app {
namespace {
sched::Task<void> prepareLuaAst(syntax::SyntaxTree tree, std::string operation, text::ByteRange range, std::string name,
                                std::stop_token stop, std::function<void(Result<lang::LuaRefactoringProposal>)> done) {
    if (operation == "inline")
        done(lang::planLuaInlineVariable(tree, range.begin, stop));
    else
        done(lang::planLuaExtractFunction(tree, range, std::move(name), stop));
    co_return;
}
QString displayLua(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}
} // namespace
void Workbench::registerLuaRefactoringActions() {
    input::ActionSpec rename;
    rename.id = "refactor.lua.renameModule";
    rename.name = "Rename Lua Module and Require Users…";
    rename.group = "Refactor";
    rename.context.all = input::ContextKeys{input::ContextKey::EditorFocus};
    rename.context.none = input::ContextKeys{input::ContextKey::EditorReadOnly, input::ContextKey::DialogFocus};
    rename.handler = [this](const input::ActionContext&, const input::InvokeInfo&) { renameLuaModule(); };
    (void)m_registry.add(std::move(rename));
    for (const auto& [operation, label] :
         {std::pair{"inline", "Inline Lua Local…"}, std::pair{"extract", "Extract Lua Function…"}}) {
        input::ActionSpec action;
        action.id = "refactor.lua." + std::string(operation);
        action.name = label;
        action.group = "Refactor";
        action.context.all = input::ContextKeys{input::ContextKey::EditorFocus};
        action.context.none = input::ContextKeys{input::ContextKey::EditorReadOnly, input::ContextKey::DialogFocus};
        action.handler = [this, operation](const input::ActionContext&, const input::InvokeInfo&) {
            showLuaRefactoring(operation);
        };
        (void)m_registry.add(std::move(action));
    }
}
void Workbench::showLuaRefactoring(std::string operation) {
    auto* view = m_actionHost.active();
    if (!view || syntax::languageForFileName(view->path()) != syntax::LanguageId::Lua || m_projectRoot.empty())
        return;
    const auto trust = m_services.trust.snapshot(m_projectRoot);
    if (!trust.trusted) {
        QMessageBox::warning(m_window, "Lua refactoring", "Trust this workspace before applying refactorings.");
        return;
    }
    const QPointer<Workbench> owner(this);
    const QPointer<ui::EditorView> selected(view);
    const auto path = view->path();
    const auto original = view->editor().document().text();
    const auto root = m_projectRoot;
    const auto provider = m_lua;
    const auto& selections = view->editor().state().selections;
    if (selections.empty())
        return;
    const auto range = selections.all()[selections.primaryIndex()].range();
    std::string name;
    if (operation == "extract") {
        bool accepted = false;
        name = QInputDialog::getText(m_window, "Extract Lua Function", "Fresh local function name:", QLineEdit::Normal,
                                     {}, &accepted)
                   .toStdString();
        if (!owner || !selected || !accepted)
            return;
    }
    const auto permission = m_services.trust.snapshot(root);
    if (m_projectRoot != root || m_lua != provider || !permission.trusted || permission.revision != trust.revision ||
        selected->path() != path || selected->editor().document().text().version() != original.version())
        return;
    auto* session = selected->syntaxSession();
    if (!session)
        return;
    session->poll();
    const auto tree = session->tree();
    if (tree.empty() || tree.stale() || tree.text().version() != original.version() ||
        tree.text().text() != original.text()) {
        QMessageBox::warning(m_window, "Lua refactoring", "Wait for the current Lua syntax parse, then retry.");
        return;
    }
    auto cancelled = std::make_shared<std::stop_source>();
    auto* progress = new QProgressDialog("Planning actual Lua AST edits…", "Cancel", 0, 0, m_window);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setWindowModality(Qt::NonModal);
    progress->show();
    connect(progress, &QProgressDialog::canceled, progress, [cancelled] { cancelled->request_stop(); });
    const QPointer<QProgressDialog> pending(progress);
    m_retiredLua.push_back(m_pool->spawn(
        sched::Lane::UserVisible,
        prepareLuaAst(
            tree, operation, range, std::move(name), cancelled->get_token(),
            [owner, selected, path, original, root, provider, trust, cancelled,
             pending](Result<lang::LuaRefactoringProposal> proposed) mutable {
                if (!owner)
                    return;
                QMetaObject::invokeMethod(
                    owner.data(),
                    [owner, selected, path, original, root, provider, trust, cancelled, pending,
                     proposed = std::move(proposed)]() mutable {
                        if (pending) {
                            pending->reset();
                            pending->deleteLater();
                        }
                        if (!owner || cancelled->stop_requested() || !selected)
                            return;
                        const auto current = owner->m_services.trust.snapshot(root);
                        if (owner->m_shutDown || owner->m_projectRoot != root || owner->m_lua != provider ||
                            !current.trusted || current.revision != trust.revision || selected->path() != path ||
                            selected->editor().document().text().version() != original.version())
                            return;
                        if (!proposed) {
                            QMessageBox::warning(owner->m_window, "Lua refactoring refused",
                                                 displayLua(proposed.error().message()));
                            return;
                        }
                        auto after = original.text();
                        for (auto edit = proposed->edits.rbegin(); edit != proposed->edits.rend(); ++edit)
                            after.replace(edit->range.begin.value(),
                                          edit->range.end.value() - edit->range.begin.value(), edit->replacement);
                        document::RefactoringPlan plan;
                        plan.label = proposed->title;
                        plan.buffers.push_back({path, original.text(), std::move(after)});
                        plan.revalidateContext = [owner, selected, path, original, root, provider,
                                                  trust]() -> Result<void> {
                            if (!owner || !selected || owner->m_shutDown || owner->m_projectRoot != root ||
                                owner->m_lua != provider || selected->path() != path ||
                                selected->editor().document().text().version() != original.version())
                                return fail(ErrorCode::Busy, "Lua preview context changed; prepare it again");
                            const auto permission = owner->m_services.trust.snapshot(root);
                            if (!permission.trusted || permission.revision != trust.revision)
                                return fail(ErrorCode::Untrusted, "Workspace trust changed during Lua preview");
                            return {};
                        };
                        if (!owner->previewCppRefactoring(plan) || !owner)
                            return;
                        auto outcome = owner->applyRefactoring(plan);
                        if (!owner)
                            return;
                        if (!outcome)
                            QMessageBox::warning(owner->m_window, "Lua refactoring refused",
                                                 displayLua(outcome.error().message()));
                        else if (outcome->status == document::RefactoringStatus::PendingRecovery)
                            QMessageBox::warning(owner->m_window, "Lua refactoring committed",
                                                 "Settlement is pending recovery. Do not retry this operation.");
                    },
                    Qt::QueuedConnection);
            })));
}
} // namespace frost::app
