// SPDX-License-Identifier: Apache-2.0
//
// frost::diff three-way merge (ADR 0019, D10-D12): merge3 chunks base, ours and theirs into stable
// and changed regions (diff3 chunking after Khanna, Kunal and Pierce, "A Formal Investigation of
// Diff3", FSTTCS 2007), and MergeModel is the resolution model behind the save manager's
// external-change merge and the minimal merge view: per-chunk choices, resolved text, diff3-style
// conflict markers and a self-contained patch for the editor buffer.
/*
 * Chunking. merge3 diffs base against ours and base against theirs (diffLines' algorithm with
 * options.diff). A base line is "stable" when both diffs leave it unchanged; maximal runs of stable
 * lines that are also consecutive in ours and theirs form Stable chunks, and the regions between
 * them are unstable chunks, classified by comparing their three sides:
 *   all three equal -> Stable (merged into its neighbours); ours == base -> Theirs (only theirs
 *   changed); theirs == base -> Ours; ours == theirs -> Both (the same change on both sides);
 *   otherwise Conflict.
 * No conflict trimming is done (git's "zealous" refinement: TODO(M11)).
 *
 * Work cap. One merge budget of options.diff.limits.maxWork units: interning the three sequences
 * charges it first (8 units per base line and, through the partner path, about 1 per line of ours
 * and theirs that matches base, so a lightly edited 100k-line merge spends about 1M of the default
 * 4.2M units there), and the remainder R is split evenly, each diff running with floor(R / 2). Equal
 * budgets make merge3(O, A, A) and merge3(O, O, B) clean under every cap. A degraded diff only
 * loses matches, so chunks only get coarser, and a coarse chunk is clean only if one side equals
 * base, or both sides are equal, over the whole chunk: degradation never turns a true conflict into
 * a clean merge; it can only turn clean changes into conflicts (ADR 0019, D7).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/diff/line_diff.hpp"
#include "frost/diff/line_sequence.hpp"
#include "frost/text/positions.hpp"
#include "frost/text/text_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::diff {

enum class ChunkKind : std::uint8_t {
    Stable,   // All three sides are equal.
    Ours,     // Only ours changed (theirs equals base).
    Theirs,   // Only theirs changed (ours equals base).
    Both,     // Ours and theirs made the same change.
    Conflict, // All three sides differ.
};

// One chunk: line ranges of each side. Every chunk is non-empty on some side.
struct MergeChunk {
    ChunkKind kind = ChunkKind::Stable;
    LineRange base;
    LineRange ours;
    LineRange theirs;

    friend constexpr bool operator==(const MergeChunk&, const MergeChunk&) noexcept = default;
};

struct MergeOptions {
    DiffOptions diff{};
    // The line break of markers and of joined pieces. None means "detect" (MergeModel::eol); any
    // other value forces it. The save manager passes the document's line separator.
    text::LineBreak eol = text::LineBreak::None;
};

// The result of merge3. Plain value.
struct Merge3 {
    std::vector<MergeChunk> chunks; // Partition base, ours and theirs in order; no two Stable chunks are adjacent.
    std::uint32_t conflicts = 0;    // The number of Conflict chunks.
    bool degraded = false;          // A diff reached its work cap; chunks may be coarser than normal.
    std::uint64_t work = 0;         // Work units spent (interning plus both diffs).
};

/*
 * Chunks a three-way merge (see the header comment). Precondition: all three sequences use the
 * same split. Errors: OutOfRange if a side has more than options.diff.limits.maxLines lines;
 * Cancelled (at entry, or polled every 4096 work units). When neither diff is degraded, the
 * diffs are exactly diffLines(base, ours) and diffLines(base, theirs) (line ids only decide
 * equality). O(N_O + N_A + N_B) after the diffs. Pure and reentrant. Budget: B21.
 */
[[nodiscard]] Result<Merge3> merge3(const LineSequence& base, const LineSequence& ours, const LineSequence& theirs,
                                    const MergeOptions& options = {}, std::stop_token stop = {}) noexcept;

/*
 * Checks a merge of the three sequences and returns ErrorCode::Internal naming the first
 * violation:
 *   C1 the chunk spans partition base, ours and theirs, in order;
 *   C2 Stable chunks are byte-equal on all three sides;
 *   C3 kinds match equality (Ours: theirs == base != ours; Theirs: ours == base != theirs;
 *      Both: ours == theirs != base; Conflict: pairwise different);
 *   C4 every chunk is non-empty on some side, no two Stable chunks are adjacent, and conflicts
 *      equals the number of Conflict chunks.
 * O(bytes); safe on any thread. For tests and fuzzing.
 */
[[nodiscard]] Result<void> checkMerge(const LineSequence& base, const LineSequence& ours, const LineSequence& theirs,
                                      const Merge3& merge) noexcept;

// How one chunk is resolved.
enum class Take : std::uint8_t {
    Unresolved,     // Conflict chunks only: rendered with markers, refused by resolvedText().
    Ours,           // Ours' lines.
    Theirs,         // Theirs' lines.
    Base,           // Base's lines (rejects both changes).
    OursThenTheirs, // Ours' lines, then theirs'.
    TheirsThenOurs, // Theirs' lines, then ours'.
    Custom,         // The custom text given to resolve(), verbatim.
};

// The largest conflict-marker size renderWithMarkers uses (see MergeModel::markerSize). It bounds
// each marker line, so the markers add at most four lines of kMaxMarkerSize + 1 + label + 2 bytes
// per conflict to the output, whatever the content.
inline constexpr std::uint32_t kMaxMarkerSize = 64;

// Options of MergeModel::renderWithMarkers. The labels are viewed only during the call.
struct MarkerOptions {
    std::string_view oursLabel = "ours";
    std::string_view baseLabel = "base";
    std::string_view theirsLabel = "theirs";
    // The smallest marker size, clamped to [7, 32]. The size used is larger when content lines
    // start with marker-character runs of this length (see MergeModel::markerSize).
    std::uint8_t minSize = 7;
};

// A rendered or resolved merge: the text, and one byte range of it per chunk in chunk order
// (empty for a chunk that emits nothing). A rendered conflict's range covers its marker lines.
struct MergeOutput {
    std::string text;
    std::vector<text::ByteRange> chunks;
};

/*
 * The edits that turn ours into a resolved merge (MergeModel::editsForOurs): sorted, non-adjacent,
 * in ours' byte coordinates, with no edit for a chunk whose output equals ours' bytes, ready for
 * TextBuffer::applyEdits (so the merge lands as one undoable edit).
 *
 * Self-contained: it holds O(1) copies of the three sequences and a heap arena for joined texts,
 * so the edits' views stay valid independently of the model's lifetime and across moves.
 * Immutable; safe to read from any thread and to hand to another thread. Move-only.
 * Precondition (the caller's duty): apply only to a buffer whose text is still ours' bytes (compare
 * TextBuffer::version() with the version ours was built at).
 */
class OursPatch {
public:
    OursPatch(OursPatch&&) noexcept = default;
    OursPatch& operator=(OursPatch&&) noexcept = default;
    OursPatch(const OursPatch&) = delete ("an OursPatch owns its arena; move it");
    OursPatch& operator=(const OursPatch&) = delete ("an OursPatch owns its arena; move it");
    ~OursPatch() = default;

    // The edits. Valid while this patch lives (and across moves). O(1).
    [[nodiscard]] std::span<const text::Edit> edits() const noexcept { return m_edits; }

private:
    friend class MergeModel;
    OursPatch() noexcept = default;

    LineSequence m_base;
    LineSequence m_ours;
    LineSequence m_theirs;
    std::unique_ptr<std::string> m_arena; // Joined texts; built completely before any view is taken.
    std::vector<text::Edit> m_edits;
};

/*
 * The resolution model: owns the three sequences, their merge3 chunks and one Take per chunk.
 *
 * Defaults: Ours -> Take::Ours, Theirs -> Take::Theirs, Both -> Take::Ours, Conflict -> Unresolved;
 * Stable chunks emit their (equal) lines and report Take::Ours. Non-conflict chunks may be
 * overridden, for example Take::Base rejects one side's change.
 *
 * Line-break joining (resolvedText, renderWithMarkers and editsForOurs): output is built from
 * pieces (a side's lines for a chunk, a custom text, a marker line). When a piece ends without a
 * line break under the sequences' split and more output follows, eol() is inserted after it. Only a
 * side's last line can lack a break, and any chunk holding a side's last line is the final chunk,
 * so for line pieces joining happens only inside the final chunk (OursThenTheirs, TheirsThenOurs,
 * and markers); a custom text without a final break is joined to whatever follows it. Under Lsp, a
 * piece ending in a lone CR followed by a piece starting with LF fuses into one CRLF in the output
 * bytes, so the output's Lsp lines can differ from the chunk structure; the bytes are still exactly
 * the concatenation (ADR 0019, D11).
 *
 * Thread safety: const members are safe concurrently; resolve() needs exclusive access (one owner,
 * like TextBuffer). Movable, not copyable; a moved-from model may only be destroyed or assigned.
 */
class MergeModel {
public:
    // Merges the three sequences (merge3) and owns them. Errors: as merge3. Budget: B21.
    [[nodiscard]] static Result<MergeModel> create(LineSequence base, LineSequence ours, LineSequence theirs,
                                                   const MergeOptions& options = {},
                                                   std::stop_token stop = {}) noexcept;

    MergeModel(MergeModel&&) noexcept = default;
    MergeModel& operator=(MergeModel&&) noexcept = default;
    MergeModel(const MergeModel&) = delete ("a model owns its resolution state; share LineSequences instead");
    MergeModel&
    operator=(const MergeModel&) = delete ("a model owns its resolution state; share LineSequences instead");
    ~MergeModel() = default;

    // The chunks. O(1).
    [[nodiscard]] const Merge3& merge() const noexcept { return m_merge; }
    [[nodiscard]] const LineSequence& base() const noexcept { return m_base; }
    [[nodiscard]] const LineSequence& ours() const noexcept { return m_ours; }
    [[nodiscard]] const LineSequence& theirs() const noexcept { return m_theirs; }
    /*
     * The line break of markers and joins: MergeOptions::eol if it is not None; else
     * ours.firstLineBreak(), else theirs', else base's, else LF. A view of a static literal
     * ("\n", "\r" or "\r\n"). O(1).
     */
    [[nodiscard]] std::string_view eol() const noexcept { return m_eol; }
    // Chunk `chunk`'s current take. Precondition: chunk < merge().chunks.size(). O(1).
    [[nodiscard]] Take take(std::size_t chunk) const noexcept;
    // The number of Conflict chunks whose take is Unresolved. O(1).
    [[nodiscard]] std::uint32_t unresolvedCount() const noexcept { return m_unresolved; }
    // Chunk `chunk`'s custom text: empty unless its take is Custom. Valid until the next resolve().
    // Precondition: chunk < merge().chunks.size(). O(1).
    [[nodiscard]] std::string_view customText(std::size_t chunk) const noexcept;

    /*
     * Sets chunk `chunk`'s take. Custom with empty text deletes the chunk. Custom text is emitted
     * verbatim (plus the joining rule) and is not validated as UTF-8; TextBuffer::applyEdits
     * validates when a patch is applied. Errors: OutOfRange (the index); InvalidArgument for a
     * Stable chunk, for Unresolved on a non-Conflict chunk, and for a non-empty customText with a
     * take other than Custom. O(|customText|).
     */
    [[nodiscard]] Result<void> resolve(std::size_t chunk, Take take, std::string customText = {}) noexcept;

    // The merged text with every chunk resolved. Errors: InvalidArgument "chunk k: conflict
    // unresolved" while any conflict is Unresolved (k is the first). O(output).
    [[nodiscard]] Result<MergeOutput> resolvedText() const noexcept;

    /*
     * The merged text with each Unresolved conflict written in diff3 style (resolved chunks are
     * emitted as resolved):
     *     <<<<<<< ours        (the label, escaped like a unified header; an empty label gives the
     *     <ours lines>         bare marker)
     *     ||||||| base
     *     <base lines>
     *     =======
     *     <theirs lines>
     *     >>>>>>> theirs
     * Marker lines end with eol() and use markerSize(options) characters, so content that looks
     * like a marker round-trips: a parser that takes a marker line to be exactly that many marker
     * characters followed by a space or the line's end never mistakes a content line for one.
     * TODO(M11): git's "merge" style without the base section, and zdiff3.
     * Errors: as markerSize. O(output); the output is at most the emitted pieces plus four marker
     * lines of at most kMaxMarkerSize + 1 + label + 2 bytes per unresolved conflict.
     */
    [[nodiscard]] Result<MergeOutput> renderWithMarkers(const MarkerOptions& options = {}) const noexcept;

    /*
     * The marker size renderWithMarkers(options) uses: the smallest n from clamp(minSize, 7, 32) up
     * to kMaxMarkerSize such that no line starts with a run of exactly n copies of one marker
     * character ('<', '|', '=', '>'), counting every line of the three sequences and of the
     * custom texts of chunks taken as Custom (lines split like the sequences). A run is maximal: a
     * line of 100 '=' is a run of 100, which collides with no size up to kMaxMarkerSize. Errors:
     * OutOfRange when every size in that range collides, which takes lines with marker runs of
     * every length in it (the caller then resolves in the merge view instead). O(custom texts)
     * after create().
     */
    [[nodiscard]] Result<std::uint32_t> markerSize(const MarkerOptions& options = {}) const noexcept;

    // The edits that turn ours into resolvedText().text (see OursPatch). Errors: as resolvedText().
    // O(changed output + chunks).
    [[nodiscard]] Result<OursPatch> editsForOurs() const noexcept;

private:
    MergeModel() noexcept = default;

    LineSequence m_base;
    LineSequence m_ours;
    LineSequence m_theirs;
    Merge3 m_merge;
    std::vector<Take> m_takes;
    std::vector<std::string> m_custom; // Per chunk; non-empty only for Take::Custom.
    std::string_view m_eol = "\n";
    std::uint32_t m_unresolved = 0;
    // Bit n - 1 set when a line of the three sequences starts with a run of exactly n marker
    // characters (n <= kMaxMarkerSize).
    std::uint64_t m_markerRuns = 0;
};

} // namespace frost::diff
