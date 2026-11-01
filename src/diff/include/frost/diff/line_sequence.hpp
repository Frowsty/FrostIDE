// SPDX-License-Identifier: Apache-2.0
//
// frost::diff::LineSequence: an immutable, O(1)-copyable byte copy of a text cut into lines, with
// a hash per line, the input every frost::diff algorithm works on (ADR 0019, D2). Also LineRange,
// the half-open range of line indices that hunks and merge chunks are made of.
/*
 * Line model. Two splits exist:
 *   - LineSplit::Lsp: lines exactly as frost::text::Snapshot reports them (LSP 3.17: LF, CR and
 *     CRLF each end a line once);
 *   - LineSplit::Lf: git's model, in which a line ends after each LF and a CR is content.
 * In both, a line's token is its bytes INCLUDING its break, and lines compare byte-exactly:
 * "a\r\n" != "a\n" and "a" != "a\n". The final line is omitted when it is empty, so "" has 0
 * lines, "a\n" has 1, "a" has 1 (without a break) and "a\n\n" has 2. Only the last line of a
 * sequence can lack a break.
 *
 * Under Lsp, line i equals Snapshot line i (its content plus its break) for every i < size(), and
 * size() == Snapshot::lineCount() - 1 when the text is empty or ends with a break, otherwise
 * lineCount(). Byte offsets equal Snapshot offsets under either split, because the bytes are an
 * exact copy.
 *
 * Any bytes are accepted: invalid UTF-8 and NUL are ordinary content. Hashes are 64-bit FNV-1a
 * (public domain; draft-eastlake-fnv), which is fast but not collision-resistant; the interner
 * that consumes them counts its probe steps as work, so crafted collisions cannot stall a diff
 * (ADR 0019, D7 and D13).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/strong_type.hpp"
#include "frost/text/positions.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace frost::text {
class Snapshot;
} // namespace frost::text

namespace frost::diff {

// How a text is cut into lines (see the header comment).
enum class LineSplit : std::uint8_t {
    Lsp, // LF, CR and CRLF each end a line (frost::text's model).
    Lf,  // Only LF ends a line; CR is content (git's model; use it for patches).
};

// A half-open range [begin, end) of line indices. Invariant (callers' responsibility):
// begin <= end. Plain value.
struct LineRange {
    LineIndex begin;
    LineIndex end;

    // The number of lines. O(1).
    [[nodiscard]] constexpr std::uint32_t size() const noexcept { return end - begin; }
    // True when the range holds no line. O(1).
    [[nodiscard]] constexpr bool empty() const noexcept { return begin == end; }

    friend constexpr bool operator==(const LineRange&, const LineRange&) noexcept = default;
};

// The default byte limit of one sequence: 64 MiB. Larger texts belong to large-file mode, whose
// callers diff ranges or replace the whole text instead.
inline constexpr std::uint64_t kDefaultMaxSequenceBytes = std::uint64_t{64} << 20;
// The default line limit of one sequence: 2^21 (2,097,152) lines.
inline constexpr std::uint32_t kDefaultMaxLines = 1u << 21;

// Limits checked before a sequence is built. Above either limit, construction fails with
// OutOfRange, before the bytes are copied or the line table is allocated.
struct SequenceLimits {
    std::uint64_t maxBytes = kDefaultMaxSequenceBytes; // Also capped at 0xFFFF'FFFE (32-bit offsets).
    std::uint32_t maxLines = kDefaultMaxLines;
};

/*
 * An immutable byte copy, a line table (size() + 1 uint32 start offsets) and one 64-bit FNV-1a
 * hash per line. Memory: the bytes plus 12 bytes per line, in four heap allocations (the shared
 * storage with its control block, the bytes, the offsets and the hashes), shared by every copy.
 *
 * Lifetime: copying is O(1) (a shared_ptr copy) and moving is noexcept. The bytes never move, so
 * every string_view returned by bytes() or line() stays valid across moves and copies while any
 * copy of this sequence lives. A moved-from sequence may only be destroyed or assigned to. The
 * default value is the empty Lsp sequence.
 *
 * Thread safety: immutable, so every const member is safe concurrently from any number of threads
 * (copying touches only the shared_ptr's atomic count).
 *
 * Index arguments are internal values (derived from this sequence); out-of-range indices are bugs
 * and fail a FROST_PRE check.
 */
class LineSequence {
public:
    LineSequence() noexcept = default;

    /*
     * Builds a sequence from any bytes (invalid UTF-8 and NUL allowed). Errors, in this order:
     * OutOfRange if bytes.size() > min(limits.maxBytes, 0xFFFF'FFFE), checked first; OutOfRange if
     * the line count exceeds limits.maxLines, counted by a break-counting pass before the line
     * table is allocated. O(bytes). Thread-safe (pure).
     */
    [[nodiscard]] static Result<LineSequence> fromBytes(std::string bytes, LineSplit split = LineSplit::Lsp,
                                                        const SequenceLimits& limits = {}) noexcept;

    /*
     * Builds a sequence from a snapshot's text. Errors as fromBytes, but both are checked BEFORE the
     * copy, in O(1): the size from Snapshot::size(), the line count from the snapshot's summary
     * (Lsp: lineBreaks plus one for a non-empty last line; Lf: lineFeeds plus one unless the text
     * ends with LF). Copies through Snapshot::chunks(): O(bytes). Safe on any thread (snapshots are
     * immutable).
     */
    [[nodiscard]] static Result<LineSequence> fromSnapshot(const text::Snapshot& snapshot,
                                                           LineSplit split = LineSplit::Lsp,
                                                           const SequenceLimits& limits = {}) noexcept;

    // The number of lines. O(1).
    [[nodiscard]] std::uint32_t size() const noexcept;
    // The split this sequence was built with. O(1).
    [[nodiscard]] LineSplit split() const noexcept;
    // The whole text. O(1).
    [[nodiscard]] std::string_view bytes() const noexcept;
    // Line `line`'s bytes including its break. Precondition: line < size(). O(1).
    [[nodiscard]] std::string_view line(LineIndex line) const noexcept;
    // Line `line`'s byte range in bytes(), break included. Precondition: line < size(). O(1).
    [[nodiscard]] text::ByteRange lineRange(LineIndex line) const noexcept;
    // The byte range of lines [range.begin, range.end). Precondition: range.begin <= range.end <=
    // size() (end == size() is allowed; an empty range at size() is {bytes, bytes}). O(1).
    [[nodiscard]] text::ByteRange byteRange(LineRange range) const noexcept;
    // Line `line`'s 64-bit FNV-1a hash (over its bytes including the break). Precondition:
    // line < size(). O(1).
    [[nodiscard]] std::uint64_t lineHash(LineIndex line) const noexcept;
    /*
     * The break of the first line that has one, or LineBreak::None when no line has a break.
     * Under Lsp it is that break's kind; under Lf it is CrLf when that line's LF follows a CR, else
     * Lf (never Cr). Computed at construction; O(1). Merge markers use it to pick their line end.
     */
    [[nodiscard]] text::LineBreak firstLineBreak() const noexcept;

    // True when `line`'s last byte ends it under this sequence's split (the last line may lack a
    // break). Precondition: line < size(). O(1).
    [[nodiscard]] bool hasBreak(LineIndex line) const noexcept;

private:
    struct Storage;

    explicit LineSequence(std::shared_ptr<const Storage> storage) noexcept;

    std::shared_ptr<const Storage> m_storage; // Null for the default (empty Lsp) sequence.
};

} // namespace frost::diff
