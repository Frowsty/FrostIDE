// SPDX-License-Identifier: Apache-2.0
//
// frost::text::TextBuffer: the single writer of a text document (ADR 0013). It loads UTF-8,
// applies edits as new immutable Snapshots (snapshot.hpp) and reports each edit as a TextChange
// normalised to line-break boundaries, ready for tree-sitter's input edits and LSP didChange.
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/strong_type.hpp"
#include "frost/text/detail/piece.hpp"
#include "frost/text/detail/ref.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/snapshot.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::text {

// One edit of a batch (TextBuffer::applyEdits): replace `range` of the text before the batch with
// `text`. The view must stay valid for the call.
struct Edit {
    ByteRange range;
    std::string_view text;
};

/*
 * What one edit step changed: the old text's bytes [start, oldEnd) became the new text's bytes
 * [start, newEnd). startPoint and oldEndPoint are points in the old text, newEndPoint in the new.
 *
 * Normalisation (ADR 0013, D8): the change is widened, by at most one byte on each side of the
 * requested range, so that no bound lies strictly inside a CRLF in the old or the new text and the
 * line structure outside the change is identical in both. Its points therefore follow the LSP line
 * model and are representable in LSP, and replaying it as an LSP edit on the old text yields the
 * new text. The byte fields have the shape of tree-sitter's TSInputEdit; the syntax host converts
 * the points to TSPoints (LF rows) itself (D2).
 *
 * A no-op edit reports an empty change at the requested offset: start == oldEnd == newEnd and all
 * three points equal pointAt(offset), unnormalised.
 */
struct TextChange {
    ByteOffset start;
    ByteOffset oldEnd;
    ByteOffset newEnd;
    Point startPoint;
    Point oldEndPoint;
    Point newEndPoint;

    friend constexpr bool operator==(const TextChange&, const TextChange&) noexcept = default;
};

// Buffer options. Both are test knobs; production uses the defaults. At namespace scope because a
// nested struct with member initializers cannot be a default argument inside its own class.
struct TextBufferOptions {
    // The largest piece in bytes. FROST_PRE: 4 <= value <= 1 MiB. Bounds every in-piece scan.
    std::uint32_t maxPieceBytes = 4096;
    // The largest text in bytes. FROST_PRE: value <= 0xFFFF'FFFE (D6: line numbers, byte columns
    // and tree-sitter's uint32 offsets cover every byte and the end).
    std::uint64_t maxBufferBytes = 0xFFFF'FFFE;
};

/*
 * The single writer of one storage lineage. Edits build a new Snapshot and leave every earlier
 * snapshot untouched; current() and snapshot() expose the latest one.
 *
 * Thread safety: not thread-safe. Exactly one owner thread at a time (the document's actor or the
 * UI thread). Snapshots taken from it may be read on any thread.
 *
 * Errors and allocation: edits are all-or-nothing; on error the text, the version and the storage
 * are unchanged. Allocation failure terminates the process: edit, load and query members are
 * noexcept, so std::bad_alloc never leaves frost::text (as in frost::sched).
 *
 * Edit validation, in this order (shared with the test oracle):
 *   1. range.begin > range.end: InvalidArgument;
 *   2. range.end > size(): OutOfRange;
 *   3. begin or end inside a UTF-8 sequence: InvalidArgument, "byte x is inside a UTF-8 sequence";
 *   4. ill-formed text: InvalidArgument, "ill-formed UTF-8 at byte k of the inserted text";
 *   5. a result longer than maxBufferBytes: OutOfRange.
 * The text may point into this buffer's own storage (for example a view from chunkAt): appending
 * never moves or overwrites published bytes.
 *
 * Versions: a successful edit that changes the text bumps the version by 1. A no-op edit (an
 * empty insert, an empty erase, or a replace of an empty range with empty text) is validated like
 * any edit and, if valid, succeeds with an empty change at the offset and keeps the version.
 *
 * Cost of an edit: O(B log n + P + |text|) time; it allocates O(log n + |text| / P) nodes and
 * appends |text| bytes to the storage (deleted text is not reclaimed; ADR 0013, D5).
 */
class TextBuffer {
public:
    using Options = TextBufferOptions;

    // An empty buffer at version 0 with default options.
    TextBuffer() noexcept;
    // An empty buffer at version 0. Precondition: `options` within the bounds above.
    explicit TextBuffer(Options options) noexcept;

    /*
     * A buffer holding `text` at version 0. Takes ownership (pass an rvalue to avoid a copy).
     * Errors, checked in this order: OutOfRange if the text exceeds options.maxBufferBytes (checked
     * before scanning); InvalidArgument "ill-formed UTF-8 at byte k". O(bytes): validation plus
     * per-piece summaries (summarizePieces), then an O(n) balanced build (buildTree).
     * Precondition: `options` within the bounds above. Budget: B15.
     */
    [[nodiscard]] static Result<TextBuffer> fromUtf8(std::string text, Options options = {}) noexcept;

    // Movable. A moved-from TextBuffer may only be destroyed or assigned to.
    TextBuffer(TextBuffer&& other) noexcept;
    TextBuffer& operator=(TextBuffer&& other) noexcept;
    ~TextBuffer();

    TextBuffer(const TextBuffer&) = delete ("a TextBuffer is the only writer of its storage; share Snapshots");
    TextBuffer&
    operator=(const TextBuffer&) = delete ("a TextBuffer is the only writer of its storage; share Snapshots");

    // The latest snapshot, borrowed: the reference is invalidated by the next edit, restore, move
    // or assignment. O(1).
    [[nodiscard]] const Snapshot& current() const noexcept { return m_current; }
    // A copy of the latest snapshot. O(1). Budget: B5.
    [[nodiscard]] Snapshot snapshot() const noexcept { return m_current; }
    // The current version: 0 after construction or load, +1 per text-changing edit or restore.
    [[nodiscard]] std::uint64_t version() const noexcept { return m_version; }
    // The options this buffer was created with.
    [[nodiscard]] const Options& options() const noexcept { return m_options; }

    // Inserts `utf8` at `at` (a replace of the empty range [at, at)). Budget: B5.
    [[nodiscard]] Result<TextChange> insert(ByteOffset at, std::string_view utf8) noexcept;
    // Removes `range` (a replace with empty text).
    [[nodiscard]] Result<TextChange> erase(ByteRange range) noexcept;
    // Replaces `range` with `utf8`.
    [[nodiscard]] Result<TextChange> replace(ByteRange range, std::string_view utf8) noexcept;

    /*
     * Applies a batch atomically. Offsets refer to the text before the batch, and for every i > 0
     * edits[i-1].range.end <= edits[i].range.begin: sorted and non-overlapping; several inserts at
     * one offset keep their given order, and an insert may sit at the begin of a following range,
     * but a non-empty range may not be followed by an insert at its begin. Everything is validated
     * before anything is applied: for each i in order, checks 1-4 on edits[i] and then the order
     * rule against edits[i-1]; then check 5 on the batch's total size. Messages name the edit
     * index. Edits are applied last to first; the changes are returned in application order, each
     * normalised in the coordinates of its own step (ready for sequential LSP didChange entries).
     * No-op edits produce no entry. The version is bumped once if any edit changes the text,
     * otherwise not at all; an empty span returns an empty vector.
     */
    [[nodiscard]] Result<std::vector<TextChange>> applyEdits(std::span<const Edit> edits) noexcept;

    /*
     * Makes `earlier` (a snapshot of this buffer) the current text again. O(1); bumps the version.
     * The change covers the whole text: start 0, oldEnd the old size, newEnd the new size, with
     * points taken from the summaries (m1/undo-anchors makes it precise). Errors: InvalidArgument
     * if !earlier.sharesStorageWith(current()) (a default Snapshot is foreign). `earlier` may alias
     * current().
     */
    [[nodiscard]] Result<TextChange> restore(const Snapshot& earlier) noexcept;

    /*
     * Snapshot::checkInvariants plus
     *   P5 every piece lies inside the used part of the original or of one add chunk;
     *   P6 current().version() == version(), and current() shares storage with the buffer.
     * Writer thread only (it reads the storage's chunk directory). O(bytes + n log chunks).
     */
    [[nodiscard]] Result<void> checkInvariants() const noexcept;

private:
    TextBuffer(detail::Ref<detail::Storage> storage, detail::PieceTree tree, Options options) noexcept;

    // Validates one edit (checks 1-4) against the current text; `label` prefixes the message.
    [[nodiscard]] Result<void> validate(ByteRange range, std::string_view text, std::string_view label) const noexcept;
    // The tree with [range) replaced by `text` (appended to the storage); no validation.
    [[nodiscard]] detail::PieceTree replaceTree(const detail::PieceTree& tree, ByteRange range,
                                                std::string_view text) noexcept;

    detail::Ref<detail::Storage> m_storage;
    Snapshot m_current;
    std::uint64_t m_version = 0;
    Options m_options;
};

} // namespace frost::text
