// SPDX-License-Identifier: Apache-2.0
// REF-15 normal actions: fresh syntax queries, captured file scopes, preview and one REF-1 history edge.
#include "frost/app/workbench.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/syntax/structural_search.hpp"
#include "frost/text/text_file.hpp"
#include "frost/ui/dialog_chrome.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressDialog>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
namespace frost::app {
namespace {
struct StructuralInput {
    std::string path;
    QPointer<ui::EditorView> editor; // Only inspected on owner thread.
    std::optional<text::Snapshot> overlay;
    std::optional<std::string> disk;
    std::optional<syntax::StructuralResult> result;
};
struct StructuralWork {
    syntax::LanguageId language = syntax::LanguageId::None;
    std::vector<StructuralInput> inputs;
    std::string pattern;
    std::optional<std::string> replacement;
    document::RefactoringPlan plan;
};
QString displaySsr(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}
bool inside(std::string_view root, std::string_view path) {
    const auto r = lang::documentKey(root), p = lang::documentKey(path);
    return p.size() > r.size() && p.starts_with(r) &&
           (r.back() == '/' || r.back() == '\\' || p[r.size()] == '/' || p[r.size()] == '\\');
}
sched::Task<void> prepareStructural(io::IFileSystem* fs, std::string root, std::shared_ptr<StructuralWork> work,
                                    std::stop_token stop, std::function<void(Result<void>)> done) {
    std::size_t bytes = 0, matches = 0, retained = 0;
    const auto deadline = systemClock().steadyNow() + std::chrono::seconds(10);
    for (auto& input : work->inputs) {
        if (stop.stop_requested() || systemClock().steadyNow() >= deadline) {
            done(fail(ErrorCode::Cancelled, "Structural scope cancelled or exceeded its ten-second budget"));
            co_return;
        }
        auto physical = fs->physicalPath(input.path);
        auto physicalRoot = fs->physicalPath(root);
        if (!physical || !physicalRoot || !inside(*physicalRoot, *physical)) {
            done(fail(ErrorCode::PermissionDenied, "Structural scope file is outside the physical workspace"));
            co_return;
        }
        text::Snapshot snapshot;
        if (input.overlay)
            snapshot = *input.overlay;
        else {
            auto raw = fs->readFileSnapshot(input.path, 4 * 1024 * 1024);
            if (!raw) {
                done(std::unexpected(raw.error()));
                co_return;
            }
            auto decoded = text::decodeTextFile(*raw);
            if (!decoded) {
                done(std::unexpected(decoded.error()));
                co_return;
            }
            snapshot = decoded->buffer.current();
            input.disk = std::move(*raw);
            work->plan.dependencies.push_back({input.path, input.disk});
        }
        bytes += snapshot.size().value();
        if (bytes > 16 * 1024 * 1024) {
            done(fail(ErrorCode::OutOfRange, "Structural scope exceeds 16 MiB"));
            co_return;
        }
        auto result = syntax::structuralSearch(
            work->language, snapshot, work->pattern,
            work->replacement ? std::optional<std::string_view>(*work->replacement) : std::nullopt, stop);
        if (!result) {
            done(std::unexpected(result.error().withContext(input.path)));
            co_return;
        }
        for (const auto& match : result->matches)
            for (const auto& [name, capture] : match.captures) {
                (void)name;
                retained += capture.text.size();
                if (retained > 16 * 1024 * 1024) {
                    done(fail(ErrorCode::OutOfRange, "Structural captured-source budget exceeded"));
                    co_return;
                }
            }
        matches += result->matches.size();
        if (matches > 2000) {
            done(fail(ErrorCode::OutOfRange, "Structural scope exceeds 2000 results"));
            co_return;
        }
        if (input.overlay)
            work->plan.bufferDependencies.push_back({input.path, snapshot.text()});
        if (result->after && *result->after != snapshot.text()) {
            if (input.overlay)
                work->plan.buffers.push_back({input.path, snapshot.text(), *result->after});
            else {
                auto decoded = text::decodeTextFile(*input.disk);
                auto after = text::TextBuffer::fromUtf8(*result->after);
                if (!decoded || !after) {
                    done(fail(ErrorCode::Corrupt, "Structural output cannot preserve its file encoding"));
                    co_return;
                }
                auto encoded = text::encodeForSave(after->current(), decoded->encoding);
                if (!encoded) {
                    done(std::unexpected(encoded.error()));
                    co_return;
                }
                work->plan.disk.push_back({input.path, *input.disk, std::move(*encoded)});
            }
        }
        input.result = std::move(*result);
    }
    done({});
    co_return;
}
} // namespace
void Workbench::registerStructuralSearchActions() {
    for (const bool replace : {false, true}) {
        input::ActionSpec action;
        action.id = replace ? "refactor.structuralReplace" : "search.structuralSearch";
        action.name = replace ? "Structural Replace…" : "Structural Search…";
        action.group = replace ? "Refactor" : "Search";
        action.context.all = {input::ContextKey::EditorFocus};
        action.context.none = {input::ContextKey::DialogFocus};
        if (replace)
            action.context.none.set(input::ContextKey::EditorReadOnly);
        action.handler = [this, replace](const input::ActionContext&, const input::InvokeInfo&) {
            showStructuralSearch(replace);
        };
        (void)m_registry.add(std::move(action));
    }
}
void Workbench::showStructuralSearch(bool replace) {
    auto* current = currentView();
    if (!current || m_projectRoot.empty())
        return;
    const QPointer<ui::EditorView> selected(&current->editor());
    const auto selectedPath = current->path();
    const auto selectedVersion = current->editor().editor().document().text().version();
    const auto language = syntax::languageForFileName(current->path());
    if (language == syntax::LanguageId::None) {
        notify("Structural search", "No syntax parser is available for this document.", ui::NotificationKind::Error);
        return;
    }
    const QPointer<Workbench> owner(this);
    const std::string root = m_projectRoot;
    const auto permission = m_services.trust.snapshot(root);
    if (replace && !permission.trusted) {
        notify("Structural replace", "Trust the workspace before replacement.", ui::NotificationKind::Error);
        return;
    }
    QDialog dialog(m_window);
    dialog.setObjectName("structuralSearchPattern");
    dialog.setWindowTitle(replace ? "Structural Replace" : "Structural Search");
    auto* form = new QFormLayout(&dialog);
    auto* description =
        new QLabel("Syntax nodes, not semantic types or bindings. One tree-sitter pattern with @match.\n"
                   "Literal #eq?, #not-eq?, #any-of?, #not-any-of? predicates only.\n"
                   "Replacement: ${capture} copies exact bytes; $$ inserts a dollar. See grammar node names.");
    description->setWordWrap(true);
    form->addRow(description);
    QPlainTextEdit pattern(&dialog);
    pattern.setObjectName("structuralPattern");
    pattern.setPlainText("((identifier) @match (#eq? @match \"old\"))");
    form->addRow("Pattern", &pattern);
    QPlainTextEdit replacement(&dialog);
    replacement.setObjectName("structuralReplacement");
    replacement.setPlainText("${match}");
    if (replace)
        form->addRow("Replacement", &replacement);
    QComboBox scope(&dialog);
    scope.setObjectName("structuralScope");
    scope.addItems({"Current document", "Open documents (same language)", "Indexed project files (same language)"});
    form->addRow("Scope", &scope);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    form->addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    ui::installDialogChrome(&dialog, true);
    dialog.resize(760, 460);
    if (dialog.exec() != QDialog::Accepted || !owner || m_projectRoot != root || !selected || !currentView() ||
        &currentView()->editor() != selected || currentView()->path() != selectedPath ||
        selected->editor().document().text().version() != selectedVersion)
        return;
    auto work = std::make_shared<StructuralWork>();
    work->language = language;
    work->pattern = pattern.toPlainText().toStdString();
    if (replace)
        work->replacement = replacement.toPlainText().toStdString();
    work->plan.label = "Structural Replace";
    std::map<std::string, StructuralInput, std::less<>> inputs;
    if (scope.currentIndex() == 2 && m_nav)
        for (const auto& path : m_nav->projectFiles().candidates())
            if (syntax::languageForFileName(path) == language && inside(root, path))
                inputs.emplace(lang::documentKey(path), StructuralInput{path, {}, {}, {}, {}});
    for (auto* view : m_editors->uniqueDocumentViews()) {
        if (syntax::languageForFileName(view->path()) != language || !inside(root, view->path()) ||
            (scope.currentIndex() == 0 && view->path() != selectedPath))
            continue;
        inputs.insert_or_assign(
            lang::documentKey(view->path()),
            StructuralInput{view->path(), &view->editor(), view->editor().editor().document().text(), {}, {}});
    }
    if (inputs.empty() || inputs.size() > 128) {
        notify("Structural search", "Select between one and 128 same-language workspace files.",
               ui::NotificationKind::Error);
        return;
    }
    for (auto& [path, input] : inputs) {
        (void)path;
        work->inputs.push_back(std::move(input));
    }
    m_structuralStop.request_stop();
    m_structuralStop = std::stop_source{};
    auto stop = std::make_shared<std::stop_source>(m_structuralStop);
    auto* progress = new QProgressDialog("Parsing and matching structural scope…", "Cancel", 0, 0, m_window);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->show();
    connect(progress, &QProgressDialog::canceled, progress, [stop] { stop->request_stop(); });
    const QPointer<QProgressDialog> pending(progress);
    std::erase_if(m_retiredLua, [](const auto& task) { return task.valid() && task.ready(); });
    m_retiredLua.push_back(m_pool->spawn(
        sched::Lane::UserVisible,
        prepareStructural(
            &m_services.fs, root, work, stop->get_token(),
            [owner, root, permission, work, stop, pending, replace](Result<void> prepared) mutable {
                if (!owner)
                    return;
                QMetaObject::invokeMethod(
                    owner.data(),
                    [owner, root, permission, work, stop, pending, replace, prepared = std::move(prepared)]() mutable {
                        if (pending) {
                            pending->reset();
                            pending->deleteLater();
                        }
                        if (!owner || stop->stop_requested() || owner->m_shutDown || owner->m_projectRoot != root)
                            return;
                        const std::weak_ptr<StructuralWork> weakWork = work;
                        auto valid = [owner, root, permission, weakWork, replace]() -> Result<void> {
                            const auto work = weakWork.lock();
                            if (!work)
                                return fail(ErrorCode::Cancelled, "Structural scope was released");
                            if (!owner || owner->m_shutDown || owner->m_projectRoot != root)
                                return fail(ErrorCode::Busy, "Structural workspace changed");
                            auto trust = owner->m_services.trust.snapshot(root);
                            if (replace && (!trust.trusted || trust.revision != permission.revision))
                                return fail(ErrorCode::Untrusted, "Structural preview trust changed");
                            for (const auto& input : work->inputs) {
                                auto* view = owner->m_editors->find(input.path);
                                if (input.overlay &&
                                    (!input.editor || !view || &view->editor() != input.editor ||
                                     view->editor().editor().document().text().version() != input.overlay->version()))
                                    return fail(ErrorCode::Busy, "Structural source document changed");
                                if (!input.overlay && view &&
                                    (replace || view->editor().document().text().text() != input.result->before.text()))
                                    return fail(ErrorCode::Busy, "A structural disk input changed classification or "
                                                                 "source; prepare the scope again");
                            }
                            return {};
                        };
                        if (!prepared) {
                            owner->notify("Structural search", displaySsr(prepared.error().message()),
                                          ui::NotificationKind::Error);
                            return;
                        }
                        if (auto current = valid(); !current) {
                            owner->notify("Structural search", displaySsr(current.error().message()),
                                          ui::NotificationKind::Error);
                            return;
                        }
                        if (replace) {
                            if (work->plan.buffers.empty() && work->plan.disk.empty()) {
                                owner->notify("Structural replace",
                                              "No structural replacements changed the captured scope.",
                                              ui::NotificationKind::Info);
                                return;
                            }
                            work->plan.revalidateContext = valid;
                            if (!owner->previewCppRefactoring(work->plan) || !owner)
                                return;
                            auto applied = owner->applyRefactoring(work->plan);
                            if (!applied && owner)
                                owner->notify("Structural replace", displaySsr(applied.error().message()),
                                              ui::NotificationKind::Error);
                            return;
                        }
                        auto* results = new QDialog(owner->m_window);
                        results->setAttribute(Qt::WA_DeleteOnClose);
                        results->setObjectName("structuralSearchResults");
                        results->setWindowTitle("Structural Search — captured syntax matches");
                        auto* layout = new QVBoxLayout(results);
                        auto* table = new QTableWidget(results);
                        table->setColumnCount(3);
                        table->setHorizontalHeaderLabels({"File", "Byte range", "Matched source"});
                        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
                        layout->addWidget(table);
                        auto targets = std::make_shared<std::vector<std::pair<std::size_t, text::ByteRange>>>();
                        for (std::size_t index = 0; index < work->inputs.size(); ++index)
                            for (const auto& match : work->inputs[index].result->matches) {
                                const auto& input = work->inputs[index];
                                targets->emplace_back(index, match.range);
                                const int row = table->rowCount();
                                table->insertRow(row);
                                table->setItem(row, 0, new QTableWidgetItem(displaySsr(input.path)));
                                table->setItem(
                                    row, 1,
                                    new QTableWidgetItem(
                                        QString("%1–%2").arg(match.range.begin.value()).arg(match.range.end.value())));
                                table->setItem(
                                    row, 2,
                                    new QTableWidgetItem(displaySsr(match.captures.at("match").text.substr(0, 240))));
                            }
                        connect(table, &QTableWidget::cellDoubleClicked, results,
                                [owner, work, valid, targets](int row, int) {
                                    if (!owner || row < 0 || static_cast<std::size_t>(row) >= targets->size())
                                        return;
                                    if (auto admitted = valid(); !admitted) {
                                        owner->notify("Structural search", displaySsr(admitted.error().message()),
                                                      ui::NotificationKind::Error);
                                        return;
                                    }
                                    const auto [index, range] = (*targets)[static_cast<std::size_t>(row)];
                                    const auto& input = work->inputs[index];
                                    if (!owner->openPath(input.path) || !owner)
                                        return;
                                    auto* view = owner->currentView();
                                    if (!view || view->editor().editor().document().text().text() !=
                                                     input.result->before.text()) {
                                        owner->notify("Structural search",
                                                      "The source changed after this captured search; run it again.",
                                                      ui::NotificationKind::Error);
                                        return;
                                    }
                                    const std::vector<text::Selection> selection{{range.begin, range.end}};
                                    view->editor().setState(
                                        text::EditorState::from(text::SelectionSet::fromSelections(selection, 0)));
                                    view->editor().setFocus();
                                });
                        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
                        ui::installDialogChrome(results, true);
                        results->resize(1000, 560);
                        results->show();
                    },
                    Qt::QueuedConnection);
            })));
}
} // namespace frost::app
