// SPDX-License-Identifier: Apache-2.0
//
// LanguageBridge (language_bridge.hpp): the language features' server, editor host and open
// documents over the Workbench's clangd and editor views.

#include "frost/app/workbench.hpp"
#include "frost/lang/document_sync.hpp"
#include "frost/text/editor/commands.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/lang/editor_insight_binding.hpp"
#include "frost/ui/shell/editor_tabs.hpp"

#include "language_bridge.hpp"

#include <algorithm>
#include <utility>

namespace frost::app {

namespace {

// Where `offset` (before the batch) lands after the sorted, non-overlapping `edits`: shifted by the
// edits before it; inside a replaced range, at the end of the replacement.
ByteOffset mapThrough(ByteOffset offset, std::span<const lang::ByteEdit> edits) {
    std::int64_t delta = 0;
    for (const lang::ByteEdit& edit : edits) {
        const auto begin = static_cast<std::int64_t>(edit.range.begin.value());
        const auto end = static_cast<std::int64_t>(edit.range.end.value());
        const auto inserted = static_cast<std::int64_t>(edit.text.size());
        const auto at = static_cast<std::int64_t>(offset.value());
        if (end <= at && !(begin == end && begin == at)) {
            delta += inserted - (end - begin);
        } else if (begin < at && at < end) {
            return ByteOffset{static_cast<std::uint64_t>(begin + inserted + delta)};
        } else {
            break;
        }
    }
    return ByteOffset{static_cast<std::uint64_t>(static_cast<std::int64_t>(offset.value()) + delta)};
}

} // namespace

LanguageBridge::LanguageBridge(Workbench& workbench)
    : m_workbench(workbench), m_applier(static_cast<lang::IWorkspaceDocuments&>(*this), workbench.m_services.fs,
                                        lang::PositionEncoding::Utf16) {}

LanguageBridge::~LanguageBridge() = default;

lang::IWorkspaceEditApplier& LanguageBridge::applier() noexcept { return *this; }

Result<void> LanguageBridge::apply(const lang::WorkspaceEdit& edit, std::string_view label) {
    // Applying edits is on the GUI thread; encoding may change on a clangd restart.
    m_applier.setEncoding(encoding());
    return m_applier.apply(edit, label);
}

// --- ILspRequester ---

sched::Task<Result<lang::Json>> LanguageBridge::request(std::string method, lang::Json params, std::stop_token token) {
    return m_server.request(std::move(method), std::move(params), std::move(token));
}

lang::PositionEncoding LanguageBridge::encoding() const { return m_server.encoding(); }

std::optional<lang::ServerCapabilities> LanguageBridge::serverCapabilities() const {
    return m_server.serverCapabilities();
}

// --- IEditorLanguageHost ---

ui::EditorView* LanguageBridge::activeEditor() const {
    DocumentEditorView* view = m_workbench.currentView();
    return view != nullptr ? &view->editor() : nullptr;
}

std::string LanguageBridge::documentPath() const {
    const DocumentEditorView* view = m_workbench.currentView();
    return view != nullptr ? view->path() : std::string();
}

std::optional<text::Snapshot> LanguageBridge::snapshot() const {
    const ui::EditorView* editor = activeEditor();
    if (editor == nullptr) {
        return std::nullopt;
    }
    return editor->editor().text();
}

std::int64_t LanguageBridge::documentVersion() const {
    const lang::ClangdService* clangd = m_workbench.m_clangd.get();
    const std::string path = documentPath();
    if (isManagedSource(path))
        return m_workbench.m_managed ? m_workbench.m_managed->documentVersion(path).value_or(0) : 0;
    if (isLuaSource(path))
        return m_workbench.m_lua ? m_workbench.m_lua->documentVersion(path).value_or(0) : 0;
    if (clangd == nullptr || path.empty()) {
        return 0;
    }
    return clangd->documentVersion(path).value_or(0);
}

ByteOffset LanguageBridge::caretOffset() const {
    const ui::EditorView* editor = activeEditor();
    return editor != nullptr ? editor->editor().state().primary().head : ByteOffset{0};
}

std::optional<text::ByteRange> LanguageBridge::selection() const {
    const ui::EditorView* editor = activeEditor();
    if (editor == nullptr || editor->editor().state().primary().empty()) {
        return std::nullopt;
    }
    return editor->editor().state().primary().range();
}

ui::lang::FormattingSettings LanguageBridge::formattingOptions() const {
    const ui::EditorView* editor = activeEditor();
    if (!editor)
        return {};
    const auto& settings = editor->settings();
    return {.tabSize = settings.tabSize, .indentSize = settings.indentSize, .insertSpaces = settings.insertSpaces};
}

QRect LanguageBridge::caretScreenRect() const {
    const ui::EditorView* editor = activeEditor();
    return editor != nullptr ? editor->globalCaretRect(editor->editor().state().primary().head) : QRect();
}

QRect LanguageBridge::screenRectAt(ByteOffset offset) const {
    const ui::EditorView* editor = activeEditor();
    return editor != nullptr ? editor->globalCaretRect(offset) : QRect();
}

Result<void> LanguageBridge::applyTo(ui::EditorView& editor, std::span<const lang::ByteEdit> edits,
                                     std::optional<ByteOffset> caret) {
    if (editor.isReadOnly()) {
        return fail(ErrorCode::InvalidArgument, editor.path() + " is read-only");
    }
    text::CommandResult result;
    result.edits.reserve(edits.size());
    for (const lang::ByteEdit& edit : edits) {
        result.edits.push_back(text::TextEdit{edit.range, edit.text});
    }
    const ByteOffset at = caret.value_or(mapThrough(editor.editor().state().primary().head, edits));
    result.state = text::EditorState::caretAt(at);
    result.breakCoalescing = true; // One undo step of its own.
    if (!editor.apply(result)) {
        return editor.lastError() ? std::unexpected(*editor.lastError())
                                  : fail(ErrorCode::InvalidArgument, "the editor refused the edits");
    }
    return {};
}

Result<void> LanguageBridge::applyEdits(std::span<const lang::ByteEdit> edits, std::string_view,
                                        std::optional<ByteOffset> caret) {
    ui::EditorView* editor = activeEditor();
    if (editor == nullptr) {
        return fail(ErrorCode::NotFound, "no editor is active");
    }
    return applyTo(*editor, edits, caret);
}

void LanguageBridge::select(text::ByteRange range) {
    ui::EditorView* editor = activeEditor();
    if (editor == nullptr) {
        return;
    }
    const text::Selection selection{range.begin, range.end};
    editor->setState(text::EditorState::from(text::SelectionSet::fromSelections(std::span(&selection, 1), 0)));
}

void LanguageBridge::openLocation(std::string_view path, lang::LspPosition position) {
    // Through the one open path (a jump in the navigation history), then the exact LSP position.
    if (!m_workbench.openPath(path)) {
        return;
    }
    DocumentEditorView* view = m_workbench.editors().find(path);
    if (view == nullptr) {
        return;
    }
    const text::Snapshot& text = view->editor().editor().text();
    const Result<ByteOffset> offset = lang::fromLspPosition(text, position, encoding());
    if (!offset) {
        return;
    }
    const text::Point point = text.pointAt(*offset);
    view->goToLine(static_cast<int>(point.line.value()) + 1, static_cast<int>(point.column.value()) + 1);
}

void LanguageBridge::setUsageHighlights(std::vector<lang::DocumentHighlight> highlights) {
    ui::EditorView* editor = activeEditor();
    if (editor == nullptr) {
        return;
    }
    // Editor-insight's usage boxes (read and write usages differ) and their error-stripe marks.
    editor->setUsageHighlights(ui::lang::toUsageHighlights(highlights, editor->editor().text(), encoding()));
}

std::vector<std::pair<std::string, text::Snapshot>> LanguageBridge::openDocuments() const {
    std::vector<std::pair<std::string, text::Snapshot>> documents;
    for (DocumentEditorView* view : m_workbench.editors().views()) {
        documents.emplace_back(view->path(), view->editor().editor().text());
    }
    return documents;
}

QWidget* LanguageBridge::editorWidget() const {
    ui::EditorView* editor = activeEditor();
    return editor != nullptr ? static_cast<QWidget*>(editor) : static_cast<QWidget*>(m_workbench.window());
}

// --- IWorkspaceDocuments ---

std::optional<lang::OpenDocumentState> LanguageBridge::find(std::string_view path) {
    DocumentEditorView* view = m_workbench.editors().find(path);
    if (view == nullptr) {
        return std::nullopt;
    }
    lang::OpenDocumentState state;
    state.snapshot = view->editor().editor().text();
    if (isManagedSource(path) && m_workbench.m_managed)
        state.lspVersion = m_workbench.m_managed->documentVersion(path);
    else if (isLuaSource(path) && m_workbench.m_lua)
        state.lspVersion = m_workbench.m_lua->documentVersion(path);
    else if (isCFamilySource(path) && m_workbench.m_clangd)
        state.lspVersion = m_workbench.m_clangd->documentVersion(path);
    state.dirty = view->document().dirty();
    state.changedOnDisk = view->document().diskState() == document::DiskState::Changed ||
                          view->document().diskState() == document::DiskState::OutOfDate;
    return state;
}

Result<void> LanguageBridge::applyEdits(std::string_view path, std::span<const lang::ByteEdit> edits,
                                        std::string_view) {
    DocumentEditorView* view = m_workbench.editors().find(path);
    if (view == nullptr) {
        return fail(ErrorCode::NotFound, std::string(path) + " is not open");
    }
    return applyTo(view->editor(), edits, std::nullopt);
}

} // namespace frost::app
