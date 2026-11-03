// SPDX-License-Identifier: Apache-2.0
//
// The workbench's real editors (W1, ADR 0044): DocumentEditorFactory, an IEditorFactory that opens
// a file through the save manager (one FileDocument per path) and shows it in the real editor widget
// (frost::ui::EditorView), and DocumentEditorView, the IEditorView of one tab's editor. The tabs
// themselves come from the navigation workbench's DocumentEditorFactory (the registry's canonical
// path and banners), which opens these as its inner editors.
/*
 * Status. The tab's status bar fields come from the EditorView (caret, selection, indentation) and
 * the FileDocument (encoding, line separator, read-only, dirty), so the dirty marker follows the
 * document's saved state, including a document restored from the hot-exit journal.
 *
 * Closing. requestClose() asks the listener (the workbench), which saves a dirty document first
 * (Rider's autosave) and asks only when that save fails. Destroying a view tells the listener
 * (editorClosed), which closes the document in the language server; the registry closes the
 * document in the save manager when its last view goes.
 *
 * Settings. Each view follows its file's settings (the store plus .editorconfig) through an
 * EditorSettingsBinding when the factory has a store; soft wrap follows editor.softWrap (off by
 * default) everywhere.
 *
 * GUI thread only. The save manager, the listener and the store outlive the factory and every view.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/document/file_document.hpp"
#include "frost/document/save_manager.hpp"
#include "frost/io/file_system.hpp"
#include "frost/settings/store.hpp"
#include "frost/text/document.hpp"
#include "frost/ui/editor/editor_view.hpp"
#include "frost/ui/shell/editor_host.hpp"

#include <QPointer>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::app {

class DocumentEditorFactory;
class DocumentEditorView;

// What the factory's views report to their owner (the workbench). GUI thread.
class IDocumentEditorListener {
public:
    // A view was created for `view.document()` (also when the document was already open).
    virtual void editorOpened(DocumentEditorView& view) = 0;
    // The widget and document are both still alive.
    virtual void editorClosing(DocumentEditorView&) {}
    // The view is being destroyed; its document is still open.
    virtual void editorClosed(DocumentEditorView& view) = 0;
    // The text changed through the view (EditorView::textChanged).
    virtual void editorTextChanged(DocumentEditorView& view, const text::ChangeSet& change) = 0;
    // The tab asks to close: true lets it (the listener saves a dirty document or asks first).
    [[nodiscard]] virtual bool editorCloseRequested(DocumentEditorView& view) = 0;

protected:
    IDocumentEditorListener() = default;
    IDocumentEditorListener(const IDocumentEditorListener&) = default;
    IDocumentEditorListener& operator=(const IDocumentEditorListener&) = default;
    ~IDocumentEditorListener() = default;
};

class DocumentEditorView final : public ui::IEditorView {
public:
    // Takes `editor` (a view on document.document()). `host`, `listener` and `factory` (the one that
    // lists the view; may be null) outlive the view.
    DocumentEditorView(document::FileDocument& document, std::unique_ptr<ui::EditorView> editor, ui::IEditorHost& host,
                       IDocumentEditorListener& listener, DocumentEditorFactory* factory = nullptr);
    ~DocumentEditorView() override;

    [[nodiscard]] QWidget* widget() override;
    [[nodiscard]] const std::string& path() const override { return m_path; }
    [[nodiscard]] ui::EditorStatus status() const override;
    void goToLine(int line, int column) override;
    void goToStatusPosition(int line, int column) override;
    [[nodiscard]] input::ContextKeys contextKeys() const override;
    [[nodiscard]] bool requestClose() override;
    // The caret and the first visible row (session restore).
    [[nodiscard]] ui::EditorViewState viewState() const override;
    void restoreViewState(const ui::EditorViewState& state) override;

    [[nodiscard]] ui::EditorView& editor() noexcept { return *m_editor; }
    [[nodiscard]] document::FileDocument& document() noexcept { return *m_document; }

    // Reads the document's file facts (encoding, separator, read-only) into the editor and tells the
    // host the status changed (after a save, a reload or an external-change decision).
    void refresh();
    // After a save: the editor's undo baseline becomes the saved one, then refresh().
    void markSaved();
    // After the document's text changed outside the editor (a reload): the editor follows it.
    void reloadFromDocument();
    /*
     * Follows a state change of the document reported by the registry (a save, a reload, a merge
     * decision): reloads when the text changed outside the editor, else records a clean document as
     * saved. Returns what happened.
     */
    enum class Followed : std::uint8_t { Nothing, Reloaded, Saved };
    Followed followDocument();

private:
    [[nodiscard]] std::uint64_t foldTextHash() const;
    mutable std::optional<text::Snapshot> m_foldHashSnapshot;
    mutable std::uint64_t m_foldHash = 0;
    document::FileDocument* m_document;
    std::string m_path;
    std::unique_ptr<ui::EditorView> m_editor;
    ui::IEditorHost& m_host;
    IDocumentEditorListener& m_listener;
    DocumentEditorFactory* m_factory;
};

class DocumentEditorFactory final : public ui::IEditorFactory {
public:
    DocumentEditorFactory(document::SaveManager& documents, IDocumentEditorListener& listener) noexcept
        : m_documents(documents), m_listener(listener) {}

    // What new views get (the services' pointers must outlive the views). Applies to later opens.
    void setServices(ui::EditorServices services) { m_services = services; }
    // The store and file system each new view's settings binding follows; null store: defaults.
    void setSettings(settings::SettingsStore* store, io::IFileSystem* fs) noexcept {
        m_store = store;
        m_fs = fs;
    }

    /*
     * Opens `path` through SaveManager::open (the existing document when it is open) in a new
     * EditorView. Errors: the save manager's (the load service's refusals: NotFound,
     * PermissionDenied, IoError, OutOfRange for the large-file guard).
     */
    [[nodiscard]] Result<std::unique_ptr<ui::IEditorView>> open(const std::string& path,
                                                                ui::IEditorHost& host) override;

    // The open views, in opening order.
    [[nodiscard]] const std::vector<DocumentEditorView*>& views() const noexcept { return m_views; }
    // The view of `document`, or nullptr.
    [[nodiscard]] DocumentEditorView* find(const document::FileDocument& document) const noexcept;
    // The view whose path has `path`'s document key (lang::documentKey), or nullptr.
    [[nodiscard]] DocumentEditorView* find(std::string_view path) const;

private:
    friend class DocumentEditorView;
    void forget(DocumentEditorView& view) noexcept;

    document::SaveManager& m_documents;
    IDocumentEditorListener& m_listener;
    ui::EditorServices m_services;
    settings::SettingsStore* m_store = nullptr;
    io::IFileSystem* m_fs = nullptr;
    std::vector<DocumentEditorView*> m_views;
    // Fold state of recently closed views, keyed by canonical document identity.
    std::map<std::string, ui::EditorViewState, std::less<>> m_closedFolds;
};

// The status-bar encoding token of `document` ("UTF-8", "UTF-8 BOM") and its line separator ("LF").
[[nodiscard]] std::string encodingToken(const document::FileDocument& document);
[[nodiscard]] std::string lineSeparatorToken(const document::FileDocument& document);

} // namespace frost::app
