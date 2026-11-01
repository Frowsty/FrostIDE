// SPDX-License-Identifier: Apache-2.0
//
// Piece-tree algorithms (see piece_tree.hpp): piece building, byte splits, seeks, the in-piece
// count-only scanners behind the position queries, range summaries and the line scanner.
/*
 * Every query is a seek by a monotone predicate on prefix summaries (bytes > x, lineBreaks >= L or
 * utf16 > u) followed by a scan inside one piece (at most P bytes) with count-only scanners that
 * carry "the previous byte was CR" across the piece boundary. A full summarize() runs only where a
 * TextSummary is needed: cutting pieces and summarizeRange's boundary pieces.
 *
 * CRLF handling: a CR and an LF may sit in different pieces. Breaks are counted at their first
 * byte (a CR, or an LF not preceded by a CR), which is also how combine() counts, so "the piece
 * whose prefix-through reaches lineBreaks >= k" holds break k's first byte. An offset strictly
 * between a CR and its LF reports the CR's point; when the CR ends the previous piece that takes a
 * second descent.
 */

#include "piece_tree.hpp"

#include "frost/core/assert.hpp"

#include <algorithm>
#include <cstring>

namespace frost::text::detail {
namespace {

using Cursor = PieceTree::Cursor;
constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

// UTF-16 units of `bytes` by the per-byte rule (lead bytes count 1, bytes >= 0xF0 one more).
std::uint64_t utf16Units(std::string_view bytes) noexcept {
    std::uint64_t units = 0;
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        units += (b & 0xC0u) != 0x80u ? 1u : 0u;
        units += b >= 0xF0u ? 1u : 0u;
    }
    return units;
}

// Line breaks and the start of the current line within piece bytes [0, end), continuing from
// `prefix` (the summary of everything before the piece).
struct BreakScan {
    std::uint32_t breaks = 0;   // Breaks before `end`, including the prefix's.
    std::size_t lineStart = 0;  // Where the line containing `end` starts in the piece...
    bool startsInPiece = false; // ...if it starts in the piece at all.
};

BreakScan scanBreaks(const TextSummary& prefix, std::string_view piece, std::size_t end) noexcept {
    BreakScan scan{prefix.lineBreaks, 0, false};
    bool carry = prefix.endsWithCr; // The previous byte was CR.
    for (std::size_t i = 0; i < end; ++i) {
        const char b = piece[i];
        if (b == '\r') {
            ++scan.breaks;
            scan.lineStart = i + 1;
            scan.startsInPiece = true;
            carry = true;
        } else if (b == '\n') {
            if (!carry) {
                ++scan.breaks; // An LF after a CR completes that CR's break.
            }
            scan.lineStart = i + 1;
            scan.startsInPiece = true;
            carry = false;
        } else {
            carry = false;
        }
    }
    return scan;
}

// Steps the offset `o` of piece bytes back to a char boundary (at most 3 bytes; a piece starts at
// a char boundary).
std::size_t floorInPiece(std::string_view piece, std::size_t o) noexcept {
    while (o > 0 && o < piece.size() && isContinuation(piece[o])) {
        --o;
    }
    return o;
}

// The piece holding the first byte of break number `k` (1-based), and that byte's index in it.
struct BreakAt {
    Cursor cursor;
    std::size_t index = 0;
};

BreakAt findBreak(const PieceTree& tree, std::uint32_t k) noexcept {
    BreakAt at{seekLineBreak(tree, k), 0};
    FROST_ASSERT(at.cursor.item() != nullptr);
    const TextSummary& prefix = at.cursor.prefix();
    const std::string_view piece = at.cursor.item()->view();
    std::uint32_t count = prefix.lineBreaks;
    bool carry = prefix.endsWithCr;
    for (std::size_t i = 0; i < piece.size(); ++i) {
        const char b = piece[i];
        if (b == '\r') {
            if (++count == k) {
                at.index = i;
                return at;
            }
            carry = true;
        } else if (b == '\n') {
            if (!carry && ++count == k) {
                at.index = i;
                return at;
            }
            carry = false;
        } else {
            carry = false;
        }
    }
    FROST_ASSERT(false); // The piece found by lineBreaks holds the break.
    return at;
}

// The kind of the break whose first byte is at `index` of the cursor's piece, and the offset just
// after it. A CR at the end of a piece is a CRLF when the next piece starts with LF.
struct BreakEnd {
    LineBreak kind = LineBreak::None;
    std::uint64_t next = 0;
};

BreakEnd breakAfter(const Cursor& cursor, std::size_t index) noexcept {
    const std::string_view piece = cursor.item()->view();
    const std::uint64_t at = cursor.prefix().bytes + index;
    if (piece[index] == '\n') {
        return {LineBreak::Lf, at + 1};
    }
    if (index + 1 < piece.size()) {
        return piece[index + 1] == '\n' ? BreakEnd{LineBreak::CrLf, at + 2} : BreakEnd{LineBreak::Cr, at + 1};
    }
    Cursor peek = cursor;
    if (peek.next() && peek.item()->data[0] == '\n') {
        return {LineBreak::CrLf, at + 2};
    }
    return {LineBreak::Cr, at + 1};
}

// Advances `cursor` (at or before the piece containing `offset`) to that piece, or to the end
// when offset is the size. At most a few steps: callers move by at most one break.
void advanceTo(Cursor& cursor, std::uint64_t offset) noexcept {
    while (cursor.item() != nullptr && cursor.prefix().bytes + cursor.item()->size() <= offset) {
        cursor.next();
    }
}

TextSummary summarizeNode(const PieceTree::Node* node, std::uint64_t start, std::uint64_t a, std::uint64_t b) noexcept {
    const std::uint64_t end = start + node->summary.bytes;
    if (a <= start && end <= b) {
        return node->summary;
    }
    TextSummary acc{};
    std::uint64_t position = start;
    if (node->height == 0) {
        const auto* leaf = static_cast<const PieceTree::Leaf*>(node);
        for (std::size_t i = 0; i < leaf->count && position < b; ++i) {
            const Piece& piece = leaf->items[i];
            const std::uint64_t pieceEnd = position + piece.summary.bytes;
            if (pieceEnd > a) {
                const std::uint64_t lo = std::max(a, position);
                const std::uint64_t hi = std::min(b, pieceEnd);
                acc = combine(acc, lo == position && hi == pieceEnd
                                       ? piece.summary
                                       : summarize(piece.view().substr(lo - position, hi - lo)));
            }
            position = pieceEnd;
        }
        return acc;
    }
    const auto* internal = static_cast<const PieceTree::Internal*>(node);
    for (std::size_t i = 0; i < internal->count && position < b; ++i) {
        const std::uint64_t childEnd = position + internal->childSummaries[i].bytes;
        if (childEnd > a) {
            acc = combine(acc, summarizeNode(internal->children[i].get(), position, a, b));
        }
        position = childEnd;
    }
    return acc;
}

void addStats(const PieceTree::Node* node, TreeStats& stats) noexcept {
    if (node->height == 0) {
        ++stats.leaves;
        stats.pieces += node->count;
        stats.nodeBytes += sizeof(PieceTree::Leaf);
        return;
    }
    ++stats.internals;
    stats.nodeBytes += sizeof(PieceTree::Internal);
    const auto* internal = static_cast<const PieceTree::Internal*>(node);
    for (std::size_t i = 0; i < internal->count; ++i) {
        addStats(internal->children[i].get(), stats);
    }
}

} // namespace

Piece makePiece(const char* data, std::size_t size) noexcept { return Piece{data, summarize({data, size})}; }

void appendPieces(std::vector<Piece>& out, const char* data, std::size_t size, std::uint32_t maxPieceBytes) noexcept {
    FROST_PRE(maxPieceBytes >= 4);
    std::size_t position = 0;
    while (position < size) {
        std::size_t end = std::min(size, position + maxPieceBytes);
        // Back up to a char boundary; at most 3 bytes, so with a cap of at least 4 every piece
        // keeps at least one character.
        while (end < size && isContinuation(data[end])) {
            --end;
        }
        out.push_back(makePiece(data + position, end - position));
        position = end;
    }
}

std::vector<Piece> summarizePieces(std::string_view stored, std::uint32_t maxPieceBytes) noexcept {
    std::vector<Piece> pieces;
    pieces.reserve(stored.size() / maxPieceBytes + 1);
    appendPieces(pieces, stored.data(), stored.size(), maxPieceBytes);
    return pieces;
}

PieceTree buildTree(std::span<const Piece> pieces) noexcept { return PieceTree::fromItems(pieces); }

PieceTree singlePiece(const Piece& piece) noexcept { return PieceTree::fromItems(std::span<const Piece>(&piece, 1)); }

std::pair<PieceTree, PieceTree> splitAtByte(const PieceTree& tree, std::uint64_t x) noexcept {
    const std::uint64_t size = tree.summary().bytes;
    if (x == 0) {
        return {PieceTree(), tree};
    }
    if (x >= size) {
        return {tree, PieceTree()};
    }
    auto [left, right] = tree.split([x](const TextSummary& s) { return s.bytes > x; });
    const std::uint64_t leftBytes = left.summary().bytes;
    if (leftBytes == x) {
        return {std::move(left), std::move(right)};
    }
    // The first piece of `right` straddles x: cut it there (x is a char boundary, so both halves
    // are well-formed). Summaries are not subtractable (the longest line), so both halves are
    // summarised again: O(P).
    const Piece straddling = *right.front();
    const auto cut = static_cast<std::size_t>(x - leftBytes);
    FROST_ASSERT(cut > 0 && cut < straddling.size() && !isContinuation(straddling.data[cut]));
    const Piece head = makePiece(straddling.data, cut);
    const Piece tail = makePiece(straddling.data + cut, straddling.size() - cut);
    PieceTree newLeft = PieceTree::concat(left, singlePiece(head));
    PieceTree newRight = PieceTree::concat(singlePiece(tail), right.withoutFront());
    return {std::move(newLeft), std::move(newRight)};
}

Cursor seekByte(const PieceTree& tree, std::uint64_t x) noexcept {
    return tree.seek([x](const TextSummary& s) { return s.bytes > x; });
}

Cursor seekLineBreak(const PieceTree& tree, std::uint32_t breaks) noexcept {
    return tree.seek([breaks](const TextSummary& s) { return s.lineBreaks >= breaks; });
}

Cursor seekUtf16(const PieceTree& tree, std::uint64_t units) noexcept {
    return tree.seek([units](const TextSummary& s) { return s.utf16 > units; });
}

char byteAt(const PieceTree& tree, std::uint64_t x) noexcept {
    FROST_PRE(x < tree.summary().bytes);
    const Cursor cursor = seekByte(tree, x);
    return cursor.item()->data[x - cursor.prefix().bytes];
}

Point pointAt(const PieceTree& tree, std::uint64_t x) noexcept {
    const TextSummary& total = tree.summary();
    FROST_PRE(x <= total.bytes);
    if (x == total.bytes) {
        return {LineIndex{total.lineBreaks}, ByteColumn{total.lastLineBytes}};
    }
    const Cursor cursor = seekByte(tree, x);
    const TextSummary& prefix = cursor.prefix();
    const std::string_view piece = cursor.item()->view();
    auto o = static_cast<std::size_t>(x - prefix.bytes);
    const bool afterCr = o > 0 ? piece[o - 1] == '\r' : prefix.endsWithCr;
    if (afterCr && piece[o] == '\n') {
        // CRLF-interior: answer for the CR. A CR ending the previous piece is a second descent.
        if (o == 0) {
            return pointAt(tree, x - 1);
        }
        --o;
    }
    const BreakScan scan = scanBreaks(prefix, piece, o);
    const std::uint64_t column = scan.startsInPiece ? o - scan.lineStart : prefix.lastLineBytes + o;
    return {LineIndex{scan.breaks}, ByteColumn{static_cast<std::uint32_t>(column)}};
}

Utf16Point utf16PointAt(const PieceTree& tree, std::uint64_t x) noexcept {
    const TextSummary& total = tree.summary();
    FROST_PRE(x <= total.bytes);
    if (x == total.bytes) {
        return {LineIndex{total.lineBreaks}, Utf16Column{total.lastLineUtf16}};
    }
    const Cursor cursor = seekByte(tree, x);
    const TextSummary& prefix = cursor.prefix();
    const std::string_view piece = cursor.item()->view();
    std::size_t o = floorInPiece(piece, static_cast<std::size_t>(x - prefix.bytes));
    const bool afterCr = o > 0 ? piece[o - 1] == '\r' : prefix.endsWithCr;
    if (afterCr && piece[o] == '\n') {
        if (o == 0) {
            return utf16PointAt(tree, prefix.bytes - 1);
        }
        --o;
    }
    const BreakScan scan = scanBreaks(prefix, piece, o);
    const std::uint64_t column = scan.startsInPiece ? utf16Units(piece.substr(scan.lineStart, o - scan.lineStart))
                                                    : prefix.lastLineUtf16 + utf16Units(piece.substr(0, o));
    return {LineIndex{scan.breaks}, Utf16Column{static_cast<std::uint32_t>(column)}};
}

std::uint64_t utf16At(const PieceTree& tree, std::uint64_t x) noexcept {
    const TextSummary& total = tree.summary();
    FROST_PRE(x <= total.bytes);
    if (x == total.bytes) {
        return total.utf16;
    }
    const Cursor cursor = seekByte(tree, x);
    const std::string_view piece = cursor.item()->view();
    const std::size_t o = floorInPiece(piece, static_cast<std::size_t>(x - cursor.prefix().bytes));
    return cursor.prefix().utf16 + utf16Units(piece.substr(0, o));
}

std::uint64_t clip(const PieceTree& tree, std::uint64_t x) noexcept {
    const std::uint64_t size = tree.summary().bytes;
    if (x >= size) {
        return size;
    }
    const Cursor cursor = seekByte(tree, x);
    const std::string_view piece = cursor.item()->view();
    return cursor.prefix().bytes + floorInPiece(piece, static_cast<std::size_t>(x - cursor.prefix().bytes));
}

bool isCharBoundary(const PieceTree& tree, std::uint64_t x) noexcept {
    const std::uint64_t size = tree.summary().bytes;
    if (x > size) {
        return false;
    }
    return x == size || !isContinuation(byteAt(tree, x));
}

std::uint64_t offsetAtUtf16(const PieceTree& tree, std::uint64_t units) noexcept {
    const TextSummary& total = tree.summary();
    FROST_PRE(units <= total.utf16);
    if (units == total.utf16) {
        return total.bytes;
    }
    const Cursor cursor = seekUtf16(tree, units);
    const std::string_view piece = cursor.item()->view();
    const std::uint64_t target = units - cursor.prefix().utf16;
    std::size_t position = 0;
    std::uint64_t counted = 0;
    while (position < piece.size()) {
        const auto lead = static_cast<unsigned char>(piece[position]);
        const std::size_t length = lead < 0x80u ? 1 : lead < 0xE0u ? 2 : lead < 0xF0u ? 3 : 4;
        const std::uint64_t charUnits = length == 4 ? 2 : 1;
        if (counted + charUnits > target) {
            break; // `units` is this character's first unit or, inside a pair, its second.
        }
        counted += charUnits;
        position += length;
    }
    return cursor.prefix().bytes + position;
}

TextSummary summarizeRange(const PieceTree& tree, std::uint64_t a, std::uint64_t b) noexcept {
    if (a >= b || tree.empty()) {
        return {};
    }
    return summarizeNode(tree.rootNode(), 0, a, b);
}

LineScan lineScanAt(const PieceTree& tree, std::uint32_t line) noexcept {
    FROST_PRE(line <= tree.summary().lineBreaks);
    LineScan scan;
    scan.line = line;
    if (line == 0) {
        scan.cursor = tree.first();
        return scan;
    }
    BreakAt at = findBreak(tree, line);
    const BreakEnd end = breakAfter(at.cursor, at.index);
    advanceTo(at.cursor, end.next);
    scan.cursor = at.cursor;
    scan.position = end.next;
    return scan;
}

LineInfo nextLine(const PieceTree& tree, LineScan& scan) noexcept {
    const TextSummary& total = tree.summary();
    FROST_PRE(scan.line <= total.lineBreaks);
    LineInfo info;
    info.line = LineIndex{scan.line};
    info.content.begin = ByteOffset{scan.position};
    if (scan.line == total.lineBreaks) {
        info.content.end = ByteOffset{total.bytes};
        info.lineBreak = LineBreak::None;
        ++scan.line;
        return info;
    }
    // The first CR or LF in the rest of the current piece ends this line (content holds none).
    Cursor cursor = scan.cursor;
    std::size_t index = kNotFound;
    if (cursor.item() != nullptr) {
        const std::string_view piece = cursor.item()->view();
        for (auto i = static_cast<std::size_t>(scan.position - cursor.prefix().bytes); i < piece.size(); ++i) {
            if (piece[i] == '\n' || piece[i] == '\r') {
                index = i;
                break;
            }
        }
    }
    if (index == kNotFound) {
        // No break follows in this piece: seek the line's break directly (O(B log n + P)).
        BreakAt at = findBreak(tree, scan.line + 1);
        cursor = at.cursor;
        index = at.index;
    }
    const std::uint64_t breakStart = cursor.prefix().bytes + index;
    const BreakEnd end = breakAfter(cursor, index);
    info.content.end = ByteOffset{breakStart};
    info.lineBreak = end.kind;
    advanceTo(cursor, end.next);
    scan.cursor = cursor;
    scan.position = end.next;
    ++scan.line;
    return info;
}

TreeStats treeStats(const PieceTree& tree) noexcept {
    TreeStats stats;
    if (!tree.empty()) {
        addStats(tree.rootNode(), stats);
    }
    return stats;
}

} // namespace frost::text::detail
