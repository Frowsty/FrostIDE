// SPDX-License-Identifier: Apache-2.0
//
// frost::text::Snapshot: an immutable, O(1)-copyable version of a text buffer that any number of
// threads can query without locks (ADR 0013, D5 and D7), plus its ChunkRange and LineCursor
// views. TextBuffer (text_buffer.hpp) is the single writer that produces snapshots.
/*
 * PieceTree stores a document as a persistent B+tree of pieces. Leaves reference ranges in the
 * original text or in append-only add chunks; inner nodes cache TextSummary values (bytes, line
 * breaks, UTF-16 units, characters, the longest line), so offset <-> line <-> UTF-16 lookups are
 * O(log n). Copying a snapshot only increments two reference counts.
 *
 * Positions follow LSP 3.17's line model: LF, CR and CRLF each end a line; columns exclude the
 * break. An offset strictly between the CR and the LF of a CRLF ("CRLF-interior") reports the
 * same point as the CR, which is the end of its line. Content is always well-formed UTF-8, so a
 * "char boundary" is any offset that is not inside a multi-byte sequence.
 *
 * Round-trip rules, with crlfFloor(y) = y - 1 if y is CRLF-interior, else y:
 *   offsetAt(pointAt(x))        == crlfFloor(clip(x))
 *   offsetAt(utf16PointAt(x))   == crlfFloor(clip(x))
 *   offsetAtUtf16(utf16At(x))   == clip(x)
 *   pointAt(*offsetAt(p))       == p for every canonical p (p.line < lineCount() and p.column a
 *                                  char boundary within the line's content).
 *
 * Complexity: n = pieces, P = maxPieceBytes (4096 by default), B = 8 (the tree's fan-out).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/strong_type.hpp"
#include "frost/text/detail/piece.hpp"
#include "frost/text/detail/ref.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/text_summary.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

namespace frost::text {

class ChunkRange;
class LineCursor;
class TextBuffer;

/*
 * An immutable value: a piece tree, a reference to the storage its pieces point into, and the
 * buffer version it was taken at. The default value is the empty text at version 0 with no
 * storage, which shares storage with nothing.
 *
 * Lifetime: copy is O(1) (two relaxed atomic increments); move is noexcept and touches no
 * counter. The destructor is O(1) unless it drops the last reference to nodes or storage, which
 * frees them (O(nodes freed)) on the releasing thread: about 40k nodes for a 2 GB document, in the
 * millisecond range. TODO(M1): m1/large-file moves that final release to a background lane.
 * String views returned by any query stay valid while any Snapshot (including those held by
 * ChunkRange and LineCursor) or TextBuffer sharing this storage is alive.
 *
 * Thread safety: every const member is lock-free and safe concurrently from any number of threads.
 * The queries take no references; they walk the tree through the Snapshot they are called on, so
 * readers that only query one snapshot do not contend on its counters. The exceptions are
 * chunks() and linesFrom(): their ChunkRange and LineCursor hold a Snapshot copy, so each call
 * makes two relaxed increments and, when its result is destroyed, two acq_rel decrements on the
 * root node's and the storage's shared counters, on which readers of one snapshot do contend
 * (once per call, not per chunk or line). Hand snapshots across threads through any
 * happens-before mechanism (a sched task, a mutex, release/acquire). Assigning a Snapshot object
 * while another thread reads that same object is a data race.
 *
 * Byte-offset queries take offsets derived from this snapshot; an offset beyond the stated range
 * is a bug and fails a FROST_PRE check. External positions (Point, Utf16Point, Utf16Offset, line
 * numbers) come from users and protocols and are checked: the functions return Result.
 */
class Snapshot {
public:
    Snapshot() noexcept;
    Snapshot(const Snapshot& other) noexcept;
    Snapshot(Snapshot&& other) noexcept;
    Snapshot& operator=(const Snapshot& other) noexcept;
    Snapshot& operator=(Snapshot&& other) noexcept;
    ~Snapshot();

    // Totals, all O(1).
    // The buffer version this snapshot was taken at (TextBuffer::version()).
    [[nodiscard]] std::uint64_t version() const noexcept { return m_version; }
    // The text's length in bytes.
    [[nodiscard]] ByteOffset size() const noexcept { return ByteOffset{m_tree.summary().bytes}; }
    // The summary of the whole text.
    [[nodiscard]] const TextSummary& summary() const noexcept { return m_tree.summary(); }
    // The number of lines: lineBreaks + 1, at least 1 (fits in 32 bits by the buffer limit, D6).
    [[nodiscard]] std::uint32_t lineCount() const noexcept { return m_tree.summary().lineBreaks + 1; }
    // The text's length in UTF-16 code units.
    [[nodiscard]] Utf16Offset utf16Size() const noexcept { return Utf16Offset{m_tree.summary().utf16}; }

    // True when both snapshots come from the same TextBuffer lineage (the same storage). A default
    // Snapshot shares storage with nothing, not even another default Snapshot. O(1).
    [[nodiscard]] bool sharesStorageWith(const Snapshot& other) const noexcept;

    // Byte-offset queries. Each is O(B log n + P), noexcept, and allocates nothing.

    // The line containing `offset`. Precondition: offset <= size(). A CRLF-interior offset reports
    // the CR's line. Budget: B5.
    [[nodiscard]] LineIndex lineAt(ByteOffset offset) const noexcept;
    // The line and byte column of `offset`. Precondition: offset <= size(). A CRLF-interior offset
    // reports the CR's point. The column is not floored: an offset inside a multi-byte character
    // gives a column inside it. One descent; two when the CRLF straddles pieces. Budget: B5.
    [[nodiscard]] Point pointAt(ByteOffset offset) const noexcept;
    // The number of UTF-16 units before `offset`, floored to a char boundary first. Precondition:
    // offset <= size(). A CRLF-interior offset counts the CR (it is a char boundary).
    [[nodiscard]] Utf16Offset utf16At(ByteOffset offset) const noexcept;
    // The line and UTF-16 column of `offset`, floored to a char boundary first. Precondition:
    // offset <= size(). A CRLF-interior offset reports the CR's point.
    [[nodiscard]] Utf16Point utf16PointAt(ByteOffset offset) const noexcept;
    // True when `offset` is not inside a multi-byte sequence. Any offset: false beyond size(),
    // true at size().
    [[nodiscard]] bool isCharBoundary(ByteOffset offset) const noexcept;
    // `offset` clamped to size(), then floored to a char boundary. Any offset. A CRLF-interior
    // offset is a char boundary and is returned unchanged.
    [[nodiscard]] ByteOffset clip(ByteOffset offset) const noexcept;
    // The byte at `offset`. Precondition: offset < size().
    [[nodiscard]] char byteAt(ByteOffset offset) const noexcept;

    // External-position queries, O(B log n + P) each.

    // The offset of `point`. Errors: OutOfRange if point.line >= lineCount(). A column beyond the
    // line's content clamps to the content end (the LSP rule); a column inside a character floors
    // to its start.
    [[nodiscard]] Result<ByteOffset> offsetAt(Point point) const noexcept;
    // The offset of an LSP position. Same rules and errors; a column between the two units of a
    // surrogate pair floors to the character's start.
    [[nodiscard]] Result<ByteOffset> offsetAt(Utf16Point point) const noexcept;
    // The offset of the char that holds UTF-16 unit `offset` (floored inside a surrogate pair).
    // Errors: OutOfRange if offset > utf16Size().
    [[nodiscard]] Result<ByteOffset> offsetAtUtf16(Utf16Offset offset) const noexcept;
    // Line `line`: its content range and break. Errors: OutOfRange if line >= lineCount().
    // Budget: B5.
    [[nodiscard]] Result<LineInfo> lineInfo(LineIndex line) const noexcept;

    // Text and iteration.

    // The whole text. O(size).
    [[nodiscard]] std::string text() const noexcept;
    // The bytes of `range`, byte-exact with no boundary requirement. Precondition:
    // range.begin <= range.end <= size(). O(B log n + |range|).
    [[nodiscard]] std::string text(ByteRange range) const noexcept;
    // summarize(text(range)) without copying: the partial summaries of the two boundary pieces and
    // the cached summaries of every whole subtree between them. Precondition: range.begin <=
    // range.end <= size(), both char boundaries. O(B log n + P).
    [[nodiscard]] TextSummary summaryOf(ByteRange range) const noexcept;
    // The bytes of `range` as a forward range of non-empty string_views in order, each valid
    // UTF-8, ending exactly at the range bounds. Precondition: range.begin <= range.end <= size(),
    // both char boundaries. The ChunkRange holds a Snapshot copy, so it may outlive this object and
    // the TextBuffer. O(B log n) to begin, amortised O(1) per chunk. Budget: B5.
    [[nodiscard]] ChunkRange chunks(ByteRange range) const noexcept;
    // The rest of the piece containing `offset`, or an empty view at size(). Precondition:
    // offset <= size(). It starts with a whole character when `offset` is a char boundary: the
    // shape of tree-sitter's read callback. O(B log n).
    [[nodiscard]] std::string_view chunkAt(ByteOffset offset) const noexcept;
    // A cursor over the lines from `first` to the last line. Errors: OutOfRange if first >=
    // lineCount(). The LineCursor holds a Snapshot copy. O(B log n + P).
    [[nodiscard]] Result<LineCursor> linesFrom(LineIndex first) const noexcept;

    /*
     * Checks invariants S1-S4 (sum_tree.hpp) and P1-P4, using the options stored with the storage,
     * and returns ErrorCode::Internal naming the first violation:
     *   P1 1 <= piece bytes <= maxPieceBytes;  P2 a piece's summary equals summarize(its bytes);
     *   P3 every piece is well-formed UTF-8;   P4 total bytes <= maxBufferBytes.
     * O(bytes + n); safe on any thread. For tests and fuzzing.
     */
    [[nodiscard]] Result<void> checkInvariants() const noexcept;

    // The piece tree, for frost::text's own code and white-box tests. Not public API.
    [[nodiscard]] const detail::PieceTree& pieceTree() const noexcept { return m_tree; }

private:
    friend class TextBuffer;

    Snapshot(detail::PieceTree tree, detail::Ref<detail::Storage> storage, std::uint64_t version) noexcept;

    detail::PieceTree m_tree;
    detail::Ref<detail::Storage> m_storage;
    std::uint64_t m_version = 0;
};

/*
 * The bytes of a byte range of a snapshot as a forward range of non-empty string_views
 * (Snapshot::chunks). It holds a Snapshot copy, so the views stay valid while it lives; its
 * iterators borrow from it and are valid while the ChunkRange is alive. Thread safety: like a
 * Snapshot; an iterator is not safe to share between threads while it is advanced.
 */
class ChunkRange {
public:
    class Iterator {
    public:
        using value_type = std::string_view;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::forward_iterator_tag;

        // The end iterator.
        Iterator() noexcept = default;

        // The current chunk (non-empty unless this is the end iterator).
        [[nodiscard]] std::string_view operator*() const noexcept { return m_chunk; }
        // Moves to the next chunk. Amortised O(1).
        Iterator& operator++() noexcept;
        Iterator operator++(int) noexcept {
            Iterator before = *this;
            ++*this;
            return before;
        }

        friend bool operator==(const Iterator& a, const Iterator& b) noexcept {
            return a.m_chunk.data() == b.m_chunk.data() && a.m_chunk.size() == b.m_chunk.size();
        }
        friend bool operator==(const Iterator& it, std::default_sentinel_t) noexcept { return it.m_chunk.empty(); }

    private:
        friend class ChunkRange;

        detail::PieceTree::Cursor m_cursor;
        std::uint64_t m_end = 0;  // Absolute end of the range.
        std::string_view m_chunk; // Empty (and null) at the end.
    };

    // The first chunk. O(B log n).
    [[nodiscard]] Iterator begin() const noexcept;
    [[nodiscard]] std::default_sentinel_t end() const noexcept { return {}; }
    // The byte range this range covers.
    [[nodiscard]] ByteRange range() const noexcept { return m_range; }

private:
    friend class Snapshot;

    ChunkRange(Snapshot snapshot, ByteRange range) noexcept;

    Snapshot m_snapshot;
    ByteRange m_range;
};

/*
 * A forward cursor over the lines of a snapshot, from a first line to the last
 * (Snapshot::linesFrom). It holds a Snapshot copy. next() scans forward from the previous line's
 * end through the rest of the current piece and seeks by line breaks only when no break follows
 * in that piece: per line O(B log n + P) in the worst case whatever the line's length, and
 * O(line bytes) typically. Not thread-safe as an object.
 */
class LineCursor {
public:
    // Stores the next line in `line` and returns true, or returns false after the last line.
    bool next(LineInfo& line) noexcept;

private:
    friend class Snapshot;

    LineCursor(Snapshot snapshot, detail::PieceTree::Cursor cursor, std::uint64_t position,
               std::uint32_t line) noexcept;

    Snapshot m_snapshot;
    detail::PieceTree::Cursor m_cursor; // The piece containing m_position (the end cursor at size()).
    std::uint64_t m_position = 0;       // Where the next line starts.
    std::uint32_t m_line = 0;           // The next line's index.
    bool m_done = false;
};

} // namespace frost::text
