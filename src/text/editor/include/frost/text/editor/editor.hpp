// SPDX-License-Identifier: Apache-2.0
//
// frost::text::Editor: the stateful side of the editor model (ADR 0025). It holds one view's
// selections and caret states and its DisplayMap over a borrowed Document, and turns CommandResults
// into Document edits, so every command reaches the undo tree as one step with the selections
// before and after it, and the display map follows every change.
/*
 * Undo grouping. Each CommandResult is one Document::edit batch, so a line operation or a replace
 * all is one undo step. Typing, Backspace and Delete coalesce through Document's rules (same kind,
 * inside the coalescing window, carets not moved in between); a result with breakCoalescing seals
 * the open step first, which is how a word, a line break or a paste starts a step of its own.
 * Undo and redo restore the selections the step recorded.
 *
 * Several editors may share a Document (split views): the editor that edits gets the ChangeSet
 * from apply(), and every other editor must be given it through follow() before its next command.
 *
 * Lifetime: the Document and the ITextShaper must outlive the Editor (or the shaper be replaced
 * with setShaper first). Thread safety: not thread-safe; the Document's writer thread, which is
 * also the DisplayMap's owner (the UI thread). Allocation failure terminates.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/text/display_map.hpp"
#include "frost/text/document.hpp"
#include "frost/text/editor/commands.hpp"
#include "frost/text/editor/language.hpp"
#include "frost/text/snapshot.hpp"
#include "frost/text/text_shaper.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace frost::text {

// An editor's configuration.
struct EditorConfig {
    WrapSettings wrap;
    EditorSettings settings;
    const LanguageInfo* language = nullptr; // nullptr: plain text.
    std::uint32_t pageRows = 20;            // Rows per PageUp/PageDown; the view sets its height.
};

// What an applied command, undo, redo or followed change did.
struct EditorUpdate {
    // The Document's ChangeSet when the text changed (or when undo/redo moved the history without
    // changing it).
    std::optional<ChangeSet> change;
    // The display rows to repaint (DisplayMap::apply).
    std::vector<RowEdit> rows;
};

class Editor {
public:
    using Inlay = DisplayMap::Inlay;

    // An editor on `document` with one caret at 0, wrapping and measuring with `config.wrap` and
    // `shaper`. Precondition: both outlive the editor.
    Editor(Document& document, const ITextShaper& shaper, EditorConfig config = {}) noexcept;

    Editor(Editor&&) noexcept = default;
    Editor& operator=(Editor&&) noexcept = default;
    Editor(const Editor&) = delete ("an editor owns its display map");
    Editor& operator=(const Editor&) = delete ("an editor owns its display map");
    ~Editor() = default;

    [[nodiscard]] Document& document() noexcept { return *m_document; }
    [[nodiscard]] const Document& document() const noexcept { return *m_document; }
    // The current text, borrowed until the next change. O(1).
    [[nodiscard]] const Snapshot& text() const noexcept { return m_document->text(); }
    [[nodiscard]] const DisplayMap& display() const noexcept { return m_display; }
    [[nodiscard]] const EditorState& state() const noexcept { return m_state; }
    [[nodiscard]] const EditorConfig& config() const noexcept { return m_config; }

    // The input for a pure command at the current state. O(carets).
    [[nodiscard]] CommandInput input() const noexcept;

    /*
     * Applies a command's result: seals the open undo step when asked, edits the Document with
     * EditContext{kind, selections before, selections after}, updates the display map and takes the
     * result's state. A result without edits only changes the state. Errors: Document::edit's (Busy
     * while an IME composition is active, the TextBuffer checks), and InvalidArgument from the
     * display map when a change of another editor was not followed. On error the state is kept.
     */
    [[nodiscard]] Result<EditorUpdate> apply(const CommandResult& result) noexcept;
    // Runs a pure command on input() and applies it: run(commands::type, "x").
    template<class Command, class... Args>
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward): preserve lvalue command invocation.
    [[nodiscard]] Result<EditorUpdate> run(Command&& command, Args&&... args) noexcept {
        return apply(command(input(), std::forward<Args>(args)...));
    }

    // Undo and redo through the Document; the selections become the step's recorded ones.
    // Errors: Document::undo's and redo's.
    [[nodiscard]] Result<EditorUpdate> undo() noexcept;
    [[nodiscard]] Result<EditorUpdate> redo() noexcept;
    // Follows a change another editor (or a reload) made to the shared Document: maps the
    // selections through its edits and updates the display map. Errors: the display map's
    // InvalidArgument when `change` does not continue from this editor's version.
    [[nodiscard]] Result<EditorUpdate> follow(const ChangeSet& change) noexcept;

    // Replaces the selections and caret states (a click, a find result shown elsewhere).
    // Precondition: every offset <= text().size().
    void setState(EditorState state) noexcept;
    void setSettings(const EditorSettings& settings) noexcept { m_config.settings = settings; }
    void setLanguage(const LanguageInfo* language) noexcept { m_config.language = language; }
    void setPageRows(std::uint32_t rows) noexcept { m_config.pageRows = rows; }
    // Wraps again (DisplayMap::setWrap); returns the rows to repaint.
    std::vector<RowEdit> setWrap(WrapSettings wrap) noexcept;
    // Measures with `shaper` from now on (DisplayMap::setShaper).
    std::vector<RowEdit> setShaper(const ITextShaper& shaper) noexcept;
    // Replaces the collapsed ranges in this view's display map.
    [[nodiscard]] Result<std::vector<RowEdit>> setFolds(std::vector<ByteRange> ranges) noexcept {
        return m_display.setFolds(std::move(ranges));
    }
    [[nodiscard]] Result<FoldId> addFold(ByteRange range) noexcept { return m_display.addFold(range); }
    [[nodiscard]] Result<void> removeFold(FoldId id) noexcept { return m_display.removeFold(id); }
    void setInlays(std::vector<Inlay> inlays) noexcept;
    [[nodiscard]] const std::vector<Inlay>& inlays() const noexcept;

private:
    // Follows a history step: display map, then the step's selections (else the mapped ones).
    [[nodiscard]] Result<EditorUpdate> finishHistory(ChangeSet change) noexcept;

    Document* m_document;
    EditorConfig m_config;
    DisplayMap m_display;
    EditorState m_state;
};

} // namespace frost::text
