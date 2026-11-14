// SPDX-License-Identifier: Apache-2.0
//
// frost::text::Editor (frost/text/editor/editor.hpp, ADR 0025): applies CommandResults to the
// Document with their undo context, keeps the DisplayMap in step and restores selections on undo.

#include "frost/text/editor/editor.hpp"

#include "frost/core/assert.hpp"

#include "command_support.hpp"

#include <utility>

namespace frost::text {

Editor::Editor(Document& document, const ITextShaper& shaper, EditorConfig config) noexcept
    : m_document(&document), m_config(config), m_display(document.text(), shaper, config.wrap),
      m_state(EditorState::caretAt(ByteOffset{0})) {}

CommandInput Editor::input() const noexcept {
    return CommandInput{m_document->text(), m_state,    m_config.settings,
                        m_config.language,  &m_display, m_config.pageRows};
}

Result<EditorUpdate> Editor::apply(const CommandResult& result) noexcept {
    EditorUpdate update;
    if (result.edits.empty()) {
        m_state = result.state;
        return update;
    }
    if (result.breakCoalescing) {
        m_document->breakCoalescing();
    }
    std::vector<Edit> edits;
    edits.reserve(result.edits.size());
    for (const TextEdit& edit : result.edits) {
        edits.push_back(Edit{edit.range, edit.text});
    }
    const EditContext context{result.kind, m_state.selections, result.state.selections};
    Result<ChangeSet> change = m_document->edit(edits, context);
    if (!change) {
        return std::unexpected(std::move(change.error()));
    }
    Result<std::vector<RowEdit>> rows = m_display.apply(*change, m_document->text());
    if (!rows) {
        return std::unexpected(std::move(rows.error()));
    }
    m_state = result.state;
    update.change = std::move(*change);
    update.rows = std::move(*rows);
    return update;
}

Result<EditorUpdate> Editor::undo() noexcept {
    Result<ChangeSet> change = m_document->undo();
    if (!change) {
        return std::unexpected(std::move(change.error()));
    }
    return finishHistory(std::move(*change));
}

Result<EditorUpdate> Editor::redo() noexcept {
    Result<ChangeSet> change = m_document->redo();
    if (!change) {
        return std::unexpected(std::move(change.error()));
    }
    return finishHistory(std::move(*change));
}

Result<EditorUpdate> Editor::follow(const ChangeSet& change) noexcept {
    Result<std::vector<RowEdit>> rows = m_display.apply(change, m_document->text());
    if (!rows) {
        return std::unexpected(std::move(rows.error()));
    }
    m_state = detail::mapState(m_state, change.edits);
    EditorUpdate update;
    update.change = change;
    update.rows = std::move(*rows);
    return update;
}

Result<EditorUpdate> Editor::finishHistory(ChangeSet change) noexcept {
    Result<std::vector<RowEdit>> rows = m_display.apply(change, m_document->text());
    if (!rows) {
        return std::unexpected(std::move(rows.error()));
    }
    if (!change.selections.empty()) {
        m_state = EditorState::from(change.selections);
    } else {
        m_state = detail::mapState(m_state, change.edits);
    }
    EditorUpdate update;
    update.change = std::move(change);
    update.rows = std::move(*rows);
    return update;
}

void Editor::setState(EditorState state) noexcept {
    FROST_PRE(state.selections.maxOffset() <= text().size());
    m_state = std::move(state);
}

std::vector<RowEdit> Editor::setWrap(WrapSettings wrap) noexcept {
    m_config.wrap = wrap;
    return m_display.setWrap(wrap);
}

std::vector<RowEdit> Editor::setShaper(const ITextShaper& shaper) noexcept { return m_display.setShaper(shaper); }

void Editor::setInlays(std::vector<Inlay> inlays) noexcept { m_display.setInlays(std::move(inlays)); }

const std::vector<Editor::Inlay>& Editor::inlays() const noexcept { return m_display.inlays(); }

} // namespace frost::text
