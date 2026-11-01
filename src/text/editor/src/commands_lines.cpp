// SPDX-License-Identifier: Apache-2.0
//
// Line operations (frost/text/editor/commands.hpp, ADR 0025): duplicate line or selection, delete
// lines, move lines up and down, join lines, and line and block comments with the language's
// tokens (language.hpp).

#include "frost/core/assert.hpp"
#include "frost/text/editor/commands.hpp"
#include "frost/text/editor/unicode.hpp"

#include "char_cursor.hpp"
#include "command_support.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace frost::text::commands {

namespace {

using detail::BatchBuilder;
using detail::LineSpan;

const LanguageInfo& languageOf(const CommandInput& in) noexcept {
    return in.language != nullptr ? *in.language : plainTextLanguage();
}

// True when the text at `at` starts with `token`.
bool startsWith(const Snapshot& text, ByteOffset at, std::string_view token) noexcept {
    if (text.size() - at < token.size()) {
        return false;
    }
    return text.text(ByteRange{at, at + token.size()}) == token;
}

// The span in `spans` (sorted, disjoint) holding `line`, or nullptr.
const LineSpan* spanOf(const std::vector<LineSpan>& spans, std::uint32_t line) noexcept {
    for (const LineSpan& span : spans) {
        if (span.first <= line && line <= span.last) {
            return &span;
        }
    }
    return nullptr;
}

// A selection's endpoints mapped through `batch` with the derived biases (selection.hpp).
Selection mapSelection(const BatchBuilder& batch, const Selection& selection) noexcept {
    if (selection.empty()) {
        const ByteOffset at = batch.map(selection.head, Bias::Right);
        return Selection::caret(at);
    }
    const ByteRange range = selection.range();
    const ByteOffset begin = batch.map(range.begin, Bias::Left);
    const ByteOffset end = batch.map(range.end, Bias::Right);
    return selection.reversed() ? Selection{end, begin} : Selection{begin, end};
}

CommandResult finishWith(const CommandInput& in, BatchBuilder& batch, const std::vector<Selection>& after) {
    CommandResult result;
    result.state = detail::makeState(after, {}, detail::primaryOf(in.state));
    result.kind = EditKind::Other;
    result.edits = batch.take();
    return result;
}

CommandResult moveLines(const CommandInput& in, bool up) {
    const Snapshot& text = in.text;
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    const std::uint32_t lineCount = text.lineCount();
    BatchBuilder batch;
    // One moved region: its lines, where each starts in the new text, and the edit's id.
    struct Region {
        std::uint32_t first = 0;
        std::uint32_t last = 0;
        std::vector<std::uint64_t> newStart;     // By line - first.
        std::vector<std::uint64_t> contentSize;  // By line - first.
        std::vector<std::uint64_t> newBreakSize; // The break that follows the line in the new text.
        std::size_t id = 0;
    };
    std::vector<Region> regions;
    for (const LineSpan& span : detail::touchedSpans(text, selections, true)) {
        if (up ? span.first == 0 : span.last + 1 >= lineCount) {
            continue; // Already at the first (last) line.
        }
        Region region;
        region.first = up ? span.first - 1 : span.first;
        region.last = up ? span.last : span.last + 1;
        const std::uint32_t n = region.last - region.first + 1;
        std::vector<std::string> contents;
        std::vector<std::string_view> breaks;
        for (std::uint32_t line = region.first; line <= region.last; ++line) {
            const LineInfo info = detail::lineAt(text, line);
            contents.push_back(text.text(info.content));
            breaks.push_back(detail::breakText(info.lineBreak));
            region.contentSize.push_back(info.content.size());
        }
        // The new order of the region's lines; the breaks stay in place.
        std::vector<std::uint32_t> order;
        if (up) {
            for (std::uint32_t k = 1; k < n; ++k) {
                order.push_back(k);
            }
            order.push_back(0);
        } else {
            order.push_back(n - 1);
            for (std::uint32_t k = 0; k + 1 < n; ++k) {
                order.push_back(k);
            }
        }
        std::string moved;
        region.newStart.assign(n, 0);
        region.newBreakSize.assign(n, 0);
        for (std::uint32_t k = 0; k < n; ++k) {
            region.newStart[order[k]] = moved.size();
            region.newBreakSize[order[k]] = breaks[k].size();
            moved += contents[order[k]];
            moved += breaks[k];
        }
        const ByteRange range{detail::lineAt(text, region.first).content.begin,
                              detail::lineEndWithBreak(detail::lineAt(text, region.last))};
        region.id = batch.add(range, std::move(moved));
        regions.push_back(std::move(region));
    }
    batch.finish();
    // An endpoint inside a region keeps its place in its line; `exclusiveEnd` marks the end of a
    // selection that stops at the start of the line after its last line (it stays after that line).
    const auto mapEndpoint = [&](ByteOffset offset, bool exclusiveEnd) {
        const Point point = text.pointAt(offset);
        const std::uint32_t line = exclusiveEnd ? point.line.value() - 1 : point.line.value();
        for (const Region& region : regions) {
            if (region.first <= line && line <= region.last) {
                const std::size_t k = line - region.first;
                const std::uint64_t column = exclusiveEnd
                                                 ? region.contentSize[k] + region.newBreakSize[k]
                                                 : std::min<std::uint64_t>(point.column.value(), region.contentSize[k]);
                return batch.post(region.id, region.newStart[k] + column);
            }
        }
        return batch.map(offset, Bias::Right);
    };
    std::vector<Selection> after;
    for (const Selection& selection : selections) {
        const ByteRange range = selection.range();
        const Point endPoint = text.pointAt(range.end);
        const bool exclusiveEnd =
            !range.empty() && endPoint.column == ByteColumn{0} && endPoint.line > text.pointAt(range.begin).line;
        const ByteOffset begin = mapEndpoint(range.begin, false);
        const ByteOffset end = mapEndpoint(range.end, exclusiveEnd);
        if (selection.empty()) {
            after.push_back(Selection::caret(begin));
        } else {
            after.push_back(selection.reversed() ? Selection{end, begin} : Selection{begin, end});
        }
    }
    return finishWith(in, batch, after);
}

// Line comments with block tokens, for languages without a line comment (XML, Markdown).
CommandResult blockCommentLines(const CommandInput& in, const LanguageInfo& language) {
    const Snapshot& text = in.text;
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    const std::string_view open = language.blockOpen;
    const std::string_view close = language.blockClose;
    BatchBuilder batch;
    for (const LineSpan& span : detail::touchedSpans(text, selections, false)) {
        bool all = true;
        bool any = false;
        for (std::uint32_t line = span.first; line <= span.last; ++line) {
            const LineInfo info = detail::lineAt(text, line);
            const ByteOffset begin = detail::indentEnd(text, info);
            const ByteOffset end = detail::trimmedEnd(text, info);
            if (begin >= end) {
                continue;
            }
            any = true;
            const std::string content = text.text(ByteRange{begin, end});
            all = all && content.size() >= open.size() + close.size() && content.starts_with(open) &&
                  content.ends_with(close);
        }
        if (!any) {
            continue;
        }
        for (std::uint32_t line = span.first; line <= span.last; ++line) {
            const LineInfo info = detail::lineAt(text, line);
            const ByteOffset begin = detail::indentEnd(text, info);
            const ByteOffset end = detail::trimmedEnd(text, info);
            if (begin >= end) {
                continue;
            }
            if (all) {
                const bool spaceAfter =
                    startsWith(text, begin + open.size(), " ") && begin + open.size() + 1 <= end - close.size();
                const bool spaceBefore = end - close.size() > begin + open.size() + (spaceAfter ? 1u : 0u) &&
                                         text.byteAt(end - close.size() - 1) == ' ';
                batch.add(ByteRange{begin, begin + open.size() + (spaceAfter ? 1u : 0u)}, {});
                batch.add(ByteRange{end - close.size() - (spaceBefore ? 1u : 0u), end}, {});
            } else {
                batch.add(ByteRange{begin, begin}, std::string(open) + " ");
                batch.add(ByteRange{end, end}, " " + std::string(close));
            }
        }
    }
    batch.finish();
    std::vector<Selection> after;
    for (const Selection& selection : selections) {
        after.push_back(mapSelection(batch, selection));
    }
    return finishWith(in, batch, after);
}

} // namespace

CommandResult duplicate(const CommandInput& in) noexcept {
    const Snapshot& text = in.text;
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    BatchBuilder batch;
    struct Target {
        std::size_t anchorId = 0;
        std::uint64_t anchorAt = 0;
        std::size_t headId = 0;
        std::uint64_t headAt = 0;
    };
    std::vector<Target> targets(selections.size());
    std::vector<Selection> carets;
    std::vector<std::size_t> caretIndex;
    for (std::size_t i = 0; i < selections.size(); ++i) {
        const Selection& selection = selections[i];
        if (selection.empty()) {
            carets.push_back(selection);
            caretIndex.push_back(i);
            continue;
        }
        const ByteRange range = selection.range();
        const std::size_t id = batch.add(ByteRange{range.end, range.end}, text.text(range));
        const std::uint64_t size = range.size();
        targets[i] = selection.reversed() ? Target{id, size, id, 0} : Target{id, 0, id, size};
    }
    std::size_t c = 0;
    for (const LineSpan& span : detail::touchedSpans(text, carets, false)) {
        const LineInfo first = detail::lineAt(text, span.first);
        const LineInfo last = detail::lineAt(text, span.last);
        const std::string_view lineBreak = last.lineBreak != LineBreak::None
                                               ? detail::breakText(last.lineBreak)
                                               : detail::documentBreak(text, in.settings.defaultLineBreak);
        std::string copy(lineBreak);
        copy += text.text(ByteRange{first.content.begin, last.content.end});
        const std::uint64_t size = copy.size();
        const std::size_t id = batch.add(ByteRange{last.content.end, last.content.end}, std::move(copy));
        for (; c < carets.size() && text.lineAt(carets[c].head).value() <= span.last; ++c) {
            const std::uint64_t at =
                std::min<std::uint64_t>(lineBreak.size() + (carets[c].head - first.content.begin), size);
            targets[caretIndex[c]] = Target{id, at, id, at};
        }
    }
    batch.finish();
    std::vector<Selection> after;
    for (const Target& target : targets) {
        after.push_back(
            Selection{batch.post(target.anchorId, target.anchorAt), batch.post(target.headId, target.headAt)});
    }
    return finishWith(in, batch, after);
}

CommandResult deleteLines(const CommandInput& in) noexcept {
    const Snapshot& text = in.text;
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    const std::uint32_t lineCount = text.lineCount();
    const std::vector<LineSpan> spans = detail::touchedSpans(text, selections, true);
    BatchBuilder batch;
    for (const LineSpan& span : spans) {
        const LineInfo first = detail::lineAt(text, span.first);
        const LineInfo last = detail::lineAt(text, span.last);
        if (span.last + 1 < lineCount) {
            batch.add(ByteRange{first.content.begin, detail::lineEndWithBreak(last)}, {});
        } else if (span.first > 0) {
            batch.add(ByteRange{detail::lineAt(text, span.first - 1).content.end, last.content.end}, {});
        } else {
            batch.add(ByteRange{ByteOffset{0}, text.size()}, {});
        }
    }
    batch.finish();
    std::vector<Selection> after;
    for (const Selection& selection : selections) {
        const Point head = text.pointAt(selection.head);
        const LineSpan* span = spanOf(spans, head.line.value());
        FROST_ASSERT(span != nullptr);
        std::optional<std::uint32_t> target;
        if (span->last + 1 < lineCount) {
            target = span->last + 1;
        } else if (span->first > 0) {
            target = span->first - 1;
        }
        ByteOffset at{0};
        if (target) {
            const LineInfo line = detail::lineAt(text, *target);
            at = text.clip(line.content.begin + std::min<std::uint64_t>(head.column.value(), line.content.size()));
        }
        after.push_back(Selection::caret(batch.map(at, Bias::Right)));
    }
    return finishWith(in, batch, after);
}

CommandResult moveLinesUp(const CommandInput& in) noexcept { return moveLines(in, true); }

CommandResult moveLinesDown(const CommandInput& in) noexcept { return moveLines(in, false); }

CommandResult joinLines(const CommandInput& in) noexcept {
    const Snapshot& text = in.text;
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    const std::uint32_t lineCount = text.lineCount();
    // The lines k joined with k + 1, and for each selection the first one it joins.
    std::vector<std::uint32_t> pairs;
    std::vector<std::optional<std::uint32_t>> firstPair;
    for (const Selection& selection : selections) {
        const LineSpan span = detail::touchedLines(text, selection);
        const std::uint32_t end = span.first == span.last ? span.first + 1 : span.last;
        firstPair.push_back(span.first + 1 < lineCount ? std::optional<std::uint32_t>(span.first) : std::nullopt);
        for (std::uint32_t k = span.first; k < end && k + 1 < lineCount; ++k) {
            pairs.push_back(k);
        }
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    BatchBuilder batch;
    std::vector<std::size_t> ids;
    for (const std::uint32_t k : pairs) {
        const LineInfo current = detail::lineAt(text, k);
        const LineInfo next = detail::lineAt(text, k + 1);
        const ByteOffset from = detail::trimmedEnd(text, current);
        const ByteOffset to = detail::indentEnd(text, next);
        const bool currentBlank = detail::indentEnd(text, current) == current.content.end;
        const bool nextBlank = to == next.content.end;
        const std::optional<DecodedChar> last = from > current.content.begin ? charBefore(text, from) : std::nullopt;
        const std::optional<DecodedChar> first = nextBlank ? std::nullopt : charAt(text, to);
        const bool tight = currentBlank || nextBlank || (last && (last->cp == U'(' || last->cp == U'[')) ||
                           (first && std::u32string_view(U")]},;.").find(first->cp) != std::u32string_view::npos);
        ids.push_back(batch.add(ByteRange{from, to}, tight ? std::string() : std::string(" ")));
    }
    batch.finish();
    std::vector<Selection> after;
    for (std::size_t i = 0; i < selections.size(); ++i) {
        const Selection& selection = selections[i];
        if (selection.empty() && firstPair[i]) {
            const auto found = std::lower_bound(pairs.begin(), pairs.end(), *firstPair[i]);
            after.push_back(Selection::caret(batch.post(ids[static_cast<std::size_t>(found - pairs.begin())], 0)));
        } else {
            after.push_back(mapSelection(batch, selection));
        }
    }
    return finishWith(in, batch, after);
}

CommandResult toggleLineComment(const CommandInput& in) noexcept {
    const LanguageInfo& language = languageOf(in);
    if (!language.hasLineComment()) {
        if (language.hasBlockComment()) {
            return blockCommentLines(in, language);
        }
        return CommandResult{{}, in.state, EditKind::Other, false};
    }
    const Snapshot& text = in.text;
    const std::string_view token = language.lineComment;
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    BatchBuilder batch;
    for (const LineSpan& span : detail::touchedSpans(text, selections, false)) {
        bool all = true;
        bool any = false;
        std::uint64_t indent = std::numeric_limits<std::uint64_t>::max();
        for (std::uint32_t line = span.first; line <= span.last; ++line) {
            const LineInfo info = detail::lineAt(text, line);
            const ByteOffset start = detail::indentEnd(text, info);
            if (start == info.content.end) {
                continue; // Blank lines neither decide nor change.
            }
            any = true;
            indent = std::min<std::uint64_t>(indent, start - info.content.begin);
            all = all && startsWith(text, start, token);
        }
        if (!any) {
            continue;
        }
        for (std::uint32_t line = span.first; line <= span.last; ++line) {
            const LineInfo info = detail::lineAt(text, line);
            const ByteOffset start = detail::indentEnd(text, info);
            if (start == info.content.end) {
                continue;
            }
            if (all) {
                const std::uint64_t remove = token.size() + (startsWith(text, start + token.size(), " ") ? 1u : 0u);
                batch.add(ByteRange{start, std::min(start + remove, info.content.end)}, {});
            } else {
                const ByteOffset at = info.content.begin + indent;
                batch.add(ByteRange{at, at}, std::string(token) + " ");
            }
        }
    }
    batch.finish();
    std::vector<Selection> after;
    for (const Selection& selection : selections) {
        if (!selection.empty()) {
            after.push_back(mapSelection(batch, selection));
            continue;
        }
        // A caret moves down one line, keeping its column (IntelliJ).
        const Point head = text.pointAt(selection.head);
        if (head.line.value() + 1 < text.lineCount()) {
            const LineInfo next = detail::lineAt(text, head.line.value() + 1);
            const ByteOffset at =
                text.clip(next.content.begin + std::min<std::uint64_t>(head.column.value(), next.content.size()));
            after.push_back(Selection::caret(batch.map(at, Bias::Left)));
        } else {
            after.push_back(Selection::caret(batch.map(selection.head, Bias::Right)));
        }
    }
    return finishWith(in, batch, after);
}

CommandResult toggleBlockComment(const CommandInput& in) noexcept {
    const LanguageInfo& language = languageOf(in);
    if (!language.hasBlockComment()) {
        return toggleLineComment(in);
    }
    const Snapshot& text = in.text;
    const std::string_view open = language.blockOpen;
    const std::string_view close = language.blockClose;
    const std::vector<Selection> selections = detail::selectionsOf(in.state);
    BatchBuilder batch;
    // Per selection: either mapped through the batch, or the inner range of a new comment.
    struct Plan {
        bool wrapped = false;
        std::size_t openId = 0;
        std::size_t closeId = 0;
    };
    std::vector<Plan> plans(selections.size());
    for (std::size_t i = 0; i < selections.size(); ++i) {
        const Selection& selection = selections[i];
        const ByteRange range = selection.range();
        if (!range.empty()) {
            const std::string inner = text.text(range);
            if (inner.size() >= open.size() + close.size() && inner.starts_with(open) && inner.ends_with(close)) {
                batch.add(ByteRange{range.begin, range.begin + open.size()}, {});
                batch.add(ByteRange{range.end - close.size(), range.end}, {});
            } else if (range.begin - ByteOffset{0} >= open.size() &&
                       startsWith(text, range.begin - open.size(), open) && startsWith(text, range.end, close)) {
                batch.add(ByteRange{range.begin - open.size(), range.begin}, {});
                batch.add(ByteRange{range.end, range.end + close.size()}, {});
            } else {
                plans[i] = Plan{true, batch.add(ByteRange{range.begin, range.begin}, std::string(open)),
                                batch.add(ByteRange{range.end, range.end}, std::string(close))};
            }
            continue;
        }
        // A caret inside a block comment on its line unwraps it.
        const ByteOffset at = range.begin;
        const LineInfo line = detail::lineOf(text, at);
        const std::string before = text.text(ByteRange{line.content.begin, std::min(at, line.content.end)});
        const std::string afterText = text.text(ByteRange{std::min(at, line.content.end), line.content.end});
        const std::size_t o = before.rfind(open);
        const std::size_t c = afterText.find(close);
        if (o != std::string::npos && c != std::string::npos &&
            before.find(close, o + open.size()) == std::string::npos &&
            afterText.substr(0, c).find(open) == std::string::npos) {
            batch.add(ByteRange{line.content.begin + o, line.content.begin + o + open.size()}, {});
            const ByteOffset closeAt = std::min(at, line.content.end) + c;
            batch.add(ByteRange{closeAt, closeAt + close.size()}, {});
        } else {
            plans[i] = Plan{true, batch.add(ByteRange{at, at}, std::string(open)), 0};
            plans[i].closeId = batch.add(ByteRange{at, at}, std::string(close));
        }
    }
    batch.finish();
    std::vector<Selection> after;
    for (std::size_t i = 0; i < selections.size(); ++i) {
        const Selection& selection = selections[i];
        if (!plans[i].wrapped) {
            after.push_back(mapSelection(batch, selection));
            continue;
        }
        const ByteOffset begin = batch.post(plans[i].openId, open.size());
        const ByteOffset end = batch.post(plans[i].closeId, 0);
        after.push_back(selection.reversed() ? Selection{end, begin} : Selection{begin, end});
    }
    return finishWith(in, batch, after);
}

} // namespace frost::text::commands
