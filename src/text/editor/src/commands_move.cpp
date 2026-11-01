// SPDX-License-Identifier: Apache-2.0
//
// Caret movement and selection (frost/text/editor/commands.hpp, ADR 0025): by grapheme cluster, by
// word, smart Home and End, by display row through DisplayMap::moveVertically (by buffer line
// without a map), by page, to the text ends, select all, Go to Line and next/previous member.

#include "frost/core/assert.hpp"
#include "frost/text/editor/commands.hpp"
#include "frost/text/editor/unicode.hpp"

#include "char_cursor.hpp"
#include "command_support.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <limits>
#include <system_error>
#include <vector>

namespace frost::text {

namespace {

// Where a caret moves without a display map: the same code point column `goal` on the line `rows`
// lines away (clamped to the text), and that column as the new goal.
std::pair<ByteOffset, TextWidth> moveByLines(const Snapshot& text, ByteOffset from, std::optional<TextWidth> goalX,
                                             std::int64_t rows) noexcept {
    const LineInfo line = detail::lineOf(text, from);
    TextWidth goal = 0;
    if (goalX) {
        goal = *goalX;
    } else {
        detail::ForwardChars chars(text, line.content.begin);
        ByteOffset at;
        DecodedChar c;
        while (chars.next(at, c) && at < from) {
            ++goal;
        }
    }
    const std::int64_t last = static_cast<std::int64_t>(text.lineCount()) - 1;
    const std::int64_t target = std::clamp<std::int64_t>(static_cast<std::int64_t>(line.line.value()) + rows, 0, last);
    if (target == static_cast<std::int64_t>(line.line.value())) {
        return {from, goal};
    }
    const LineInfo to = detail::lineAt(text, static_cast<std::uint32_t>(target));
    ByteOffset offset = to.content.begin;
    detail::ForwardChars chars(text, to.content.begin);
    ByteOffset at;
    DecodedChar c;
    for (TextWidth i = 0; i < goal && chars.next(at, c) && at < to.content.end; ++i) {
        offset = at + c.bytes;
    }
    return {offset, goal};
}

// Home: the indentation end, or column 0 when the caret is already there (smart home).
ByteOffset lineStartTarget(const Snapshot& text, ByteOffset head, bool smartHome) noexcept {
    const LineInfo line = detail::lineOf(text, head);
    if (!smartHome) {
        return line.content.begin;
    }
    const ByteOffset indent = detail::indentEnd(text, line);
    return head == indent ? line.content.begin : indent;
}

// The result of moving every selection: `heads[i]` and `carets[i]` for selection i.
CommandResult moved(const CommandInput& in, const std::vector<Selection>& selections,
                    const std::vector<ByteOffset>& heads, const std::vector<CaretState>& carets, bool extend) {
    std::vector<Selection> after;
    after.reserve(selections.size());
    for (std::size_t i = 0; i < selections.size(); ++i) {
        after.push_back(extend ? Selection{selections[i].anchor, heads[i]} : Selection::caret(heads[i]));
    }
    CommandResult result;
    result.state = detail::makeState(after, carets, detail::primaryOf(in.state));
    return result;
}

} // namespace

Result<Point> parseGoToLine(std::string_view input) noexcept {
    const auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
            s.remove_prefix(1);
        }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
            s.remove_suffix(1);
        }
        return s;
    };
    const auto number = [](std::string_view s) -> std::optional<std::uint32_t> {
        std::uint64_t value = 0;
        const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (s.empty() || error != std::errc{} || end != s.data() + s.size() || value == 0 ||
            value > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(value);
    };
    input = trim(input);
    const std::size_t separator = input.find_first_of(":,");
    const std::optional<std::uint32_t> line = number(trim(input.substr(0, separator)));
    std::optional<std::uint32_t> column = 1;
    if (separator != std::string_view::npos) {
        column = number(trim(input.substr(separator + 1)));
    }
    if (!line || !column) {
        return fail(ErrorCode::InvalidArgument, std::format("'{}' is not a line or line:column", input));
    }
    return Point{LineIndex{*line - 1}, ByteColumn{*column - 1}};
}

namespace commands {

CommandResult move(const CommandInput& in, Movement movement, bool extend) noexcept {
    const Snapshot& text = in.text;
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    std::vector<ByteOffset> heads;
    std::vector<CaretState> carets;
    heads.reserve(selections.size());
    carets.reserve(selections.size());
    const std::int64_t page = std::max<std::uint32_t>(in.pageRows, 1);
    if (in.display != nullptr) {
        FROST_PRE(in.display->version() == text.version());
    }
    for (std::size_t i = 0; i < selections.size(); ++i) {
        const Selection& selection = selections[i];
        const CaretState caret = in.state.caret(i);
        const ByteOffset head = text.clip(selection.head);
        std::int64_t rows = 0;
        switch (movement) {
        case Movement::LineUp: rows = -1; break;
        case Movement::LineDown: rows = 1; break;
        case Movement::PageUp: rows = -page; break;
        case Movement::PageDown: rows = page; break;
        default: break;
        }
        if (rows != 0) {
            if (in.display != nullptr) {
                const VerticalMove move =
                    in.display->moveVertically(head, caret.bias, caret.goalX, static_cast<std::int32_t>(rows));
                heads.push_back(move.offset);
                carets.push_back(CaretState{move.bias, move.goalX});
            } else {
                const auto [offset, goal] = moveByLines(text, head, caret.goalX, rows);
                heads.push_back(offset);
                carets.push_back(CaretState{Bias::Right, goal});
            }
            continue;
        }
        ByteOffset target = head;
        switch (movement) {
        case Movement::CharLeft:
            target = !extend && !selection.empty() ? selection.range().begin : previousGraphemeBoundary(text, head);
            break;
        case Movement::CharRight:
            target = !extend && !selection.empty() ? selection.range().end : nextGraphemeBoundary(text, head);
            break;
        case Movement::WordLeft: target = previousWordBoundary(text, head); break;
        case Movement::WordRight: target = nextWordBoundary(text, head); break;
        case Movement::LineStart: target = lineStartTarget(text, head, in.settings.smartHome); break;
        case Movement::LineEnd: target = detail::lineOf(text, head).content.end; break;
        case Movement::TextStart: target = ByteOffset{0}; break;
        case Movement::TextEnd: target = text.size(); break;
        default: break;
        }
        heads.push_back(target);
        carets.push_back(CaretState{});
    }
    return moved(in, selections, heads, carets, extend);
}

CommandResult selectAll(const CommandInput& in) noexcept {
    const Selection all{ByteOffset{0}, in.text.size()};
    CommandResult result;
    result.state = detail::makeState(std::span<const Selection>(&all, 1), {}, 0);
    return result;
}

CommandResult goToLine(const CommandInput& in, Point target) noexcept {
    const Snapshot& text = in.text;
    if (target.line.value() >= text.lineCount()) {
        target.line = LineIndex{text.lineCount() - 1};
    }
    CommandResult result;
    result.state = EditorState::caretAt(*text.offsetAt(target));
    return result;
}

CommandResult nextMember(const CommandInput& in, const IOutlineProvider& outline, bool extend) noexcept {
    const std::vector<ByteRange> members = outline.members(in.text);
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    std::vector<ByteOffset> heads;
    for (const Selection& selection : selections) {
        std::optional<ByteOffset> best;
        for (const ByteRange& member : members) {
            if (member.begin > selection.head && (!best || member.begin < *best)) {
                best = member.begin;
            }
        }
        heads.push_back(best.value_or(selection.head));
    }
    return moved(in, selections, heads, {}, extend);
}

CommandResult previousMember(const CommandInput& in, const IOutlineProvider& outline, bool extend) noexcept {
    const std::vector<ByteRange> members = outline.members(in.text);
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    std::vector<ByteOffset> heads;
    for (const Selection& selection : selections) {
        std::optional<ByteOffset> best;
        for (const ByteRange& member : members) {
            if (member.begin < selection.head && (!best || member.begin > *best)) {
                best = member.begin;
            }
        }
        heads.push_back(best.value_or(selection.head));
    }
    return moved(in, selections, heads, {}, extend);
}

} // namespace commands

} // namespace frost::text
