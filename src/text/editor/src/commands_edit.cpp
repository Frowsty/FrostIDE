// SPDX-License-Identifier: Apache-2.0
//
// Typing, smart keys and deletion (frost/text/editor/commands.hpp, ADR 0025): auto-close and
// overtype of brackets and quotes, surround, Enter with auto-indent and bracket expansion, Tab and
// Shift+Tab, Backspace over indentation and empty pairs, deletion by grapheme cluster and by word.

#include "frost/core/assert.hpp"
#include "frost/text/editor/commands.hpp"
#include "frost/text/editor/unicode.hpp"

#include "char_cursor.hpp"
#include "command_support.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace frost::text::commands {

namespace {

using detail::BatchBuilder;

// The closer of an auto-closed opener, or 0.
char closerOf(char32_t cp) noexcept {
    switch (cp) {
    case U'(': return ')';
    case U'[': return ']';
    case U'{': return '}';
    case U'"': return '"';
    case U'\'': return '\'';
    default: return 0;
    }
}

// The opener of a bracket closer, or 0 (quotes are their own openers).
char openerOf(char32_t cp) noexcept {
    switch (cp) {
    case U')': return '(';
    case U']': return '[';
    case U'}': return '{';
    default: return 0;
    }
}

bool isQuote(char32_t cp) noexcept { return cp == U'"' || cp == U'\''; }

// How often byte `c` occurs in the line's content.
std::size_t countOnLine(const Snapshot& text, const LineInfo& line, char c) noexcept {
    std::size_t count = 0;
    if (line.content.empty()) {
        return 0;
    }
    for (const std::string_view chunk : text.chunks(line.content)) {
        count += static_cast<std::size_t>(std::count(chunk.begin(), chunk.end(), c));
    }
    return count;
}

// True when the character after a caret allows auto-closing: the line end, whitespace or a closer.
bool closesBefore(std::optional<DecodedChar> next) noexcept {
    if (!next) {
        return true;
    }
    const char32_t cp = next->cp;
    const CharClass cls = charClass(cp);
    return cls == CharClass::Space || cls == CharClass::LineBreak || cp == U')' || cp == U']' || cp == U'}' ||
           cp == U',' || cp == U';' || cp == U':';
}

// The single code point of `utf8`, or nullopt when it holds more or fewer.
std::optional<char32_t> singleChar(std::string_view utf8) noexcept {
    if (utf8.empty()) {
        return std::nullopt;
    }
    const DecodedChar c = decodeUtf8(utf8);
    return c.bytes == utf8.size() ? std::optional<char32_t>(c.cp) : std::nullopt;
}

// The indentation unit: indentSize spaces or a tab.
std::string indentUnit(const EditorSettings& settings) {
    return settings.insertSpaces ? std::string(std::max<std::uint32_t>(settings.indentSize, 1), ' ') : "\t";
}

// Lex the prefix to distinguish Lua block keywords from quoted and long strings/comments.
// A long delimiter has matching counts of '='; short strings and line comments end on newline.
struct LuaLine {
    std::string first;
    std::string last;
    int openFunctions = 0;
    std::optional<std::string> closerIndent;
    bool code = true;
    char lastSymbol = 0;
};
LuaLine luaLineAt(const Snapshot& text, ByteOffset lineStart, ByteOffset at, const IIndentContext* context) {
    ByteOffset start{0};
    if (context != nullptr) {
        start = context->scanStart(lineStart);
        const auto region = context->opaqueAt(lineStart);
        if (region && region->begin < lineStart) {
            if (region->end >= at) {
                LuaLine inside;
                inside.code = false;
                return inside;
            }
            start = region->end;
        }
    } else if (at.value() > 65536) {
        // Without a current parse, do not guess whether an old long string or comment is open.
        LuaLine unknown;
        unknown.code = false;
        return unknown;
    }
    if (at.value() - start.value() > 65536) {
        LuaLine unknown;
        unknown.code = false;
        return unknown;
    }
    const std::string prefix = text.text(ByteRange{start, at});
    LuaLine line;
    char quote = 0;
    bool lineComment = false;
    int longEquals = -1;
    struct Block {
        std::string kind;
        // Offsets into `prefix`: sharing one indentation across many blocks stays O(prefix).
        std::size_t indentStart;
        std::size_t indentSize;
        std::size_t lineNumber;
    };
    std::vector<Block> blocks;
    std::size_t indentStart = 0;
    std::size_t indentSize = 0;
    bool leading = true;
    bool elseIf = false;
    std::size_t lineNumber = 0;
    const auto nextLine = [&] {
        line = LuaLine{};
        lineComment = false;
        indentStart = 0;
        indentSize = 0;
        leading = true;
        elseIf = false;
        ++lineNumber;
    };
    for (std::size_t i = 0; i < prefix.size();) {
        const char c = prefix[i];
        if (c == '\n' || c == '\r') {
            nextLine();
            quote = 0;
            ++i;
            continue;
        }
        if (lineComment) {
            ++i;
            continue;
        }
        if (quote != 0) {
            if (c == '\\' && i + 1 < prefix.size()) {
                const char escaped = prefix[i + 1];
                i += 2;
                if (escaped == 'z') {
                    // Lua 5.4 manual section 3.1: \z consumes all following ASCII whitespace.
                    while (i < prefix.size() && (prefix[i] == ' ' || prefix[i] == '\t' || prefix[i] == '\n' ||
                                                 prefix[i] == '\r' || prefix[i] == '\f' || prefix[i] == '\v')) {
                        if (prefix[i] == '\n' || prefix[i] == '\r') {
                            nextLine();
                        }
                        ++i;
                    }
                } else if (escaped == '\r' || escaped == '\n') {
                    nextLine();
                    if (i < prefix.size() &&
                        ((escaped == '\r' && prefix[i] == '\n') || (escaped == '\n' && prefix[i] == '\r'))) {
                        ++i;
                    }
                }
                continue;
            }
            if (c == quote) {
                quote = 0;
            }
            ++i;
            continue;
        }
        if (longEquals >= 0) {
            if (c == ']') {
                std::size_t j = i + 1;
                while (j < prefix.size() && prefix[j] == '=') {
                    ++j;
                }
                if (j < prefix.size() && prefix[j] == ']' && j - i - 1 == static_cast<std::size_t>(longEquals)) {
                    longEquals = -1;
                    i = j + 1;
                    continue;
                }
            }
            ++i;
            continue;
        }
        if (leading && (c == ' ' || c == '\t')) {
            if (indentSize == 0) {
                indentStart = i;
            }
            ++indentSize;
            ++i;
            continue;
        }
        leading = false;
        if (c == '-' && i + 1 < prefix.size() && prefix[i + 1] == '-') {
            i += 2;
            if (i >= prefix.size() || prefix[i] != '[') {
                lineComment = true;
                continue;
            }
            std::size_t j = i + 1;
            while (j < prefix.size() && prefix[j] == '=') {
                ++j;
            }
            if (j >= prefix.size() || prefix[j] != '[') {
                lineComment = true;
                continue;
            }
            longEquals = static_cast<int>(j - i - 1);
            i = j + 1;
            continue;
        }
        if (c == '[') {
            std::size_t j = i + 1;
            while (j < prefix.size() && prefix[j] == '=') {
                ++j;
            }
            if (j < prefix.size() && prefix[j] == '[') {
                longEquals = static_cast<int>(j - i - 1);
                i = j + 1;
                continue;
            }
        }
        if (c == '\'' || c == '"') {
            quote = c;
            ++i;
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            const std::size_t tokenStart = i++;
            while (i < prefix.size() &&
                   ((prefix[i] >= 'a' && prefix[i] <= 'z') || (prefix[i] >= 'A' && prefix[i] <= 'Z') ||
                    (prefix[i] >= '0' && prefix[i] <= '9') || prefix[i] == '_')) {
                ++i;
            }
            line.last = prefix.substr(tokenStart, i - tokenStart);
            if (line.first.empty()) {
                line.first = line.last;
            }
            if (line.last == "elseif") {
                elseIf = true;
            }
            if (line.last == "function" || line.last == "do" || line.last == "repeat" ||
                (line.last == "then" && !elseIf)) {
                blocks.push_back({line.last, indentStart, indentSize, lineNumber});
                if (line.last == "function") {
                    ++line.openFunctions;
                }
            } else if (line.last == "end" || line.last == "until") {
                if (!blocks.empty()) {
                    if (line.first == line.last) {
                        line.closerIndent = prefix.substr(blocks.back().indentStart, blocks.back().indentSize);
                    }
                    if (blocks.back().lineNumber == lineNumber && blocks.back().kind == "function") {
                        --line.openFunctions;
                    }
                    blocks.pop_back();
                }
            }
            line.lastSymbol = 0;
            continue;
        }
        if (c != ' ' && c != '\t') {
            line.lastSymbol = c;
        }
        ++i;
    }
    line.code = quote == 0 && longEquals < 0;
    return line;
}

// The whitespace to insert at `at` to reach the next indent stop.
std::string indentAt(const Snapshot& text, ByteOffset at, const EditorSettings& settings) {
    if (!settings.insertSpaces) {
        return "\t";
    }
    const std::uint64_t size = std::max<std::uint32_t>(settings.indentSize, 1);
    const std::uint64_t column = detail::visualColumn(text, detail::lineOf(text, at), at, settings.tabSize);
    return std::string(static_cast<std::size_t>(size - column % size), ' ');
}

// True when a new undo step should start before typing `utf8` at `at` (commands.hpp: a line
// break, or a non-space after a space, a line break or the text start).
bool startsNewStep(const Snapshot& text, ByteOffset at, std::string_view utf8) noexcept {
    if (utf8.find_first_of("\r\n") != std::string_view::npos) {
        return true;
    }
    const CharClass typed = charClass(decodeUtf8(utf8).cp);
    if (typed == CharClass::Space) {
        return false;
    }
    const std::optional<DecodedChar> before = charBefore(text, at);
    return !before || charClass(before->cp) == CharClass::Space || charClass(before->cp) == CharClass::LineBreak;
}

// Replaces every selection with `utf8`; the carets go after the inserted text.
CommandResult insertEverywhere(const CommandInput& in, std::string_view utf8, EditKind kind) {
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    BatchBuilder batch;
    std::vector<std::size_t> ids;
    for (const Selection& selection : selections) {
        ids.push_back(batch.add(selection.range(), std::string(utf8)));
    }
    batch.finish();
    std::vector<Selection> after;
    for (const std::size_t id : ids) {
        after.push_back(Selection::caret(batch.post(id, utf8.size())));
    }
    CommandResult result;
    result.state = detail::makeState(after, {}, detail::primaryOf(in.state));
    result.edits = batch.take();
    result.kind = kind;
    return result;
}

// Deletes `ranges` (one per selection, in selection order; they may overlap and are merged) and
// leaves a caret at each deletion point.
CommandResult deleteRanges(const CommandInput& in, const std::vector<ByteRange>& ranges, EditKind kind) {
    std::vector<ByteRange> sorted = ranges;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const ByteRange& a, const ByteRange& b) { return a.begin < b.begin; });
    std::vector<ByteRange> merged;
    for (const ByteRange& range : sorted) {
        if (!merged.empty() && range.begin <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, range.end);
        } else {
            merged.push_back(range);
        }
    }
    BatchBuilder batch;
    for (const ByteRange& range : merged) {
        if (!range.empty()) {
            batch.add(range, {});
        }
    }
    batch.finish();
    CommandResult result;
    result.kind = kind;
    std::vector<Selection> after;
    for (const ByteRange& range : ranges) {
        after.push_back(Selection::caret(batch.map(range.begin, Bias::Left)));
    }
    result.state = detail::makeState(after, {}, std::min(detail::primaryOf(in.state), after.size() - 1));
    result.edits = batch.take();
    return result;
}

} // namespace

CommandResult type(const CommandInput& in, std::string_view utf8) noexcept {
    if (utf8.empty()) {
        return CommandResult{{}, in.state, EditKind::Typing, false};
    }
    const Snapshot& text = in.text;
    const std::optional<char32_t> single = singleChar(utf8);
    const bool smart = in.settings.autoClosePairs && single.has_value();
    const std::vector<Selection> selections = detail::selectionsOf(in.state);

    BatchBuilder batch;
    // Per selection: the new selection as edit ids and offsets into their text, or a moved caret.
    struct Plan {
        std::size_t anchorId = 0;
        std::uint64_t anchorAt = 0;
        std::size_t headId = 0;
        std::uint64_t headAt = 0;
        std::optional<ByteOffset> overtype; // The caret steps over the next character instead.
    };
    std::vector<Plan> plans;
    for (const Selection& selection : selections) {
        const ByteRange range = selection.range();
        Plan plan;
        if (smart && !range.empty() && closerOf(*single) != 0) {
            // Surround: the selection stays on the inner text.
            const std::size_t open = batch.add(ByteRange{range.begin, range.begin}, std::string(utf8));
            const std::size_t close = batch.add(ByteRange{range.end, range.end}, std::string(1, closerOf(*single)));
            const bool reversed = selection.reversed();
            plan.anchorId = reversed ? close : open;
            plan.anchorAt = reversed ? 0 : utf8.size();
            plan.headId = reversed ? open : close;
            plan.headAt = reversed ? utf8.size() : 0;
            plans.push_back(plan);
            continue;
        }
        if (smart && range.empty()) {
            const ByteOffset at = range.begin;
            const std::optional<DecodedChar> next = charAt(text, at);
            const LineInfo line = detail::lineOf(text, at);
            // Overtype a closer that is already there.
            if (next && next->cp == *single && (openerOf(*single) != 0 || isQuote(*single))) {
                const bool balanced = isQuote(*single) ? countOnLine(text, line, static_cast<char>(*single)) % 2 == 0
                                                       : countOnLine(text, line, openerOf(*single)) <=
                                                             countOnLine(text, line, static_cast<char>(*single));
                if (balanced) {
                    plan.overtype = at + next->bytes;
                    plans.push_back(plan);
                    continue;
                }
            }
            // Auto-close an opener.
            if (closerOf(*single) != 0 && closesBefore(next)) {
                bool close = true;
                if (isQuote(*single)) {
                    const std::optional<DecodedChar> before = charBefore(text, at);
                    close = (!before || (charClass(before->cp) != CharClass::Word && before->cp != *single)) &&
                            countOnLine(text, line, static_cast<char>(*single)) % 2 == 0;
                }
                if (close) {
                    const std::size_t id = batch.add(range, std::string(utf8) + closerOf(*single));
                    plan.anchorId = plan.headId = id;
                    plan.anchorAt = plan.headAt = utf8.size();
                    plans.push_back(plan);
                    continue;
                }
            }
        }
        const std::size_t id = batch.add(range, std::string(utf8));
        plan.anchorId = plan.headId = id;
        plan.anchorAt = plan.headAt = utf8.size();
        plans.push_back(plan);
    }
    batch.finish();
    std::vector<Selection> after;
    for (const Plan& plan : plans) {
        if (plan.overtype) {
            after.push_back(Selection::caret(batch.map(*plan.overtype, Bias::Right)));
        } else {
            after.push_back(Selection{batch.post(plan.anchorId, plan.anchorAt), batch.post(plan.headId, plan.headAt)});
        }
    }
    CommandResult result;
    result.state = detail::makeState(after, {}, detail::primaryOf(in.state));
    result.kind = EditKind::Typing;
    const Selection primary = in.state.primary();
    result.breakCoalescing = !primary.empty() || startsNewStep(text, primary.range().begin, utf8);
    result.edits = batch.take();
    return result;
}

CommandResult paste(const CommandInput& in, std::string_view utf8) noexcept {
    if (utf8.empty()) {
        return CommandResult{{}, in.state, EditKind::Paste, false};
    }
    CommandResult result = insertEverywhere(in, utf8, EditKind::Paste);
    result.breakCoalescing = true;
    return result;
}

CommandResult pasteLines(const CommandInput& in, std::string_view utf8) noexcept {
    if (utf8.empty()) {
        return CommandResult{{}, in.state, EditKind::Paste, false};
    }
    const Snapshot& text = in.text;
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    // One insertion per line: the selections are sorted, so those sharing a line are adjacent.
    BatchBuilder batch;
    std::optional<ByteOffset> previous;
    for (const Selection& selection : selections) {
        const ByteOffset start = detail::lineOf(text, selection.range().begin).content.begin;
        if (previous != start) {
            batch.add(ByteRange{start, start}, std::string(utf8));
            previous = start;
        }
    }
    batch.finish();
    // Right bias: an endpoint at its line start stays with its line, below the pasted text.
    std::vector<Selection> after;
    for (const Selection& selection : selections) {
        after.push_back(Selection{batch.map(selection.anchor, Bias::Right), batch.map(selection.head, Bias::Right)});
    }
    CommandResult result;
    result.state = detail::makeState(after, {}, detail::primaryOf(in.state));
    result.edits = batch.take();
    result.kind = EditKind::Paste;
    result.breakCoalescing = true;
    return result;
}

CommandResult newline(const CommandInput& in) noexcept {
    const Snapshot& text = in.text;
    const std::string_view fallbackBreak = detail::documentBreak(text, in.settings.defaultLineBreak);
    const std::string unit = indentUnit(in.settings);
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    BatchBuilder batch;
    std::vector<std::pair<std::size_t, std::uint64_t>> carets;
    for (const Selection& selection : selections) {
        const ByteRange range = selection.range();
        const LineInfo line = detail::lineOf(text, range.begin);
        const std::string_view lineBreak =
            line.lineBreak != LineBreak::None ? detail::breakText(line.lineBreak) : fallbackBreak;
        const ByteOffset indentStop = std::min(detail::indentEnd(text, line), range.begin);
        std::string indent = text.text(ByteRange{line.content.begin, indentStop});
        // The last non-space character before the selection on its line, and the one after it.
        std::optional<DecodedChar> before;
        {
            detail::BackwardChars chars(text, range.begin);
            ByteOffset at;
            DecodedChar c;
            while (chars.next(at, c) && at >= line.content.begin) {
                if (c.cp != U' ' && c.cp != U'\t') {
                    before = c;
                    break;
                }
                if (at == line.content.begin) {
                    break;
                }
            }
        }
        const std::optional<DecodedChar> after = charAt(text, range.end);
        const bool lua = in.language != nullptr && in.language->id == "lua";
        const LuaLine luaLine = lua ? luaLineAt(text, line.content.begin, range.begin, in.indentContext) : LuaLine{};
        const bool luaOpen =
            lua && luaLine.code && luaLine.lastSymbol == 0 &&
            (luaLine.last == "then" || luaLine.last == "do" || luaLine.last == "else" || luaLine.last == "repeat");
        const bool luaFunction = lua && luaLine.code && luaLine.openFunctions > 0 && luaLine.lastSymbol == ')';
        const bool opensBlock = (!lua || luaLine.code) && ((before && before->cp == U'{') || luaOpen || luaFunction);
        if (lua && luaLine.code && (luaLine.first == "end" || luaLine.first == "until")) {
            std::optional<std::string> openerIndent = luaLine.closerIndent;
            if (in.indentContext != nullptr) {
                if (const auto opener = in.indentContext->closerOpener(indentStop)) {
                    const LineInfo openingLine = detail::lineOf(text, *opener);
                    openerIndent =
                        text.text(ByteRange{openingLine.content.begin, detail::indentEnd(text, openingLine)});
                }
            }
            // A correctly aligned closer already has the opener's indentation. Only reduce
            // excess whitespace; do not guess when neither the parse nor scanner found it.
            if (openerIndent && indent.size() > openerIndent->size() && indent.starts_with(*openerIndent)) {
                indent = *openerIndent;
            }
        }
        const bool betweenPair = (!lua || luaLine.code) && before && after && closerOf(before->cp) != 0 &&
                                 !isQuote(before->cp) && static_cast<char32_t>(closerOf(before->cp)) == after->cp;
        std::string inserted(lineBreak);
        inserted += indent;
        std::uint64_t caret = inserted.size();
        if (betweenPair) {
            inserted += unit;
            caret = inserted.size();
            inserted += lineBreak;
            inserted += indent;
        } else if (opensBlock) {
            inserted += unit;
            caret = inserted.size();
        }
        carets.emplace_back(batch.add(range, std::move(inserted)), caret);
    }
    batch.finish();
    std::vector<Selection> after;
    for (const auto& [id, caret] : carets) {
        after.push_back(Selection::caret(batch.post(id, caret)));
    }
    CommandResult result;
    result.state = detail::makeState(after, {}, detail::primaryOf(in.state));
    result.kind = EditKind::Typing;
    result.breakCoalescing = true;
    result.edits = batch.take();
    return result;
}

CommandResult tab(const CommandInput& in) noexcept {
    const Snapshot& text = in.text;
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    const bool anySelection =
        std::any_of(selections.begin(), selections.end(), [](const Selection& s) { return !s.empty(); });
    BatchBuilder batch;
    if (!anySelection) {
        std::vector<std::pair<std::size_t, std::uint64_t>> carets;
        for (const Selection& selection : selections) {
            std::string indent = indentAt(text, selection.head, in.settings);
            const std::uint64_t size = indent.size();
            carets.emplace_back(batch.add(selection.range(), std::move(indent)), size);
        }
        batch.finish();
        std::vector<Selection> after;
        for (const auto& [id, caret] : carets) {
            after.push_back(Selection::caret(batch.post(id, caret)));
        }
        CommandResult result;
        result.state = detail::makeState(after, {}, detail::primaryOf(in.state));
        result.kind = EditKind::Typing;
        result.edits = batch.take();
        return result;
    }
    // Indent the lines of every selection (carets mixed in indent their own line).
    const std::string unit = indentUnit(in.settings);
    for (const detail::LineSpan& span : detail::touchedSpans(text, selections, false)) {
        for (std::uint32_t line = span.first; line <= span.last; ++line) {
            const LineInfo info = detail::lineAt(text, line);
            if (!info.content.empty()) {
                batch.add(ByteRange{info.content.begin, info.content.begin}, unit);
            }
        }
    }
    batch.finish();
    CommandResult result;
    result.state = detail::mapState(in.state, batch.raw());
    result.kind = EditKind::Other;
    result.edits = batch.take();
    return result;
}

CommandResult outdent(const CommandInput& in) noexcept {
    const Snapshot& text = in.text;
    const std::uint32_t size = std::max<std::uint32_t>(in.settings.indentSize, 1);
    BatchBuilder batch;
    for (const detail::LineSpan& span : detail::touchedSpans(text, detail::selectionsOf(in.state), false)) {
        for (std::uint32_t line = span.first; line <= span.last; ++line) {
            const LineInfo info = detail::lineAt(text, line);
            if (info.content.empty()) {
                continue;
            }
            const std::string head = text.text(
                ByteRange{info.content.begin, std::min(info.content.end, info.content.begin + std::uint64_t{size})});
            std::size_t remove = 0;
            if (!head.empty() && head[0] == '\t') {
                remove = 1;
            } else {
                while (remove < head.size() && head[remove] == ' ') {
                    ++remove;
                }
                // A tab after fewer spaces than one level ends the indent level too.
                if (remove < head.size() && head[remove] == '\t') {
                    ++remove;
                }
            }
            if (remove > 0) {
                batch.add(ByteRange{info.content.begin, info.content.begin + remove}, {});
            }
        }
    }
    batch.finish();
    CommandResult result;
    result.state = detail::mapState(in.state, batch.raw());
    result.kind = EditKind::Other;
    result.edits = batch.take();
    return result;
}

CommandResult backspace(const CommandInput& in) noexcept {
    const Snapshot& text = in.text;
    std::vector<ByteRange> ranges;
    for (const Selection& selection : detail::selectionsOf(in.state)) {
        const ByteRange range = selection.range();
        if (!range.empty()) {
            ranges.push_back(range);
            continue;
        }
        const ByteOffset at = range.begin;
        if (at == ByteOffset{0}) {
            ranges.push_back(range);
            continue;
        }
        const LineInfo line = detail::lineOf(text, at);
        // Only spaces before the caret on its line: back to the previous indent stop.
        if (at > line.content.begin && at <= line.content.end && detail::indentEnd(text, line) >= at) {
            const std::string indent = text.text(ByteRange{line.content.begin, at});
            if (indent.find('\t') == std::string::npos) {
                const std::uint64_t size = std::max<std::uint32_t>(in.settings.indentSize, 1);
                const std::uint64_t column = indent.size();
                const std::uint64_t remove = column % size == 0 ? size : column % size;
                ranges.push_back(ByteRange{at - std::min(remove, column), at});
                continue;
            }
        }
        // An empty pair around the caret.
        if (in.settings.autoClosePairs) {
            const std::optional<DecodedChar> before = charBefore(text, at);
            const std::optional<DecodedChar> after = charAt(text, at);
            if (before && after && closerOf(before->cp) != 0 &&
                static_cast<char32_t>(closerOf(before->cp)) == after->cp) {
                ranges.push_back(ByteRange{at - 1, at + 1});
                continue;
            }
        }
        ranges.push_back(ByteRange{previousGraphemeBoundary(text, at), at});
    }
    return deleteRanges(in, ranges, EditKind::DeleteBackward);
}

CommandResult deleteForward(const CommandInput& in) noexcept {
    std::vector<ByteRange> ranges;
    for (const Selection& selection : detail::selectionsOf(in.state)) {
        const ByteRange range = selection.range();
        ranges.push_back(range.empty() ? ByteRange{range.begin, nextGraphemeBoundary(in.text, range.begin)} : range);
    }
    return deleteRanges(in, ranges, EditKind::DeleteForward);
}

CommandResult deleteWordBackward(const CommandInput& in) noexcept {
    std::vector<ByteRange> ranges;
    for (const Selection& selection : detail::selectionsOf(in.state)) {
        const ByteRange range = selection.range();
        ranges.push_back(range.empty() ? ByteRange{previousWordBoundary(in.text, range.begin), range.begin} : range);
    }
    return deleteRanges(in, ranges, EditKind::DeleteBackward);
}

CommandResult deleteWordForward(const CommandInput& in) noexcept {
    std::vector<ByteRange> ranges;
    for (const Selection& selection : detail::selectionsOf(in.state)) {
        const ByteRange range = selection.range();
        ranges.push_back(range.empty() ? ByteRange{range.begin, nextWordBoundary(in.text, range.begin)} : range);
    }
    return deleteRanges(in, ranges, EditKind::DeleteForward);
}

} // namespace frost::text::commands
