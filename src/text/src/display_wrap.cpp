// SPDX-License-Identifier: Apache-2.0
//
// The soft-wrap layer (display_layers.hpp): greedy line wrapping through an ITextShaper, the wrap
// tree's construction, its splits at line starts, the row queries and the incremental update.

#include "display_layers.hpp"

#include <format>

namespace frost::text::detail {

namespace {

constexpr char32_t kZeroWidthJoiner = 0x200D;

[[nodiscard]] bool isSpace(char32_t cp) noexcept { return cp == U' ' || cp == U'\t'; }

/*
 * Greedy wrapping of one line, fed one code point at a time (display_map.hpp's rules). It keeps the
 * code points after the last break opportunity of the current row, so that a break there can lay
 * them out again from x = 0 (tabs depend on x): each code point is measured at most twice.
 */
class LineWrapper {
public:
    LineWrapper(const ITextShaper& shaper, TextWidth width, std::uint64_t lineStart,
                std::vector<std::uint64_t>& breaks) noexcept
        : m_shaper(shaper), m_width(width), m_rowStart(lineStart), m_breaks(breaks) {}

    void step(char32_t cp, std::uint64_t at) noexcept {
        const CharWidth width = charWidth(cp);
        const bool space = isSpace(cp);
        const bool attaches = width == CharWidth::Zero || m_afterJoiner;
        if (!attaches && !space && at > m_rowStart &&
            (m_prev == Prev::Space || m_prev == Prev::Wide || width == CharWidth::Wide)) {
            m_hasOpportunity = true;
            m_opportunity = at;
            m_carried.clear();
        }
        TextWidth advance = m_shaper.advance(cp, m_x);
        if (!attaches && !space && at > m_rowStart && m_x + advance > m_width) {
            if (m_hasOpportunity) {
                breakAt(m_opportunity);
                for (const char32_t carried : m_carried) {
                    m_x += m_shaper.advance(carried, m_x);
                }
            } else {
                breakAt(at);
            }
            m_hasOpportunity = false;
            m_carried.clear();
            advance = m_shaper.advance(cp, m_x);
            if (m_x > 0 && m_x + advance > m_width) {
                // The carried word alone overflows the row: break again right here.
                breakAt(at);
                advance = m_shaper.advance(cp, 0);
            }
        }
        m_x += advance;
        if (m_hasOpportunity) {
            m_carried.push_back(cp);
        }
        if (!attaches) {
            m_prev = space ? Prev::Space : (width == CharWidth::Wide ? Prev::Wide : Prev::Other);
        }
        m_afterJoiner = cp == kZeroWidthJoiner;
    }

private:
    enum class Prev : std::uint8_t { None, Space, Wide, Other };

    void breakAt(std::uint64_t at) noexcept {
        m_breaks.push_back(at);
        m_rowStart = at;
        m_x = 0;
    }

    const ITextShaper& m_shaper;
    TextWidth m_width;
    std::uint64_t m_rowStart;
    std::vector<std::uint64_t>& m_breaks;
    TextWidth m_x = 0;
    Prev m_prev = Prev::None;
    bool m_afterJoiner = false;
    bool m_hasOpportunity = false;
    std::uint64_t m_opportunity = 0;
    std::vector<char32_t> m_carried; // Code points after the opportunity, laid out again on a break.
};

[[nodiscard]] WrapTree single(const WrapItem& item) noexcept { return WrapTree::fromItems(std::span(&item, 1)); }

// a followed by b, merging the seam's two items when a ends with a line break (WrapItem's rules
// allow any item to follow one), so unwrapped text stays one item across edits.
[[nodiscard]] WrapTree concatMerged(const WrapTree& a, const WrapTree& b) noexcept {
    if (a.empty() || b.empty()) {
        return a.empty() ? b : a;
    }
    const WrapItem last = *a.back();
    if (last.lineBreaks != last.rows) {
        return WrapTree::concat(a, b);
    }
    const WrapItem first = *b.front();
    const WrapItem merged{last.bytes + first.bytes, last.rows + first.rows, last.lineBreaks + first.lineBreaks};
    return WrapTree::concat(WrapTree::concat(a.withoutBack(), single(merged)), b.withoutFront());
}

// (rows before fold line `line`, rows from it) of `tree` (whose prefix sums are absolute). An item
// spanning the line start is cut there. O(B log n + P).
[[nodiscard]] std::pair<WrapTree, WrapTree> splitAtLine(const WrapTree& tree, std::uint32_t line,
                                                        const FoldLayer& fold) noexcept {
    if (line == 0) {
        return {WrapTree(), tree};
    }
    if (line > tree.summary().lineBreaks) {
        return {tree, WrapTree()};
    }
    // The item holding line break number `line` (1-based); its row k - 1 ends with it.
    auto [left, right] = tree.split([line](const WrapSummary& s) { return s.lineBreaks >= line; });
    const WrapItem item = *right.front();
    const WrapSummary prefix = left.summary();
    const std::uint32_t k = line - prefix.lineBreaks;
    if (k == item.rows) {
        return {WrapTree::concat(left, single(item)), right.withoutFront()};
    }
    const std::uint64_t headBytes = fold.line(line).start - prefix.bytes;
    const WrapItem head{headBytes, k, k};
    const WrapItem tail{item.bytes - headBytes, item.rows - k, item.lineBreaks - k};
    return {WrapTree::concat(left, single(head)), WrapTree::concat(single(tail), right.withoutFront())};
}

// The rows of fold lines [first, end) of `fold` as a tree, wrapped at `width` (0: off).
[[nodiscard]] WrapTree buildLines(const FoldLayer& fold, std::uint32_t first, std::uint32_t end,
                                  const ITextShaper& shaper, TextWidth width) noexcept {
    FROST_ASSERT(first < end && end <= fold.lineCount());
    const bool withLastLine = end == fold.lineCount();
    if (width <= 0) {
        const std::uint64_t begin = fold.line(first).start;
        const std::uint64_t stop = withLastLine ? fold.size() : fold.line(end).start;
        const std::uint32_t rows = end - first;
        return single(WrapItem{stop - begin, rows, withLastLine ? rows - 1 : rows});
    }
    std::vector<WrapItem> items;
    std::vector<std::uint64_t> breaks;
    WrapItem current;
    fold.forEachLine(first, end, [&](std::uint32_t, const FoldLine& info) {
        breaks.clear();
        wrapLine(fold, info, shaper, width, breaks);
        const std::uint32_t hasBreak = info.end > info.contentEnd ? 1 : 0;
        if (breaks.empty()) {
            current.bytes += info.end - info.start;
            current.rows += 1;
            current.lineBreaks += hasBreak;
            return;
        }
        // The line's first row closes the current item at the first soft wrap; each further row is
        // an item of its own; the last row opens the next item.
        current.bytes += breaks.front() - info.start;
        current.rows += 1;
        items.push_back(current);
        for (std::size_t i = 1; i < breaks.size(); ++i) {
            items.push_back(WrapItem{breaks[i] - breaks[i - 1], 1, 0});
        }
        current = WrapItem{info.end - breaks.back(), 1, hasBreak};
    });
    items.push_back(current);
    return WrapTree::fromItems(items);
}

} // namespace

void wrapLine(const FoldLayer& fold, const FoldLine& line, const ITextShaper& shaper, TextWidth width,
              std::vector<std::uint64_t>& breaks) noexcept {
    if (width <= 0 || line.contentEnd == line.start) {
        return;
    }
    LineWrapper wrapper(shaper, width, line.start, breaks);
    std::uint64_t at = line.start;
    fold.forEachChunk(line.start, line.contentEnd, [&](std::string_view chunk, SegmentKind) {
        for (std::size_t i = 0; i < chunk.size();) {
            const DecodedChar decoded = decodeUtf8(chunk.substr(i));
            wrapper.step(decoded.cp, at);
            at += decoded.bytes;
            i += decoded.bytes;
        }
    });
}

WrapLayer::WrapLayer(const FoldLayer& fold, const ITextShaper& shaper, TextWidth width) noexcept
    : m_tree(buildLines(fold, 0, fold.lineCount(), shaper, width)), m_width(width) {}

WrapRow WrapLayer::row(std::uint32_t row, const FoldLayer& fold) const noexcept {
    FROST_PRE(row < rowCount());
    const WrapTree::Cursor cursor = m_tree.seek([row](const WrapSummary& s) { return s.rows > row; });
    FROST_ASSERT(cursor.item() != nullptr);
    const WrapItem& item = *cursor.item();
    const WrapSummary& prefix = cursor.prefix();
    const std::uint32_t j = row - prefix.rows;
    const std::uint32_t line = prefix.lineBreaks + j;
    const FoldLine info = fold.line(line);
    WrapRow out;
    out.line = line;
    out.start = j == 0 ? prefix.bytes : info.start;
    const bool softEnd = j + 1 == item.rows && item.lineBreaks < item.rows;
    out.end = softEnd ? prefix.bytes + item.bytes : info.contentEnd;
    out.startsLine = out.start == info.start;
    out.endsLine = out.end == info.contentEnd;
    return out;
}

std::uint32_t WrapLayer::rowAt(std::uint64_t offset, Bias bias, const FoldLayer& fold) const noexcept {
    const WrapSummary& total = m_tree.summary();
    if (offset >= total.bytes) {
        return total.rows - 1;
    }
    const WrapTree::Cursor cursor = m_tree.seek([offset](const WrapSummary& s) { return s.bytes > offset; });
    FROST_ASSERT(cursor.item() != nullptr);
    const WrapSummary& prefix = cursor.prefix();
    const std::uint32_t line = fold.lineAt(offset);
    const std::uint32_t j = line - prefix.lineBreaks;
    FROST_ASSERT(j < cursor.item()->rows);
    const std::uint32_t row = prefix.rows + j;
    // An item that starts inside a line starts at a soft wrap: Left keeps the offset on the row above.
    if (bias == Bias::Left && j == 0 && offset == prefix.bytes && row > 0 && offset != fold.line(line).start) {
        return row - 1;
    }
    return row;
}

std::uint32_t WrapLayer::firstRowOfLine(std::uint32_t line) const noexcept {
    if (line == 0) {
        return 0;
    }
    const WrapSummary& total = m_tree.summary();
    if (line > total.lineBreaks) {
        return total.rows;
    }
    const WrapTree::Cursor cursor = m_tree.seek([line](const WrapSummary& s) { return s.lineBreaks >= line; });
    FROST_ASSERT(cursor.item() != nullptr);
    return cursor.prefix().rows + (line - cursor.prefix().lineBreaks);
}

WrapLayer WrapLayer::updated(const FoldLayer& oldFold, const FoldLayer& newFold, std::span<const SpanEdit> lines,
                             const ITextShaper& shaper, std::vector<SpanEdit>& rows) const noexcept {
    // The new tree is the old one's untouched line ranges with the edited lines wrapped again in
    // between: every split is a prefix of the old tree, so its sums stay absolute.
    WrapTree result;
    std::uint32_t kept = 0; // Old lines before `kept` are in `result` or replaced.
    for (const SpanEdit& edit : lines) {
        const WrapTree before = splitAtLine(m_tree, edit.oldFirst, oldFold).first;
        result = concatMerged(result, splitAtLine(before, kept, oldFold).second);
        const std::uint32_t oldRowFirst = firstRowOfLine(edit.oldFirst);
        const std::uint32_t oldRowEnd = firstRowOfLine(edit.oldEnd);
        const WrapTree middle = buildLines(newFold, edit.newFirst, edit.newEnd, shaper, m_width);
        const std::uint32_t newRowFirst = result.summary().rows;
        result = concatMerged(result, middle);
        rows.push_back(SpanEdit{oldRowFirst, oldRowEnd, newRowFirst, newRowFirst + middle.summary().rows});
        kept = edit.oldEnd;
    }
    result = concatMerged(result, splitAtLine(m_tree, kept, oldFold).second);
    return WrapLayer(std::move(result), m_width);
}

Result<void> WrapLayer::checkInvariants(const FoldLayer& fold) const noexcept {
    if (Result<void> tree = m_tree.checkInvariants(); !tree) {
        return tree;
    }
    const WrapSummary& total = m_tree.summary();
    if (total.bytes != fold.size() || total.lineBreaks + 1 != fold.lineCount()) {
        return fail(ErrorCode::Internal,
                    std::format("D2: the wrap rows cover {} bytes and {} line breaks, the folded text has {} and {}",
                                total.bytes, total.lineBreaks, fold.size(), fold.lineCount() - 1));
    }
    std::uint64_t offset = 0;
    std::uint32_t line = 0;
    std::size_t index = 0;
    for (WrapTree::Cursor cursor = m_tree.first(); cursor.item() != nullptr; cursor.next(), ++index) {
        const WrapItem& item = *cursor.item();
        if (item.rows == 0 || (item.lineBreaks != item.rows && item.lineBreaks + 1 != item.rows)) {
            return fail(ErrorCode::Internal, std::format("D2: wrap item {} has {} rows and {} line breaks", index,
                                                         item.rows, item.lineBreaks));
        }
        const std::uint64_t end = offset + item.bytes;
        const std::uint32_t lastLine = line + item.rows - 1;
        const FoldLine last = fold.line(lastLine);
        if (item.lineBreaks == item.rows) {
            if (end != last.end) {
                return fail(
                    ErrorCode::Internal,
                    std::format("D2: wrap item {} ends at {}, its last line break ends at {}", index, end, last.end));
            }
        } else if (end != fold.size() && (end <= last.start || end >= last.contentEnd)) {
            return fail(ErrorCode::Internal, std::format("D2: wrap item {} ends at {}, outside line {} [{}, {})", index,
                                                         end, lastLine, last.start, last.contentEnd));
        }
        offset = end;
        line += item.lineBreaks;
    }
    return {};
}

} // namespace frost::text::detail
