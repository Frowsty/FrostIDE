// SPDX-License-Identifier: Apache-2.0
//
// IEditorLanguageHost: what the language features (LanguageController, ADR 0041) need from the
// editor. The editor view is not on this slice's branch, so the workbench implements this seam
// over its active EditorView; tests implement it over a TextBuffer.
#pragma once

#include "frost/core/error.hpp"
#include "frost/lang/document_sync.hpp"
#include "frost/lang/navigation.hpp"
#include "frost/lang/refactoring.hpp"
#include "frost/lang/workspace_edit.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/snapshot.hpp"

#include <QRect>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class QWidget;

namespace frost::ui::lang {

struct FormattingSettings {
    std::uint32_t tabSize = 4;
    std::uint32_t indentSize = 4;
    bool insertSpaces = true;
    bool operator==(const FormattingSettings&) const = default;
};

/*
 * The active editor, as the language features see it. Every call is made on the UI thread.
 * The host keeps clangd's copy of the document current (ClangdService::changeDocument) before
 * it reports a new documentVersion(), so positions computed from snapshot() match what the
 * server parses.
 */
class IEditorLanguageHost {
public:
    virtual ~IEditorLanguageHost() = default;

    // The absolute path of the active document; empty when no editor is active.
    [[nodiscard]] virtual std::string documentPath() const = 0;
    // The active document's text; nullopt when no editor is active.
    [[nodiscard]] virtual std::optional<text::Snapshot> snapshot() const = 0;
    // The LSP version of snapshot() (the version last sent to the server).
    [[nodiscard]] virtual std::int64_t documentVersion() const = 0;
    // Identity of the document instance and active language-server generation, UI thread only.
    [[nodiscard]] virtual std::string structureIdentity() const { return documentPath(); }
    [[nodiscard]] virtual ByteOffset caretOffset() const = 0;
    // The selection, when one is non-empty.
    [[nodiscard]] virtual std::optional<text::ByteRange> selection() const = 0;
    // Effective tab width and indentation style for the active file, including EditorConfig.
    [[nodiscard]] virtual FormattingSettings formattingOptions() const = 0;
    // The caret's line box in global (screen) coordinates; popups open below or above it.
    [[nodiscard]] virtual QRect caretScreenRect() const = 0;
    // The caret's box for a byte offset of the active document, in global coordinates.
    [[nodiscard]] virtual QRect screenRectAt(ByteOffset offset) const = 0;

    /*
     * Applies `edits` (sorted, non-overlapping, in snapshot() offsets) as one undo step named
     * `label`, then puts the caret at `caret` (an offset in the edited text) when given.
     * Errors: the editor's (read-only document, edits out of range).
     */
    [[nodiscard]] virtual Result<void> applyEdits(std::span<const ::frost::lang::ByteEdit> edits,
                                                  std::string_view label, std::optional<ByteOffset> caret) = 0;
    // Selects `range` of the active document (a snippet tab stop) and shows it.
    virtual void select(text::ByteRange range) = 0;
    // Opens `path` (another file opens in a new tab) and puts the caret at `position`.
    virtual void openLocation(std::string_view path, ::frost::lang::LspPosition position) = 0;
    // Highlights the usages of the symbol under the caret; an empty list clears them.
    virtual void setUsageHighlights(std::vector<::frost::lang::DocumentHighlight> highlights) = 0;
    // Every open document and its current text, for find usages' line previews.
    [[nodiscard]] virtual std::vector<std::pair<std::string, text::Snapshot>> openDocuments() const = 0;
    // The editor widget: parent of non-activating popups and the focus to return to.
    [[nodiscard]] virtual QWidget* editorWidget() const = 0;

protected:
    IEditorLanguageHost() = default;
    IEditorLanguageHost(const IEditorLanguageHost&) = default;
    IEditorLanguageHost& operator=(const IEditorLanguageHost&) = default;
};

} // namespace frost::ui::lang
