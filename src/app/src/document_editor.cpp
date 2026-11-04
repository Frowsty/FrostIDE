// SPDX-License-Identifier: Apache-2.0
//
// DocumentEditorFactory and DocumentEditorView (frost/app/document_editor.hpp).

#include "frost/app/document_editor.hpp"

#include "frost/lang/uri.hpp"
#include "frost/text/encoding.hpp"
#include "frost/text/line_separator.hpp"
#include "frost/ui/editor/editor_settings.hpp"

#include <QObject>
#include <QScrollBar>

#include <algorithm>
#include <utility>

namespace frost::app {

namespace {
std::uint64_t hashFoldText(const text::Snapshot& snapshot) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const auto chunk : snapshot.chunks({ByteOffset{0}, snapshot.size()})) {
        for (const char c : chunk) {
            hash ^= static_cast<unsigned char>(c);
            hash *= 0x100000001b3ULL;
        }
    }
    return hash;
}
} // namespace

std::string encodingToken(const document::FileDocument& document) {
    const text::TextFileFormat& format = document.encoding().format;
    std::string token(text::encodingName(format.encoding));
    if (format.bom) {
        token += " BOM";
    }
    return token;
}

std::string lineSeparatorToken(const document::FileDocument& document) {
    return std::string(text::separatorName(document.encoding().format.lineSeparator));
}

// --- DocumentEditorView ---

DocumentEditorView::DocumentEditorView(document::FileDocument& document, std::unique_ptr<ui::EditorView> editor,
                                       ui::IEditorHost& host, IDocumentEditorListener& listener,
                                       DocumentEditorFactory* factory)
    : m_document(&document), m_path(document.path()), m_editor(std::move(editor)), m_host(host), m_listener(listener),
      m_factory(factory) {
    m_editor->setFileInfo(ui::EditorFileInfo{encodingToken(document), lineSeparatorToken(document),
                                             document.readOnly() != document::ReadOnlyReason::None,
                                             document.readOnly() == document::ReadOnlyReason::LongLine});
    QObject::connect(m_editor.get(), &ui::EditorView::statusChanged, m_editor.get(),
                     [this] { m_host.editorStatusChanged(*this); });
    QObject::connect(m_editor.get(), &ui::EditorView::textChanged, m_editor.get(),
                     [this](const text::ChangeSet& change) {
                         m_listener.editorTextChanged(*this, change);
                         // The document's dirty flag (status().dirty) may change with any edit.
                         m_host.editorStatusChanged(*this);
                     });
}

DocumentEditorView::~DocumentEditorView() {
    if (m_factory != nullptr) {
        m_factory->forget(*this);
    }
    m_listener.editorClosing(*this);
    // The widget goes first: the listener may close the document it shows.
    m_editor.reset();
    m_listener.editorClosed(*this);
}

QWidget* DocumentEditorView::widget() { return m_editor.get(); }

ui::EditorStatus DocumentEditorView::status() const {
    const ui::EditorViewStatus view = m_editor->status();
    ui::EditorStatus out;
    out.line = view.line;
    out.column = view.column;
    out.selectedCharacters = view.selectedCharacters;
    out.lineSeparator = view.lineSeparator;
    out.encoding = view.encoding;
    out.indent = ui::IndentStatus{view.useTabs, view.indentSize};
    out.readOnly = view.readOnly;
    out.dirty = m_document->dirty();
    return out;
}

void DocumentEditorView::goToLine(int line, int column) {
    m_editor->goToLine(line, column);
    m_host.editorStatusChanged(*this);
}

void DocumentEditorView::goToStatusPosition(int line, int column) {
    m_editor->goToStatusPosition(line, column);
    m_host.editorStatusChanged(*this);
}

input::ContextKeys DocumentEditorView::contextKeys() const { return m_editor->contextKeys(); }

bool DocumentEditorView::requestClose() { return m_listener.editorCloseRequested(*this); }

std::uint64_t DocumentEditorView::foldTextHash() const {
    const auto& snapshot = m_editor->editor().text();
    if (!m_foldHashSnapshot || !snapshot.sharesStorageWith(*m_foldHashSnapshot) ||
        snapshot.version() != m_foldHashSnapshot->version()) {
        m_foldHash = hashFoldText(snapshot);
        m_foldHashSnapshot = snapshot;
    }
    return m_foldHash;
}

ui::EditorViewState DocumentEditorView::viewState() const {
    const ui::EditorViewStatus status = m_editor->status();
    ui::EditorViewState state{status.line, status.column, static_cast<int>(m_editor->firstVisibleRow())};
    if (!m_editor->collapsedFolds().empty()) {
        state.foldTextHash = foldTextHash();
        for (const auto range : m_editor->collapsedFolds())
            state.folds.emplace_back(range.begin.value(), range.end.value());
    }
    return state;
}

void DocumentEditorView::restoreViewState(const ui::EditorViewState& state) {
    m_editor->goToStatusPosition(state.line, state.column);
    if (!state.folds.empty() && state.foldTextHash == foldTextHash()) {
        std::vector<text::ByteRange> folds;
        for (const auto& [begin, end] : state.folds)
            folds.push_back({ByteOffset{begin}, ByteOffset{end}});
        (void)m_editor->setCollapsedFolds(std::move(folds));
        m_editor->revealFoldAt(m_editor->editor().state().primary().head);
    }
    m_editor->verticalScrollBar()->setValue(state.topLine);
    m_host.editorStatusChanged(*this);
}

void DocumentEditorView::refresh() {
    m_editor->setFileInfo(ui::EditorFileInfo{encodingToken(*m_document), lineSeparatorToken(*m_document),
                                             m_document->readOnly() != document::ReadOnlyReason::None,
                                             m_document->readOnly() == document::ReadOnlyReason::LongLine});
    m_host.editorStatusChanged(*this);
}

void DocumentEditorView::markSaved() {
    m_editor->markSaved();
    refresh();
}

void DocumentEditorView::setPath(std::string path) {
    m_path = std::move(path);
    m_editor->setPath(m_path);
    if (auto* binding = m_editor->findChild<ui::EditorSettingsBinding*>()) {
        binding->setPath(m_path);
    }
    markSaved();
}

void DocumentEditorView::reloadFromDocument() {
    m_editor->reloadFromDocument();
    if (!m_document->dirty()) {
        m_editor->markSaved();
    }
    refresh();
}

DocumentEditorView::Followed DocumentEditorView::followDocument() {
    // The editor's display follows every edit made through it; a newer document version came from
    // elsewhere (the disk, a merge, a restored journal entry).
    if (m_editor->editor().display().version() != m_document->document().version()) {
        reloadFromDocument();
        return Followed::Reloaded;
    }
    if (!m_document->dirty() && m_editor->status().dirty) {
        markSaved();
        return Followed::Saved;
    }
    refresh();
    return Followed::Nothing;
}

// --- DocumentEditorFactory ---

Result<std::unique_ptr<ui::IEditorView>> DocumentEditorFactory::open(const std::string& path, ui::IEditorHost& host) {
    Result<document::FileDocument*> opened = m_documents.open(path);
    if (!opened) {
        return std::unexpected(std::move(opened.error()));
    }
    document::FileDocument& document = **opened;
    ui::EditorViewSettings settings;
    if (m_store != nullptr) {
        settings = ui::editorViewSettings(*m_store->snapshot());
    }
    std::unique_ptr<ui::EditorView> editor =
        ui::createEditorView(document.document(), document.path(), m_services, settings);
    if (m_store != nullptr && m_fs != nullptr) {
        // Owned by the editor widget: follows the settings files and .editorconfig for this file.
        new ui::EditorSettingsBinding(*m_store, *m_fs, document.path(), *editor, editor.get());
    }
    auto view = std::make_unique<DocumentEditorView>(document, std::move(editor), host, m_listener, this);
    if (const auto saved = m_closedFolds.find(lang::documentKey(view->path())); saved != m_closedFolds.end()) {
        ui::EditorViewState state;
        state.foldTextHash = saved->second.foldTextHash;
        state.folds = saved->second.folds;
        view->restoreViewState(state);
    }
    m_views.push_back(view.get());
    m_listener.editorOpened(*view);
    return std::unique_ptr<ui::IEditorView>(std::move(view));
}

std::vector<DocumentEditorView*> DocumentEditorFactory::uniqueDocumentViews() const {
    std::vector<DocumentEditorView*> result;
    for (DocumentEditorView* view : m_views) {
        if (std::ranges::none_of(
                result, [view](DocumentEditorView* existing) { return &existing->document() == &view->document(); }))
            result.push_back(view);
    }
    return result;
}

DocumentEditorView* DocumentEditorFactory::find(const document::FileDocument& document) const noexcept {
    const auto it =
        std::ranges::find_if(m_views, [&](DocumentEditorView* view) { return &view->document() == &document; });
    return it == m_views.end() ? nullptr : *it;
}

DocumentEditorView* DocumentEditorFactory::find(std::string_view path) const {
    const std::string key = lang::documentKey(path);
    const auto it =
        std::ranges::find_if(m_views, [&](DocumentEditorView* view) { return lang::documentKey(view->path()) == key; });
    return it == m_views.end() ? nullptr : *it;
}

void DocumentEditorFactory::forget(DocumentEditorView& view) noexcept {
    const std::string key = lang::documentKey(view.path());
    auto state = view.viewState();
    if (state.folds.empty())
        m_closedFolds.erase(key);
    else
        m_closedFolds.insert_or_assign(key, std::move(state));
    std::erase(m_views, &view);
}

} // namespace frost::app
