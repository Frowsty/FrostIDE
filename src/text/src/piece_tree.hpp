// SPDX-License-Identifier: Apache-2.0
//
// Piece-tree algorithms shared by Snapshot and TextBuffer (ADR 0012, sections 4 and 5; private
// header): building pieces over stored bytes, byte splits, seeks by bytes, line breaks and UTF-16
// units, in-piece scanners and the line scanner behind lineInfo and LineCursor. All functions are
// pure over immutable trees, safe from any thread, and take no references.
#pragma once

#include "frost/text/detail/piece.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/text_summary.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::text::detail {

// True for UTF-8 continuation bytes (10xxxxxx).
[[nodiscard]] constexpr bool isContinuation(char byte) noexcept {
    return (static_cast<unsigned char>(byte) & 0xC0u) == 0x80u;
}

// Building.

// Appends pieces over the stored bytes [data, data + size) (well-formed UTF-8, already in their
// final place) to `out`: each at most maxPieceBytes, cut at char boundaries by backing up at most
// 3 bytes. O(size). Splitting the original into bounded runs follows VS Code's chunked buffers
// ("Text Buffer Reimplementation", 2018; see storage.hpp).
void appendPieces(std::vector<Piece>& out, const char* data, std::size_t size, std::uint32_t maxPieceBytes) noexcept;
// The pieces of `stored` (summarizePieces in ADR 0012): pure, parallelisable later. Budget: B15.
[[nodiscard]] std::vector<Piece> summarizePieces(std::string_view stored, std::uint32_t maxPieceBytes) noexcept;
// A balanced tree of `pieces` (buildTree). O(n).
[[nodiscard]] PieceTree buildTree(std::span<const Piece> pieces) noexcept;
// A one-piece tree.
[[nodiscard]] PieceTree singlePiece(const Piece& piece) noexcept;
// The piece over `bytes` (stored bytes) with its summary. O(|bytes|).
[[nodiscard]] Piece makePiece(const char* data, std::size_t size) noexcept;

// (left, right) with left holding bytes [0, x) and right the rest; a piece containing x strictly
// inside is cut at x, which must be a char boundary. O(B log n + P).
[[nodiscard]] std::pair<PieceTree, PieceTree> splitAtByte(const PieceTree& tree, std::uint64_t x) noexcept;

// Seeks. Each returns a cursor at the first piece whose prefix-through-it satisfies the
// condition (the end cursor when none does). O(B log n). Borrowed from `tree`.
// bytes > x: the piece containing byte x (the end cursor at x == size).
[[nodiscard]] PieceTree::Cursor seekByte(const PieceTree& tree, std::uint64_t x) noexcept;
// lineBreaks >= breaks: the piece holding the first byte of break number `breaks` (1-based).
[[nodiscard]] PieceTree::Cursor seekLineBreak(const PieceTree& tree, std::uint32_t breaks) noexcept;
// utf16 > units: the piece holding UTF-16 unit `units`.
[[nodiscard]] PieceTree::Cursor seekUtf16(const PieceTree& tree, std::uint64_t units) noexcept;

// Point queries (preconditions as for the Snapshot members of the same name).
[[nodiscard]] char byteAt(const PieceTree& tree, std::uint64_t x) noexcept;
[[nodiscard]] Point pointAt(const PieceTree& tree, std::uint64_t x) noexcept;
[[nodiscard]] Utf16Point utf16PointAt(const PieceTree& tree, std::uint64_t x) noexcept;
[[nodiscard]] std::uint64_t utf16At(const PieceTree& tree, std::uint64_t x) noexcept;
[[nodiscard]] std::uint64_t clip(const PieceTree& tree, std::uint64_t x) noexcept;
[[nodiscard]] bool isCharBoundary(const PieceTree& tree, std::uint64_t x) noexcept;
// The offset of UTF-16 unit `units` (<= the tree's utf16), floored inside a surrogate pair.
[[nodiscard]] std::uint64_t offsetAtUtf16(const PieceTree& tree, std::uint64_t units) noexcept;
// summarize() of bytes [a, b) (char boundaries) from boundary pieces and cached subtree summaries.
[[nodiscard]] TextSummary summarizeRange(const PieceTree& tree, std::uint64_t a, std::uint64_t b) noexcept;

// Lines.

// A position in the line sequence: line `line` starts at byte `position`, and `cursor` is at the
// piece containing that byte (the end cursor when position == size).
struct LineScan {
    PieceTree::Cursor cursor;
    std::uint64_t position = 0;
    std::uint32_t line = 0;
};
// The scan state at the start of line `line`. Precondition: line <= lineBreaks.
[[nodiscard]] LineScan lineScanAt(const PieceTree& tree, std::uint32_t line) noexcept;
// Describes the line at `scan` and advances `scan` to the next line. Precondition: scan.line <=
// lineBreaks; after the last line, scan.line is lineBreaks + 1 and scan must not be used again.
// Scans the rest of the current piece and seeks by line breaks only when no break follows in it:
// O(B log n + P) worst case.
[[nodiscard]] LineInfo nextLine(const PieceTree& tree, LineScan& scan) noexcept;

// Memory statistics for perf reports. O(nodes).
struct TreeStats {
    std::size_t leaves = 0;
    std::size_t internals = 0;
    std::size_t pieces = 0;
    std::size_t nodeBytes = 0; // sizeof of every node.
};
[[nodiscard]] TreeStats treeStats(const PieceTree& tree) noexcept;

} // namespace frost::text::detail
