// SPDX-License-Identifier: Apache-2.0
//
// DocumentEditorFactory and DocumentEditorView (frost/app/document_editor.hpp).

#include "frost/app/document_editor.hpp"

#include "frost/lang/uri.hpp"
#include "frost/text/encoding.hpp"
#include "frost/text/line_separator.hpp"
#include "frost/ui/editor/editor_settings.hpp"

#include <QObject>

#include <algorithm>
#include <utility>

namespace frost::app {

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

input::ContextKeys DocumentEditorView::contextKeys() const { return m_editor->contextKeys(); }

bool DocumentEditorView::requestClose() { return m_listener.editorCloseRequested(*this); }

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

void DocumentEditorView::reloadFromDocument() {
    m_editor->reloadFromDocument();
    if (!m_document->dirty()) {
        m_editor->markSaved();
    }
    refresh();
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
    m_views.push_back(view.get());
    m_listener.editorOpened(*view);
    return std::unique_ptr<ui::IEditorView>(std::move(view));
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

void DocumentEditorFactory::forget(DocumentEditorView& view) noexcept { std::erase(m_views, &view); }

} // namespace frost::app
