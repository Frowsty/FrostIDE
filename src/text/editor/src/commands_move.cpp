// SPDX-License-Identifier: Apache-2.0
//
// Caret movement and selection (frost/text/editor/commands.hpp, ADR 0025): by grapheme cluster, by
// word, smart Home and End, by display row through DisplayMap::moveVertically (by buffer line
// without a map), by page, to the text ends, select all, Go to Line and next/previous member.

#include "frost/core/assert.hpp"
#include "frost/text/editor/commands.hpp"
#include "frost/text/editor/search.hpp"
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

// Clones every caret one row up (`rows == -1`) or down (`rows == 1`).
CommandResult addCaretVertical(const CommandInput& in, std::int32_t rows) noexcept {
    const Snapshot& text = in.text;
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    if (in.display != nullptr) {
        FROST_PRE(in.display->version() == text.version());
    }
    std::vector<Selection> after;
    std::vector<CaretState> carets;
    after.reserve(selections.size() * 2);
    carets.reserve(selections.size() * 2);
    for (std::size_t i = 0; i < selections.size(); ++i) {
        after.push_back(selections[i]);
        carets.push_back(in.state.caret(i));
    }
    const std::size_t primaryIn = detail::primaryOf(in.state);
    std::size_t newPrimary = primaryIn;
    for (std::size_t i = 0; i < selections.size(); ++i) {
        const ByteOffset head = text.clip(selections[i].head);
        const CaretState caret = in.state.caret(i);
        Selection added = Selection::caret(head);
        CaretState addedCaret = caret;
        if (in.display != nullptr) {
            const VerticalMove move = in.display->moveVertically(head, caret.bias, caret.goalX, rows);
            added = Selection::caret(move.offset);
            addedCaret = CaretState{move.bias, move.goalX};
        } else {
            const auto [offset, goal] = moveByLines(text, head, caret.goalX, rows);
            added = Selection::caret(offset);
            addedCaret = CaretState{Bias::Right, goal};
        }
        if (i == primaryIn) {
            newPrimary = after.size();
        }
        after.push_back(added);
        carets.push_back(addedCaret);
    }
    CommandResult result;
    result.state = detail::makeState(after, carets, newPrimary);
    return result;
}

// The word (CharClass::Word run) containing or adjacent to `at`, or an empty range when none.
ByteRange wordRangeAt(const Snapshot& text, ByteOffset at) noexcept {
    at = text.clip(at);
    const std::optional<DecodedChar> right = charAt(text, at);
    const std::optional<DecodedChar> left = charBefore(text, at);
    const bool rightWord = right && charClass(right->cp) == CharClass::Word;
    const bool leftWord = left && charClass(left->cp) == CharClass::Word;
    if (!rightWord && !leftWord) {
        return ByteRange{at, at};
    }
    ByteOffset begin = at;
    {
        detail::BackwardChars back(text, at);
        ByteOffset where;
        DecodedChar c;
        while (back.next(where, c) && charClass(c.cp) == CharClass::Word) {
            begin = where;
            if (where == ByteOffset{0}) {
                break;
            }
        }
    }
    ByteOffset end = at;
    {
        detail::ForwardChars fwd(text, at);
        ByteOffset where;
        DecodedChar c;
        while (fwd.next(where, c) && charClass(c.cp) == CharClass::Word) {
            end = where + c.bytes;
        }
    }
    return ByteRange{begin, end};
}

// Non-whitespace token around `at` on its line when `at` is not on a word character.
ByteRange tokenRangeAt(const Snapshot& text, ByteOffset at) noexcept {
    const ByteRange word = wordRangeAt(text, at);
    if (!word.empty()) {
        return word;
    }
    at = text.clip(at);
    const LineInfo line = detail::lineOf(text, at);
    const auto isTokenChar = [](char32_t cp) noexcept {
        const CharClass cls = charClass(cp);
        return cls != CharClass::Space && cls != CharClass::LineBreak;
    };
    const std::optional<DecodedChar> right = charAt(text, at);
    const std::optional<DecodedChar> left = charBefore(text, at);
    const bool rightTok = right && at < line.content.end && isTokenChar(right->cp);
    const bool leftTok = left && at > line.content.begin && isTokenChar(left->cp);
    if (!rightTok && !leftTok) {
        return ByteRange{at, at};
    }
    ByteOffset begin = at;
    {
        detail::BackwardChars back(text, at);
        ByteOffset where;
        DecodedChar c;
        while (back.next(where, c) && where >= line.content.begin && isTokenChar(c.cp)) {
            begin = where;
            if (where == line.content.begin) {
                break;
            }
        }
    }
    ByteOffset end = at;
    {
        detail::ForwardChars fwd(text, at);
        ByteOffset where;
        DecodedChar c;
        while (fwd.next(where, c) && where < line.content.end && isTokenChar(c.cp)) {
            end = where + c.bytes;
        }
    }
    return ByteRange{begin, end};
}

bool strictlyContains(ByteRange outer, ByteRange inner) noexcept {
    return outer.begin <= inner.begin && outer.end >= inner.end && (outer.begin < inner.begin || outer.end > inner.end);
}

char matchingCloser(char opener) noexcept {
    switch (opener) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    default: return 0;
    }
}

char matchingOpener(char closer) noexcept {
    switch (closer) {
    case ')': return '(';
    case ']': return '[';
    case '}': return '{';
    default: return 0;
    }
}

bool isDelimiterPair(char open, char close) noexcept {
    return (open == '(' && close == ')') || (open == '[' && close == ']') || (open == '{' && close == '}') ||
           (open == '"' && close == '"') || (open == '\'' && close == '\'') || (open == '`' && close == '`');
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

CommandResult addCaretAbove(const CommandInput& in) noexcept { return addCaretVertical(in, -1); }

CommandResult addCaretBelow(const CommandInput& in) noexcept { return addCaretVertical(in, 1); }

ByteRange extendSelectionRange(const Snapshot& text, ByteRange cur, std::span<const ByteRange> astRanges) noexcept {
    cur.begin = text.clip(cur.begin);
    cur.end = std::max(cur.begin, text.clip(cur.end));
    const ByteRange fullDoc{ByteOffset{0}, text.size()};
    if (cur == fullDoc || text.size() == ByteOffset{0}) {
        return cur;
    }

    std::vector<ByteRange> candidates;
    const auto addCandidate = [&](ByteRange range) {
        range.begin = text.clip(range.begin);
        range.end = text.clip(range.end);
        if (range.begin <= range.end && strictlyContains(range, cur)) {
            candidates.push_back(range);
        }
    };

    // 1. Word or symbol run around `cur`.
    {
        const LineInfo startLine = detail::lineOf(text, cur.begin);
        if (cur.end <= startLine.content.end) {
            bool allWord = true;
            detail::ForwardChars chars(text, cur.begin);
            ByteOffset where;
            DecodedChar c;
            while (chars.next(where, c) && where < cur.end) {
                if (charClass(c.cp) != CharClass::Word) {
                    allWord = false;
                    break;
                }
            }
            if (allWord) {
                ByteOffset wBegin = cur.begin;
                {
                    detail::BackwardChars back(text, cur.begin);
                    while (back.next(where, c) && charClass(c.cp) == CharClass::Word) {
                        wBegin = where;
                        if (where == ByteOffset{0}) {
                            break;
                        }
                    }
                }
                ByteOffset wEnd = cur.end;
                {
                    detail::ForwardChars fwd(text, cur.end);
                    while (fwd.next(where, c) && charClass(c.cp) == CharClass::Word) {
                        wEnd = where + c.bytes;
                    }
                }
                if (wBegin < wEnd) {
                    addCandidate(ByteRange{wBegin, wEnd});
                }
            }
            if (cur.empty()) {
                addCandidate(wordRangeAt(text, cur.begin));
                addCandidate(tokenRangeAt(text, cur.begin));
            }
        }
    }

    // 2. Enclosing quotes on the same line (`"..."`, `'...'`, `` `...` ``).
    {
        const LineInfo line = detail::lineOf(text, cur.begin);
        if (cur.end <= line.content.end) {
            const std::string lineStr = text.text(line.content);
            for (const char q : {'"', '\'', '`'}) {
                std::vector<std::size_t> positions;
                for (std::size_t i = 0; i < lineStr.size(); ++i) {
                    if (lineStr[i] == '\\' && i + 1 < lineStr.size()) {
                        ++i;
                        continue;
                    }
                    if (lineStr[i] == q) {
                        positions.push_back(i);
                    }
                }
                for (std::size_t j = 0; j + 1 < positions.size(); j += 2) {
                    const ByteOffset openPos = line.content.begin + positions[j];
                    const ByteOffset closePos = line.content.begin + positions[j + 1];
                    addCandidate(ByteRange{openPos + 1, closePos});
                    addCandidate(ByteRange{openPos, closePos + 1});
                }
            }
        }
    }

    // 3. Balanced brackets `(...)`, `[...]`, `{...}` in a window around `cur`.
    {
        constexpr std::uint64_t kWindow = 16384;
        const ByteOffset winStart{cur.begin.value() > kWindow ? cur.begin.value() - kWindow : 0};
        const ByteOffset winEnd = text.clip(ByteOffset{cur.end.value() + kWindow});
        const std::string window = text.text(ByteRange{winStart, winEnd});
        const std::size_t relBegin = static_cast<std::size_t>(cur.begin.value() - winStart.value());
        const std::size_t relEnd = static_cast<std::size_t>(cur.end.value() - winStart.value());

        // If `cur` is already the interior of a pair, add the exterior pair immediately.
        if (relBegin > 0 && relEnd < window.size()) {
            const char open = window[relBegin - 1];
            const char close = window[relEnd];
            if (isDelimiterPair(open, close)) {
                addCandidate(ByteRange{cur.begin - 1, cur.end + 1});
            }
        }

        // Scan backward from `relBegin` for unmatched openers, then forward from `relEnd` for their closer.
        std::vector<char> closeStack;
        for (std::size_t i = relBegin; i > 0;) {
            --i;
            const char ch = window[i];
            if (const char opener = matchingOpener(ch); opener != 0) {
                closeStack.push_back(opener);
            } else if (const char closer = matchingCloser(ch); closer != 0) {
                if (!closeStack.empty() && closeStack.back() == ch) {
                    closeStack.pop_back();
                } else {
                    // Unmatched opener at `i`; scan forward from `i + 1` to find its matching closer.
                    std::vector<char> fwdStack;
                    fwdStack.push_back(closer);
                    for (std::size_t j = i + 1; j < window.size(); ++j) {
                        const char fc = window[j];
                        if (const char fCloser = matchingCloser(fc); fCloser != 0) {
                            fwdStack.push_back(fCloser);
                        } else if (matchingOpener(fc) != 0) {
                            if (!fwdStack.empty() && fwdStack.back() == fc) {
                                fwdStack.pop_back();
                                if (fwdStack.empty()) {
                                    if (j >= relEnd) {
                                        const ByteOffset openOff = winStart + i;
                                        const ByteOffset closeOff = winStart + j;
                                        addCandidate(ByteRange{openOff + 1, closeOff});
                                        addCandidate(ByteRange{openOff, closeOff + 1});
                                    }
                                    break;
                                }
                            } else {
                                break;
                            }
                        }
                    }
                }
            }
        }
    }

    // 4. Line candidates (trimmed line, full line content, full line with break).
    {
        const LineInfo line = detail::lineOf(text, cur.begin);
        const ByteOffset fullEnd = detail::lineEndWithBreak(line);
        if (cur.end <= fullEnd) {
            const ByteOffset indent = detail::indentEnd(text, line);
            const ByteOffset trimmed = detail::trimmedEnd(text, line);
            if (indent < trimmed) {
                addCandidate(ByteRange{indent, trimmed});
            }
            addCandidate(line.content);
            addCandidate(ByteRange{line.content.begin, fullEnd});
        }
    }

    // 5. Tree-sitter AST ranges (and interior of delimited AST nodes).
    for (const ByteRange& ast : astRanges) {
        addCandidate(ast);
        if (ast.size() >= 2 && ast.end <= text.size()) {
            const std::string openByte = text.text(ByteRange{ast.begin, ast.begin + 1});
            const std::string closeByte = text.text(ByteRange{ast.end - 1, ast.end});
            if (!openByte.empty() && !closeByte.empty() && isDelimiterPair(openByte[0], closeByte[0])) {
                addCandidate(ByteRange{ast.begin + 1, ast.end - 1});
            }
        }
    }

    // 6. Full document.
    addCandidate(fullDoc);

    if (candidates.empty()) {
        return cur;
    }
    return *std::min_element(candidates.begin(), candidates.end(), [](const ByteRange& a, const ByteRange& b) {
        if (a.size() != b.size()) {
            return a.size() < b.size();
        }
        if (a.begin != b.begin) {
            return a.begin > b.begin;
        }
        return a.end < b.end;
    });
}

CommandResult extendSelection(const CommandInput& in, std::span<const ByteRange> astRanges) noexcept {
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    std::vector<Selection> after;
    after.reserve(selections.size());
    for (std::size_t i = 0; i < selections.size(); ++i) {
        const std::span<const ByteRange> ast =
            (i == detail::primaryOf(in.state)) ? astRanges : std::span<const ByteRange>{};
        const ByteRange extended = extendSelectionRange(in.text, selections[i].range(), ast);
        after.push_back(Selection{extended.begin, extended.end});
    }
    CommandResult result;
    result.state = detail::makeState(after, {}, detail::primaryOf(in.state));
    return result;
}

CommandResult selectNextOccurrence(const CommandInput& in) noexcept {
    const Snapshot& text = in.text;
    std::vector<Selection> selections = detail::selectionsOf(in.state);
    const std::size_t primaryIdx = detail::primaryOf(in.state);
    const Selection primary = selections[primaryIdx];

    if (primary.empty()) {
        const ByteRange token = tokenRangeAt(text, primary.head);
        if (token.empty()) {
            return CommandResult{{}, in.state, EditKind::Other, false};
        }
        selections[primaryIdx] = Selection{token.begin, token.end};
        CommandResult result;
        result.state = detail::makeState(selections, in.state.carets, primaryIdx);
        return result;
    }

    const std::string needle = text.text(primary.range());
    if (needle.empty()) {
        return CommandResult{{}, in.state, EditKind::Other, false};
    }
    const Result<SearchPattern> pattern = SearchPattern::compile(needle, SearchOptions{.caseSensitive = true});
    if (!pattern) {
        return CommandResult{{}, in.state, EditKind::Other, false};
    }

    const auto isSelected = [&](ByteRange range) noexcept {
        for (const Selection& sel : selections) {
            if (sel.range() == range) {
                return true;
            }
        }
        return false;
    };

    ByteOffset searchFrom = primary.range().end;
    bool wrapped = false;
    const std::size_t maxAttempts = selections.size() + 3;
    for (std::size_t attempt = 0; attempt < maxAttempts; ++attempt) {
        std::optional<ByteRange> match = pattern->findNext(text, searchFrom);
        if (!match && !wrapped && searchFrom > ByteOffset{0}) {
            wrapped = true;
            searchFrom = ByteOffset{0};
            match = pattern->findNext(text, searchFrom);
        }
        if (!match) {
            break;
        }
        if (!isSelected(*match)) {
            const std::size_t addedIdx = selections.size();
            selections.push_back(Selection{match->begin, match->end});
            CommandResult result;
            result.state = detail::makeState(selections, {}, addedIdx);
            return result;
        }
        searchFrom = match->end > searchFrom ? match->end : text.clip(searchFrom + 1);
        if (searchFrom >= text.size()) {
            if (wrapped) {
                break;
            }
            wrapped = true;
            searchFrom = ByteOffset{0};
        }
    }

    return CommandResult{{}, in.state, EditKind::Other, false};
}

CommandResult unselectOccurrence(const CommandInput& in) noexcept {
    std::vector<Selection> selections = detail::selectionsOf(in.state);
    const std::size_t primaryIdx = detail::primaryOf(in.state);
    if (selections.size() > 1) {
        selections.erase(selections.begin() + static_cast<std::ptrdiff_t>(primaryIdx));
        const std::size_t nextPrimary = primaryIdx > 0 ? primaryIdx - 1 : selections.size() - 1;
        CommandResult result;
        result.state = detail::makeState(selections, {}, nextPrimary);
        return result;
    }
    if (!selections[0].empty()) {
        const Selection collapsed = Selection::caret(selections[0].head);
        CommandResult result;
        result.state = detail::makeState(std::span<const Selection>(&collapsed, 1), {}, 0);
        return result;
    }
    return CommandResult{{}, in.state, EditKind::Other, false};
}

CommandResult selectAllOccurrences(const CommandInput& in) noexcept {
    const Snapshot& text = in.text;
    const Selection primary = in.state.primary();
    ByteRange targetRange = primary.range();
    bool wholeWords = false;
    if (targetRange.empty()) {
        targetRange = wordRangeAt(text, primary.head);
        if (!targetRange.empty()) {
            wholeWords = true;
        } else {
            targetRange = tokenRangeAt(text, primary.head);
        }
    }
    if (targetRange.empty()) {
        return CommandResult{{}, in.state, EditKind::Other, false};
    }
    const std::string needle = text.text(targetRange);
    if (needle.empty()) {
        return CommandResult{{}, in.state, EditKind::Other, false};
    }
    const Result<SearchPattern> pattern =
        SearchPattern::compile(needle, SearchOptions{.caseSensitive = true, .wholeWord = wholeWords});
    if (!pattern) {
        return CommandResult{{}, in.state, EditKind::Other, false};
    }
    const std::vector<ByteRange> matches = pattern->findAll(text);
    if (matches.empty()) {
        return CommandResult{{}, in.state, EditKind::Other, false};
    }
    std::vector<Selection> after;
    after.reserve(matches.size());
    std::size_t primaryIdx = 0;
    for (std::size_t i = 0; i < matches.size(); ++i) {
        if (matches[i] == targetRange) {
            primaryIdx = i;
        }
        after.push_back(Selection{matches[i].begin, matches[i].end});
    }
    CommandResult result;
    result.state = detail::makeState(after, {}, primaryIdx);
    return result;
}

} // namespace commands

} // namespace frost::text
