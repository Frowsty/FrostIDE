// SPDX-License-Identifier: Apache-2.0
//
// The UX-3 editor essentials (frost/text/editor/essentials.hpp): bracket and code-block queries,
// the Move Statement plan over the syntax seam, Split Line, Start New Line Before, overwrite
// typing, line-block swaps and Auto-Indent Lines.

#include "frost/core/assert.hpp"
#include "frost/text/editor/essentials.hpp"
#include "frost/text/editor/unicode.hpp"

#include "command_support.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace frost::text {

namespace {

using detail::BatchBuilder;

// How far the statement searches look (planStatementMove's contract).
constexpr std::uint32_t kStatementSearchLines = 2048;
// Auto-indent's backward searches for a reference or opener line.
constexpr std::uint32_t kIndentSearchLines = 2048;
// Lines longer than this are not lexed for brackets (they count as neutral).
constexpr std::size_t kIndentLexLimit = 64 * 1024;

[[nodiscard]] constexpr bool isOpener(char c) noexcept { return c == '(' || c == '[' || c == '{'; }
[[nodiscard]] constexpr bool isCloser(char c) noexcept { return c == ')' || c == ']' || c == '}'; }
[[nodiscard]] constexpr char partnerOf(char c) noexcept {
    switch (c) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    default: return 0;
    }
}

[[nodiscard]] ByteOffset before(ByteOffset at) noexcept { return ByteOffset{at.value() - 1}; }

// The partner of the bracket `open` at `at`, scanning forward with a nesting count of that kind.
std::optional<ByteOffset> scanForward(const Snapshot& text, ByteOffset from, char open, char close,
                                      int depth) noexcept {
    const ByteOffset end{std::min<std::uint64_t>(text.size().value(), from.value() + kBracketScanLimit)};
    std::uint64_t at = from.value();
    for (const std::string_view chunk : text.chunks(ByteRange{from, end})) {
        for (const char c : chunk) {
            if (c == open) {
                ++depth;
            } else if (c == close && --depth == 0) {
                return ByteOffset{at};
            }
            ++at;
        }
    }
    return std::nullopt;
}

// The partner of a closer, scanning backward from `from` (exclusive) with a nesting count.
std::optional<ByteOffset> scanBackward(const Snapshot& text, ByteOffset from, char open, char close,
                                       int depth) noexcept {
    const ByteOffset begin{from.value() > kBracketScanLimit ? from.value() - kBracketScanLimit : 0};
    const ReverseChunkRange chunks = text.reversedChunks(ByteRange{begin, from});
    for (auto it = chunks.begin(); it != chunks.end(); ++it) {
        const std::string_view chunk = *it;
        for (std::size_t i = chunk.size(); i-- > 0;) {
            const char c = chunk[i];
            if (c == close) {
                ++depth;
            } else if (c == open && --depth == 0) {
                return it.offset() + i;
            }
        }
    }
    return std::nullopt;
}

// The pair of the bracket at `at` (FROST_PRE: a bracket character).
std::optional<BracketPair> pairOf(const Snapshot& text, ByteOffset at, const ISyntaxRanges* syntax) noexcept {
    const char c = text.byteAt(at);
    const char other = partnerOf(c);
    const bool opener = isOpener(c);
    if (syntax != nullptr) {
        for (const ByteRange& range : syntax->enclosing(ByteRange{at, at + 1})) {
            if (range.size() < 2) {
                continue;
            }
            const ByteOffset last = before(range.end);
            if (opener && range.begin == at && text.byteAt(last) == other) {
                return BracketPair{at, last};
            }
            if (!opener && last == at && text.byteAt(range.begin) == other) {
                return BracketPair{range.begin, at};
            }
        }
    }
    if (opener) {
        if (const auto close = scanForward(text, at + 1, c, other, 1)) {
            return BracketPair{at, *close};
        }
        return std::nullopt;
    }
    if (const auto open = scanBackward(text, at, other, c, 1)) {
        return BracketPair{*open, at};
    }
    return std::nullopt;
}

// --- lines ---

[[nodiscard]] bool isBlank(const Snapshot& text, const LineInfo& line) noexcept {
    return detail::indentEnd(text, line) == line.content.end;
}

[[nodiscard]] std::uint32_t lineIndexOf(const Snapshot& text, ByteOffset at) noexcept {
    return text.lineAt(at).value();
}

// The line of a non-empty range's last byte.
[[nodiscard]] std::uint32_t lastLineOf(const Snapshot& text, ByteRange range) noexcept {
    return lineIndexOf(text, range.empty() ? range.begin : before(range.end));
}

// The statement that starts at `line`'s first non-blank character (planStatementMove's rule).
std::optional<ByteRange> statementAt(const Snapshot& text, const ISyntaxRanges& syntax, std::uint32_t line) {
    const LineInfo info = detail::lineAt(text, line);
    const ByteOffset start = detail::indentEnd(text, info);
    if (start == info.content.end) {
        return std::nullopt;
    }
    std::vector<ByteRange> ranges = syntax.enclosing(ByteRange{start, start + 1});
    if (!ranges.empty()) {
        ranges.pop_back(); // The root is never a statement.
    }
    std::optional<ByteRange> best;
    std::size_t i = 0;
    for (; i < ranges.size() && ranges[i].begin == start; ++i) {
        best = ranges[i];
    }
    if (!best) {
        return std::nullopt;
    }
    // An enclosing node that ends where the statement ends and starts a line of its own (an
    // Allman block's `if`, a template's declaration) is the statement.
    for (; i < ranges.size() && ranges[i].end == best->end; ++i) {
        const LineInfo owner = detail::lineOf(text, ranges[i].begin);
        if (detail::indentEnd(text, owner) != ranges[i].begin) {
            break;
        }
        best = ranges[i];
    }
    return best;
}

// --- auto-indent lexing ---

enum class Token : std::uint8_t { Open, Close };

struct LineShape {
    int unmatchedOpens = 0;  // Openers this line leaves open.
    int unmatchedCloses = 0; // Closers of openers above this line.
    bool startsWithCloser = false;
    bool preprocessor = false; // C family: a '#' line, left alone.
    bool commentStar = false;  // C family: a block comment's "*" continuation line.
};

[[nodiscard]] bool isWordChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// The tokens of one line's content, in order.
std::vector<Token> lexLine(std::string_view line, bool lua, std::string_view lineComment) {
    std::vector<Token> tokens;
    if (line.size() > kIndentLexLimit) {
        return tokens;
    }
    for (std::size_t i = 0; i < line.size();) {
        const char c = line[i];
        if (!lineComment.empty() && line.substr(i).starts_with(lineComment)) {
            if (lua && line.substr(i).starts_with("--[[")) {
                const std::size_t close = line.find("]]", i + 4);
                if (close == std::string_view::npos) {
                    break;
                }
                i = close + 2;
                continue;
            }
            break;
        }
        if (!lua && line.substr(i).starts_with("/*")) {
            const std::size_t close = line.find("*/", i + 2);
            if (close == std::string_view::npos) {
                break;
            }
            i = close + 2;
            continue;
        }
        if (lua && line.substr(i).starts_with("[[")) {
            const std::size_t close = line.find("]]", i + 2);
            if (close == std::string_view::npos) {
                break;
            }
            i = close + 2;
            continue;
        }
        if (c == '"' || c == '\'' || (c == '`' && !lua)) {
            std::size_t j = i + 1;
            while (j < line.size() && line[j] != c) {
                j += line[j] == '\\' ? 2 : 1;
            }
            i = j + 1;
            continue;
        }
        if (isOpener(c)) {
            tokens.push_back(Token::Open);
            ++i;
            continue;
        }
        if (isCloser(c)) {
            tokens.push_back(Token::Close);
            ++i;
            continue;
        }
        if (lua && isWordChar(c) && (i == 0 || !isWordChar(line[i - 1]))) {
            std::size_t j = i;
            while (j < line.size() && isWordChar(line[j])) {
                ++j;
            }
            const std::string_view word = line.substr(i, j - i);
            const bool member = i > 0 && (line[i - 1] == '.' || line[i - 1] == ':');
            if (!member) {
                if (word == "function" || word == "then" || word == "do" || word == "repeat") {
                    tokens.push_back(Token::Open);
                } else if (word == "end" || word == "until" || word == "elseif") {
                    tokens.push_back(Token::Close);
                } else if (word == "else") {
                    tokens.push_back(Token::Close);
                    tokens.push_back(Token::Open);
                }
            }
            i = j;
            continue;
        }
        ++i;
    }
    return tokens;
}

// True when the line's first word or character closes a block (startNewLineBefore's rule).
bool startsWithCloserText(const Snapshot& text, const LineInfo& line, bool lua) {
    const ByteOffset start = detail::indentEnd(text, line);
    if (start == line.content.end) {
        return false;
    }
    if (isCloser(text.byteAt(start))) {
        return true;
    }
    if (!lua) {
        return false;
    }
    const std::string head = text.text(ByteRange{start, std::min(line.content.end, start + 7)});
    for (const std::string_view word : {"end", "until", "elseif", "else"}) {
        if (head.starts_with(word) && (head.size() == word.size() || !isWordChar(head[word.size()]))) {
            return true;
        }
    }
    return false;
}

// The shape of `line` (its content without indentation decides startsWithCloser).
LineShape shapeOf(const Snapshot& text, const LineInfo& line, bool lua, std::string_view lineComment) {
    LineShape shape;
    const ByteOffset start = detail::indentEnd(text, line);
    const std::string content = text.text(ByteRange{start, line.content.end});
    if (!lua && content.starts_with('#')) {
        shape.preprocessor = true;
        return shape;
    }
    if (!lua && content.starts_with('*')) {
        shape.commentStar = true;
        return shape;
    }
    const std::vector<Token> tokens = lexLine(content, lua, lineComment);
    shape.startsWithCloser = startsWithCloserText(text, line, lua);
    for (const Token token : tokens) {
        if (token == Token::Open) {
            ++shape.unmatchedOpens;
        } else if (shape.unmatchedOpens > 0) {
            --shape.unmatchedOpens;
        } else {
            ++shape.unmatchedCloses;
        }
    }
    return shape;
}

// `indent` with one level removed from its end (a unit, a tab, or up to indentSize spaces).
std::string dedent(std::string indent, const EditorSettings& settings) {
    if (indent.empty()) {
        return indent;
    }
    if (indent.back() == '\t') {
        indent.pop_back();
        return indent;
    }
    std::uint32_t removed = 0;
    while (!indent.empty() && indent.back() == ' ' && removed < std::max<std::uint32_t>(settings.indentSize, 1)) {
        indent.pop_back();
        ++removed;
    }
    return indent;
}

std::string indentUnitOf(const EditorSettings& settings) {
    return settings.insertSpaces ? std::string(std::max<std::uint32_t>(settings.indentSize, 1), ' ') : "\t";
}

[[nodiscard]] bool isLua(const LanguageInfo* language) noexcept { return language != nullptr && language->id == "lua"; }

} // namespace

std::optional<BracketPair> matchingBracket(const Snapshot& text, ByteOffset caret,
                                           const ISyntaxRanges* syntax) noexcept {
    if (caret < text.size() && (isOpener(text.byteAt(caret)) || isCloser(text.byteAt(caret)))) {
        if (auto pair = pairOf(text, caret, syntax)) {
            return pair;
        }
    }
    if (caret > ByteOffset{0}) {
        const ByteOffset at = before(caret);
        if (isOpener(text.byteAt(at)) || isCloser(text.byteAt(at))) {
            return pairOf(text, at, syntax);
        }
    }
    return std::nullopt;
}

std::optional<BracketPair> enclosingBraceBlock(const Snapshot& text, ByteOffset caret,
                                               const ISyntaxRanges* syntax) noexcept {
    if (syntax != nullptr) {
        for (const ByteRange& range : syntax->enclosing(ByteRange{caret, caret})) {
            if (range.size() < 2) {
                continue;
            }
            const ByteOffset last = before(range.end);
            if (text.byteAt(range.begin) == '{' && text.byteAt(last) == '}' && range.begin < caret && caret <= last) {
                return BracketPair{range.begin, last};
            }
        }
    }
    if (caret == ByteOffset{0}) {
        return std::nullopt;
    }
    const std::optional<ByteOffset> open = scanBackward(text, caret, '{', '}', 1);
    if (!open) {
        return std::nullopt;
    }
    const std::optional<ByteOffset> close = scanForward(text, *open + 1, '{', '}', 1);
    if (!close || *close < caret) {
        return std::nullopt;
    }
    return BracketPair{*open, *close};
}

std::optional<StatementMove> planStatementMove(const Snapshot& text, const EditorState& state, bool up,
                                               const ISyntaxRanges& syntax) noexcept {
    const std::vector<Selection> selections = detail::selectionsOf(state);
    const std::vector<detail::LineSpan> spans = detail::touchedSpans(text, selections, true);
    if (spans.empty()) {
        return std::nullopt;
    }
    LineBlock block{spans.front().first, spans.back().last};
    // A caret on a statement's continuation or closing line moves the statement around it.
    if (!statementAt(text, syntax, block.first)) {
        const std::uint32_t stop = block.first > kStatementSearchLines ? block.first - kStatementSearchLines : 0;
        for (std::uint32_t line = block.first; line-- > stop;) {
            if (const auto statement = statementAt(text, syntax, line)) {
                const std::uint32_t end = lastLineOf(text, *statement);
                if (end >= block.first) {
                    block.first = line;
                    block.last = std::max(block.last, end);
                    break;
                }
            }
        }
    }
    // Grow to the end of every statement that starts inside the block.
    for (std::uint32_t line = block.first; line <= block.last && line - block.first < kStatementSearchLines; ++line) {
        if (const auto statement = statementAt(text, syntax, line)) {
            block.last = std::max(block.last, lastLineOf(text, *statement));
        }
    }
    const std::uint32_t lineCount = text.lineCount();
    if (up) {
        if (block.first == 0) {
            return std::nullopt;
        }
        const std::uint32_t above = block.first - 1;
        if (isBlank(text, detail::lineAt(text, above))) {
            return StatementMove{block, LineBlock{above, above}};
        }
        const std::uint32_t stop = above > kStatementSearchLines ? above - kStatementSearchLines : 0;
        for (std::uint32_t line = above + 1; line-- > stop;) {
            if (const auto statement = statementAt(text, syntax, line)) {
                const std::uint32_t end = lastLineOf(text, *statement);
                if (end == above) {
                    return StatementMove{block, LineBlock{line, above}};
                }
                if (end > above) {
                    return std::nullopt; // The block is the first statement of this one.
                }
            }
            if (line == 0) {
                break;
            }
        }
        return std::nullopt;
    }
    if (block.last + 1 >= lineCount) {
        return std::nullopt;
    }
    const std::uint32_t below = block.last + 1;
    if (isBlank(text, detail::lineAt(text, below))) {
        return StatementMove{block, LineBlock{below, below}};
    }
    const auto statement = statementAt(text, syntax, below);
    if (!statement) {
        return std::nullopt; // A closing line: the block is the last statement of its block.
    }
    return StatementMove{block, LineBlock{below, std::max(below, lastLineOf(text, *statement))}};
}

namespace commands {

CommandResult splitLine(const CommandInput& in) noexcept {
    CommandResult result = newline(in);
    // Every caret goes back to where its break was inserted: its edit's start, mapped.
    std::vector<Selection> carets;
    std::int64_t delta = 0;
    for (const TextEdit& edit : result.edits) {
        carets.push_back(Selection::caret(
            ByteOffset{static_cast<std::uint64_t>(static_cast<std::int64_t>(edit.range.begin.value()) + delta)}));
        delta += static_cast<std::int64_t>(edit.text.size()) - static_cast<std::int64_t>(edit.range.size());
    }
    if (carets.empty()) {
        return result;
    }
    result.state = detail::makeState(carets, {}, std::min(detail::primaryOf(in.state), carets.size() - 1));
    return result;
}

CommandResult startNewLineBefore(const CommandInput& in) noexcept {
    const Snapshot& text = in.text;
    const bool lua = isLua(in.language);
    const std::string_view fallbackBreak = detail::documentBreak(text, in.settings.defaultLineBreak);
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    BatchBuilder batch;
    // One new line per line: an insert is never merged with an identical one (command_support.hpp).
    std::vector<std::pair<std::uint32_t, std::pair<std::size_t, std::uint64_t>>> perLine;
    std::vector<std::uint32_t> lineOfSelection;
    for (const Selection& selection : selections) {
        const LineInfo line = detail::lineOf(text, selection.head);
        lineOfSelection.push_back(line.line.value());
        if (std::ranges::any_of(perLine, [&](const auto& entry) { return entry.first == line.line.value(); })) {
            continue;
        }
        std::string indent = text.text(ByteRange{line.content.begin, detail::indentEnd(text, line)});
        if (startsWithCloserText(text, line, lua)) {
            indent += indentUnitOf(in.settings);
        }
        const std::string_view lineBreak =
            line.lineBreak != LineBreak::None ? detail::breakText(line.lineBreak) : fallbackBreak;
        const std::uint64_t caret = indent.size();
        const std::size_t id =
            batch.add(ByteRange{line.content.begin, line.content.begin}, indent + std::string(lineBreak));
        perLine.emplace_back(line.line.value(), std::pair{id, caret});
    }
    batch.finish();
    std::vector<Selection> after;
    for (const std::uint32_t line : lineOfSelection) {
        const auto it = std::ranges::find_if(perLine, [&](const auto& entry) { return entry.first == line; });
        after.push_back(Selection::caret(batch.post(it->second.first, it->second.second)));
    }
    CommandResult result;
    result.state = detail::makeState(after, {}, detail::primaryOf(in.state));
    result.kind = EditKind::Typing;
    result.breakCoalescing = true;
    result.edits = batch.take();
    return result;
}

CommandResult overtype(const CommandInput& in, std::string_view utf8) noexcept {
    CommandInput plain = in;
    plain.settings.autoClosePairs = false;
    if (utf8.empty()) {
        return CommandResult{{}, in.state, EditKind::Typing, false};
    }
    const bool breakCoalescing = type(plain, utf8).breakCoalescing;
    std::vector<Selection> selections = detail::selectionsOf(in.state);
    for (Selection& selection : selections) {
        if (!selection.empty()) {
            continue;
        }
        const LineInfo line = detail::lineOf(in.text, selection.head);
        if (selection.head < line.content.end) {
            selection = Selection{selection.head, nextGraphemeBoundary(in.text, selection.head)};
        }
    }
    plain.state = detail::makeState(selections, {}, detail::primaryOf(in.state));
    CommandResult result = type(plain, utf8);
    result.breakCoalescing = breakCoalescing;
    return result;
}

CommandResult swapLineBlocks(const CommandInput& in, LineBlock block, LineBlock neighbour) noexcept {
    const Snapshot& text = in.text;
    FROST_PRE(block.first <= block.last && neighbour.first <= neighbour.last);
    FROST_PRE(neighbour.last + 1 == block.first || block.last + 1 == neighbour.first);
    FROST_PRE(std::max(block.last, neighbour.last) < text.lineCount());
    const bool neighbourAbove = neighbour.last + 1 == block.first;
    const std::uint32_t first = std::min(block.first, neighbour.first);
    const std::uint32_t last = std::max(block.last, neighbour.last);
    const std::uint32_t n = last - first + 1;
    std::vector<std::string> contents;
    std::vector<std::string_view> breaks;
    std::vector<std::uint64_t> contentSize;
    for (std::uint32_t line = first; line <= last; ++line) {
        const LineInfo info = detail::lineAt(text, line);
        contents.push_back(text.text(info.content));
        breaks.push_back(detail::breakText(info.lineBreak));
        contentSize.push_back(info.content.size());
    }
    // The new order of the region's lines (indices relative to `first`); the breaks stay in place.
    std::vector<std::uint32_t> order;
    const LineBlock leading = neighbourAbove ? block : neighbour;
    const LineBlock trailing = neighbourAbove ? neighbour : block;
    for (std::uint32_t line = leading.first; line <= leading.last; ++line) {
        order.push_back(line - first);
    }
    for (std::uint32_t line = trailing.first; line <= trailing.last; ++line) {
        order.push_back(line - first);
    }
    std::string moved;
    std::vector<std::uint64_t> newStart(n, 0);
    std::vector<std::uint64_t> newBreakSize(n, 0);
    for (std::uint32_t k = 0; k < n; ++k) {
        newStart[order[k]] = moved.size();
        newBreakSize[order[k]] = breaks[k].size();
        moved += contents[order[k]];
        moved += breaks[k];
    }
    BatchBuilder batch;
    const ByteRange range{detail::lineAt(text, first).content.begin,
                          detail::lineEndWithBreak(detail::lineAt(text, last))};
    const std::size_t id = batch.add(range, std::move(moved));
    batch.finish();
    // As in moveLines: an endpoint keeps its place in its line; `exclusiveEnd` marks a selection end
    // at the start of the line after its last line.
    const auto mapEndpoint = [&](ByteOffset offset, bool exclusiveEnd) {
        const Point point = text.pointAt(offset);
        const std::uint32_t line = exclusiveEnd ? point.line.value() - 1 : point.line.value();
        if (first <= line && line <= last) {
            const std::size_t k = line - first;
            const std::uint64_t column = exclusiveEnd ? contentSize[k] + newBreakSize[k]
                                                      : std::min<std::uint64_t>(point.column.value(), contentSize[k]);
            return batch.post(id, newStart[k] + column);
        }
        return batch.map(offset, Bias::Right);
    };
    std::vector<Selection> after;
    for (const Selection& selection : detail::selectionsOf(in.state)) {
        const ByteRange selected = selection.range();
        const Point endPoint = text.pointAt(selected.end);
        const bool exclusiveEnd =
            !selected.empty() && endPoint.column == ByteColumn{0} && endPoint.line > text.pointAt(selected.begin).line;
        const ByteOffset begin = mapEndpoint(selected.begin, false);
        const ByteOffset end = mapEndpoint(selected.end, exclusiveEnd);
        if (selection.empty()) {
            after.push_back(Selection::caret(begin));
        } else {
            after.push_back(selection.reversed() ? Selection{end, begin} : Selection{begin, end});
        }
    }
    CommandResult result;
    result.state = detail::makeState(after, {}, detail::primaryOf(in.state));
    result.kind = EditKind::Other;
    result.edits = batch.take();
    return result;
}

CommandResult moveStatement(const CommandInput& in, bool up, const ISyntaxRanges* syntax) noexcept {
    if (syntax == nullptr) {
        return up ? moveLinesUp(in) : moveLinesDown(in);
    }
    const std::optional<StatementMove> plan = planStatementMove(in.text, in.state, up, *syntax);
    if (!plan) {
        return CommandResult{{}, in.state, EditKind::Other, false};
    }
    return swapLineBlocks(in, plan->block, plan->neighbour);
}

bool autoIndentSupports(const LanguageInfo* language) noexcept {
    if (language == nullptr) {
        return false;
    }
    constexpr std::array kSupported{std::string_view{"c"},          std::string_view{"cpp"},
                                    std::string_view{"csharp"},     std::string_view{"hlsl"},
                                    std::string_view{"glsl"},       std::string_view{"javascript"},
                                    std::string_view{"typescript"}, std::string_view{"json"},
                                    std::string_view{"jsonc"},      std::string_view{"lua"}};
    return std::ranges::find(kSupported, language->id) != kSupported.end();
}

CommandResult autoIndentLines(const CommandInput& in) noexcept {
    CommandResult unchanged{{}, in.state, EditKind::Other, false};
    if (!autoIndentSupports(in.language)) {
        return unchanged;
    }
    const Snapshot& text = in.text;
    const bool lua = isLua(in.language);
    const std::string_view lineComment = in.language->lineComment;
    const std::string unit = indentUnitOf(in.settings);
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    const std::vector<detail::LineSpan> spans = detail::touchedSpans(text, selections, true);
    // The indentation each re-indented line gets (later lines refer to it, top-down).
    std::vector<std::pair<std::uint32_t, std::string>> assigned;
    const auto indentOf = [&](std::uint32_t line) {
        for (auto it = assigned.rbegin(); it != assigned.rend(); ++it) {
            if (it->first == line) {
                return it->second;
            }
        }
        const LineInfo info = detail::lineAt(text, line);
        return text.text(ByteRange{info.content.begin, detail::indentEnd(text, info)});
    };
    const auto shapeAt = [&](std::uint32_t line) {
        return shapeOf(text, detail::lineAt(text, line), lua, lineComment);
    };
    BatchBuilder batch;
    for (const detail::LineSpan& span : spans) {
        for (std::uint32_t line = span.first; line <= span.last; ++line) {
            const LineInfo info = detail::lineAt(text, line);
            if (isBlank(text, info)) {
                continue;
            }
            const LineShape shape = shapeAt(line);
            if (shape.preprocessor || shape.commentStar) {
                continue;
            }
            // The reference: the nearest non-blank line above that is not a preprocessor line.
            std::optional<std::uint32_t> reference;
            const std::uint32_t stop = line > kIndentSearchLines ? line - kIndentSearchLines : 0;
            for (std::uint32_t above = line; above-- > stop;) {
                if (!isBlank(text, detail::lineAt(text, above)) && !shapeAt(above).preprocessor) {
                    reference = above;
                    break;
                }
            }
            std::string target;
            if (reference) {
                const LineShape ref = shapeAt(*reference);
                target = indentOf(*reference);
                if (ref.commentStar && !target.empty() && target.back() == ' ') {
                    target.pop_back(); // " * text" sits one space right of its "/*".
                } else if (ref.unmatchedOpens > 0) {
                    target += unit;
                } else if (ref.unmatchedCloses > 0) {
                    // The line that opened what the reference closes sets the indentation.
                    int depth = ref.unmatchedCloses;
                    const std::uint32_t openStop =
                        *reference > kIndentSearchLines ? *reference - kIndentSearchLines : 0;
                    for (std::uint32_t above = *reference; above-- > openStop;) {
                        const LineInfo aboveInfo = detail::lineAt(text, above);
                        const ByteOffset start = detail::indentEnd(text, aboveInfo);
                        const std::string content = text.text(ByteRange{start, aboveInfo.content.end});
                        if (!lua && (content.starts_with('#') || content.starts_with('*'))) {
                            continue;
                        }
                        const std::vector<Token> tokens = lexLine(content, lua, lineComment);
                        bool found = false;
                        for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) {
                            depth += *it == Token::Close ? 1 : -1;
                            if (depth == 0) {
                                found = true;
                                break;
                            }
                        }
                        if (found) {
                            target = indentOf(above);
                            break;
                        }
                    }
                }
            }
            if (shape.startsWithCloser) {
                target = dedent(std::move(target), in.settings);
            }
            const ByteRange current{info.content.begin, detail::indentEnd(text, info)};
            if (text.text(current) != target) {
                batch.add(current, target);
            }
            assigned.emplace_back(line, std::move(target));
        }
    }
    batch.finish();
    if (batch.empty()) {
        return unchanged;
    }
    CommandResult result;
    result.state = detail::mapState(in.state, batch.raw());
    result.kind = EditKind::Other;
    result.edits = batch.take();
    return result;
}

} // namespace commands

} // namespace frost::text
