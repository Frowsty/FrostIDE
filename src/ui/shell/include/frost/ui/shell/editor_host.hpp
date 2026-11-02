// SPDX-License-Identifier: Apache-2.0
//
// The editor seam of the shell (ADR 0038): what an editor tab hosts. EditorTabs owns one
// IEditorView per open document, created by an IEditorFactory, and listens to it through
// IEditorHost. m1/editor-view implements the factory with the real editor widget;
// PlaceholderEditorFactory (placeholder_editor.hpp) is the stand-in until then.
#pragma once

#include "frost/core/error.hpp"
#include "frost/input/action.hpp"

#include <memory>
#include <string>

class QWidget;

namespace frost::ui {

// How the document indents, for the status bar ("4 spaces", "Tab").
struct IndentStatus {
    bool useTabs = false;
    int size = 4;
    friend bool operator==(const IndentStatus&, const IndentStatus&) = default;
};

/*
 * What the status bar shows for the active editor. `lineSeparator` and `encoding` are the
 * technical tokens frost::text::formatStatus produces ("CRLF", "UTF-8 BOM"; ADR 0018), shown
 * untranslated. Line and column are 1-based; the column counts what the editor's caret counts
 * (grapheme clusters in the real editor).
 */
struct EditorStatus {
    int line = 1;
    int column = 1;
    int selectedCharacters = 0;
    std::string lineSeparator = "LF";
    std::string encoding = "UTF-8";
    IndentStatus indent;
    bool readOnly = false;
    bool dirty = false;
    friend bool operator==(const EditorStatus&, const EditorStatus&) = default;
};

class IEditorView;

/*
 * The shell's side of an editor: an editor calls these on the UI thread whenever what they
 * describe changed. Implemented by EditorTabs. Never deleted through this interface.
 */
class IEditorHost {
public:
    // status() changed (caret, selection, encoding, read-only state, dirty flag).
    virtual void editorStatusChanged(IEditorView& editor) = 0;

protected:
    IEditorHost() = default;
    IEditorHost(const IEditorHost&) = default;
    IEditorHost& operator=(const IEditorHost&) = default;
    ~IEditorHost() = default;
};

/*
 * One open document's editor. Owned by EditorTabs (std::unique_ptr), which removes widget() from
 * its stack before destroying the view; the view owns its widget and deletes it in its
 * destructor. All calls run on the UI thread.
 */
class IEditorView {
public:
    IEditorView() = default;
    IEditorView(const IEditorView&) = delete ("an editor view has identity");
    IEditorView& operator=(const IEditorView&) = delete ("an editor view has identity");
    virtual ~IEditorView() = default;

    // The widget the tab shows. Valid for the view's lifetime.
    [[nodiscard]] virtual QWidget* widget() = 0;
    // The document's path (absolute, UTF-8, the form the factory was given).
    [[nodiscard]] virtual const std::string& path() const = 0;
    [[nodiscard]] virtual EditorStatus status() const = 0;
    // Moves the caret to 1-based `line` and `column`, clamped to the document.
    virtual void goToLine(int line, int column) = 0;
    // Context keys the editor adds while it has focus (EditorReadOnly, EditorHasSelection,
    // LookupActive); EditorFocus itself is set by the shell.
    [[nodiscard]] virtual input::ContextKeys contextKeys() const { return {}; }
    // Asked before the tab closes; false keeps it open (the editor may save or ask first).
    [[nodiscard]] virtual bool requestClose() { return true; }
};

/*
 * Creates editors. Errors: those of reading the document (NotFound, PermissionDenied, IoError,
 * OutOfRange for a file too large to open). UI thread only. Never deleted through this interface.
 */
class IEditorFactory {
public:
    [[nodiscard]] virtual Result<std::unique_ptr<IEditorView>> open(const std::string& path, IEditorHost& host) = 0;

protected:
    IEditorFactory() = default;
    IEditorFactory(const IEditorFactory&) = default;
    IEditorFactory& operator=(const IEditorFactory&) = default;
    ~IEditorFactory() = default;
};

} // namespace frost::ui
