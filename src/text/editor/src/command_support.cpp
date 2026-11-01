// SPDX-License-Identifier: Apache-2.0
//
// The editor commands' shared helpers (command_support.hpp, ADR 0025): the batch builder, state
// construction and line queries, plus EditorState's members (commands.hpp).

#include "command_support.hpp"

#include "frost/core/assert.hpp"
#include "frost/text/editor/commands.hpp"

#include "char_cursor.hpp"

#include <algorithm>
#include <numeric>
#include <tuple>
#include <utility>

namespace frost::text {

Selection EditorState::primary() const noexcept {
    if (selections.empty()) {
        return Selection::caret(ByteOffset{0});
    }
    return selections.all()[selections.primaryIndex()];
}

EditorState EditorState::caretAt(ByteOffset at) noexcept { return EditorState{SelectionSet::caret(at), {}}; }

EditorState EditorState::from(SelectionSet selections) noexcept { return EditorState{std::move(selections), {}}; }

bool operator==(const EditorState& a, const EditorState& b) noexcept {
    if (!(a.selections == b.selections)) {
        return false;
    }
    for (std::size_t i = 0; i < a.selections.size(); ++i) {
        if (a.caret(i) != b.caret(i)) {
            return false;
        }
    }
    return true;
}

namespace detail {

std::size_t BatchBuilder::add(ByteRange range, std::string text) {
    FROST_PRE(range.begin <= range.end);
    m_edits.push_back(TextEdit{range, std::move(text)});
    return m_edits.size() - 1;
}

void BatchBuilder::finish() noexcept {
    const std::size_t n = m_edits.size();
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
        return std::tie(m_edits[x].range.begin, m_edits[x].range.end) <
               std::tie(m_edits[y].range.begin, m_edits[y].range.end);
    });
    std::vector<TextEdit> kept;
    kept.reserve(n);
    m_slot.assign(n, kDropped);
    m_ranges.resize(n);
    for (const std::size_t id : order) {
        TextEdit& edit = m_edits[id];
        m_ranges[id] = edit.range;
        if (!kept.empty()) {
            const TextEdit& previous = kept.back();
            if (previous.range == edit.range && previous.text == edit.text && !edit.range.empty()) {
                m_slot[id] = kept.size() - 1; // The same change asked twice.
                continue;
            }
            // Sorted, so an overlap means the edit starts before the previous one ends; an insert
            // at the begin of a non-empty range cannot follow it (it sorts first).
            if (edit.range.begin < previous.range.end) {
                continue;
            }
        }
        m_slot[id] = kept.size();
        kept.push_back(std::move(edit));
    }
    m_edits = std::move(kept);
    m_shift.assign(m_edits.size(), 0);
    m_raw.clear();
    std::int64_t shift = 0;
    for (std::size_t i = 0; i < m_edits.size(); ++i) {
        const TextEdit& edit = m_edits[i];
        m_shift[i] = shift;
        shift += static_cast<std::int64_t>(edit.text.size()) - static_cast<std::int64_t>(edit.range.size());
        m_raw.push_back(RawEdit{edit.range, edit.text.size()});
    }
}

ByteOffset BatchBuilder::post(std::size_t id, std::uint64_t k) const noexcept {
    const std::size_t i = m_slot[id];
    if (i == kDropped) {
        return map(m_ranges[id].begin, Bias::Left);
    }
    FROST_PRE(k <= m_edits[i].text.size());
    const auto begin = static_cast<std::int64_t>(m_edits[i].range.begin.value());
    return ByteOffset{static_cast<std::uint64_t>(begin + m_shift[i]) + k};
}

ByteOffset BatchBuilder::map(ByteOffset offset, Bias bias) const noexcept { return mapOffset(offset, bias, m_raw); }

std::vector<TextEdit> BatchBuilder::take() noexcept {
    std::vector<TextEdit> out;
    out.reserve(m_edits.size());
    for (TextEdit& edit : m_edits) {
        if (!(edit.range.empty() && edit.text.empty())) {
            out.push_back(std::move(edit));
        }
    }
    m_edits.clear();
    return out;
}

EditorState makeState(std::span<const Selection> selections, std::span<const CaretState> carets,
                      std::size_t primary) noexcept {
    std::vector<std::uint32_t> origins;
    EditorState state;
    state.selections = SelectionSet::fromSelections(selections, primary, &origins);
    state.carets.reserve(origins.size());
    for (const std::uint32_t origin : origins) {
        state.carets.push_back(origin < carets.size() ? carets[origin] : CaretState{});
    }
    return state;
}

EditorState mapState(const EditorState& state, std::span<const RawEdit> raw) noexcept {
    std::vector<std::uint32_t> origins;
    EditorState out;
    out.selections = state.selections.transformed(raw, &origins);
    out.carets.reserve(origins.size());
    for (const std::uint32_t origin : origins) {
        out.carets.push_back(CaretState{state.caret(origin).bias, std::nullopt});
    }
    return out;
}

std::vector<Selection> selectionsOf(const EditorState& state) noexcept {
    if (state.selections.empty()) {
        return {Selection::caret(ByteOffset{0})};
    }
    const std::span<const Selection> all = state.selections.all();
    return std::vector<Selection>(all.begin(), all.end());
}

std::size_t primaryOf(const EditorState& state) noexcept {
    return state.selections.empty() ? 0 : state.selections.primaryIndex();
}

LineInfo lineOf(const Snapshot& text, ByteOffset at) noexcept { return *text.lineInfo(text.lineAt(at)); }

LineInfo lineAt(const Snapshot& text, std::uint32_t line) noexcept {
    FROST_PRE(line < text.lineCount());
    return *text.lineInfo(LineIndex{line});
}

ByteOffset lineEndWithBreak(const LineInfo& line) noexcept {
    switch (line.lineBreak) {
    case LineBreak::None: return line.content.end;
    case LineBreak::Lf:
    case LineBreak::Cr: return line.content.end + 1;
    case LineBreak::CrLf: return line.content.end + 2;
    }
    return line.content.end;
}

std::string_view breakText(LineBreak lineBreak) noexcept {
    switch (lineBreak) {
    case LineBreak::None: return "";
    case LineBreak::Lf: return "\n";
    case LineBreak::Cr: return "\r";
    case LineBreak::CrLf: return "\r\n";
    }
    return "\n";
}

std::string_view documentBreak(const Snapshot& text, LineBreak fallback) noexcept {
    const LineInfo first = lineAt(text, 0);
    if (first.lineBreak != LineBreak::None) {
        return breakText(first.lineBreak);
    }
    return fallback == LineBreak::None ? "\n" : breakText(fallback);
}

ByteOffset indentEnd(const Snapshot& text, const LineInfo& line) noexcept {
    ByteOffset at = line.content.begin;
    if (line.content.empty()) {
        return at;
    }
    for (const std::string_view chunk : text.chunks(line.content)) {
        for (const char c : chunk) {
            if (c != ' ' && c != '\t') {
                return at;
            }
            ++at;
        }
    }
    return at;
}

ByteOffset trimmedEnd(const Snapshot& text, const LineInfo& line) noexcept {
    if (line.content.empty()) {
        return line.content.end;
    }
    BackwardChars chars(text, line.content.end);
    ByteOffset at;
    DecodedChar c;
    while (chars.next(at, c) && at >= line.content.begin) {
        if (c.cp != U' ' && c.cp != U'\t') {
            return at + c.bytes;
        }
        if (at == line.content.begin) {
            break;
        }
    }
    return line.content.end; // Blank: the whole line is indentation.
}

std::uint64_t visualColumn(const Snapshot& text, const LineInfo& line, ByteOffset at, std::uint32_t tabSize) noexcept {
    const std::uint64_t tab = std::max<std::uint32_t>(tabSize, 1);
    std::uint64_t column = 0;
    ForwardChars chars(text, line.content.begin);
    ByteOffset where;
    DecodedChar c;
    while (chars.next(where, c) && where < at) {
        column = c.cp == U'\t' ? (column / tab + 1) * tab : column + 1;
    }
    return column;
}

LineSpan touchedLines(const Snapshot& text, const Selection& selection) noexcept {
    const ByteRange range = selection.range();
    const Point first = text.pointAt(range.begin);
    const Point last = text.pointAt(range.end);
    LineSpan span{first.line.value(), last.line.value()};
    if (!range.empty() && last.column == ByteColumn{0} && last.line > first.line) {
        --span.last;
    }
    return span;
}

std::vector<LineSpan> touchedSpans(const Snapshot& text, std::span<const Selection> selections,
                                   bool mergeAdjacent) noexcept {
    std::vector<LineSpan> spans;
    for (const Selection& selection : selections) {
        const LineSpan span = touchedLines(text, selection);
        if (!spans.empty()) {
            LineSpan& back = spans.back();
            const bool merge = mergeAdjacent ? span.first <= back.last + 1 : span.first <= back.last;
            if (merge) {
                back.last = std::max(back.last, span.last);
                continue;
            }
        }
        spans.push_back(span);
    }
    return spans;
}

} // namespace detail

} // namespace frost::text
