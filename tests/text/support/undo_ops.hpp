// SPDX-License-Identifier: Apache-2.0
//
// UndoDriver: drives random operations against a frost::text::Document and the UndoOracle and
// compares them (ADR 0017, section 7.1 of the slice spec). The undo differential, stress,
// property and concurrency tests all run this driver; only their ByteSource and scale differ. No
// gtest dependency and no private header (the .cpp reads the undo tree for its white-box checks).
/*
 * Op mix (percent): Typing at the caret 22; backspace or delete 10; a replace of kind Other, Paste
 * or External 8; a multi-caret Typing batch 5; caret and selection moves 5; clock advances of
 * 0-300 ms 8 and of at least the coalescing window 2; undo 10, redo along redoChild 6, redo of a
 * random branch 2; group begin, end and rollback 4; IME begin, update, commit (with and without
 * replaceInBase) and cancel 5; anchor creation 4 (char boundaries, CRLF-interior offsets and the
 * edges of recent edits) and release 2; breakCoalescing 1; invalid ops 3 (the error rows of the
 * spec's section 3.8); an explicit full check 3. While composing, three in four edit, history and
 * group ops become IME ops, so compositions end; the rest must fail with Busy.
 *
 * Checks after every op: for a rejection, the error code and an unchanged state (version, size,
 * undo state with stateId, composition, anchor count); otherwise the ChangeSet (origin, versions,
 * raw edits, the byte fields of the D8 changes and the selections) against the oracle's, the text
 * window around each change, every live anchor's offset and bias, and undoState(). The full check
 * (every checkEvery ops, on the explicit op and at the end): Document::checkInvariants, the text
 * and committedText(), anchors().all(), the current node's regions (white-box) against the
 * oracle's alignment-derived regions, the node count, current node, depth and redoChild chain, and
 * the LSP replay and points of every step of the last text-changing op.
 *
 * The driver's selections always refer to the committed text (they stay put during a
 * composition), so the Document's FROST_PRE on selection sizes holds. A mismatch returns
 * ErrorCode::Internal naming the label, the op index, the difference and the last 32 ops.
 * Deterministic for a given ByteSource. Not thread-safe.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/text/document.hpp"
#include "frost/text/selection.hpp"

#include "text/support/text_ops.hpp"
#include "text/support/undo_oracle.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::text::test {

struct UndoDriverOptions {
    TextBufferOptions buffer;
    // Generated texts keep documents within this size.
    std::uint64_t maxDocBytes = 16 * 1024;
    std::uint32_t maxTransactions = 1000;
    std::chrono::milliseconds coalesceWindow{1000};
    std::uint32_t checkEvery = 32;       // Full check every this many ops (0: only explicit ones).
    std::uint32_t maxOps = 0xFFFF'FFFFu; // done() after this many ops.
    // Anchors created right after load and never released by the op mix (the concurrency test).
    std::uint32_t trackedAnchors = 0;
    std::uint32_t maxChurnAnchors = 64;
    std::uint32_t maxCarets = 8;
    // Replays every undo, redo and rollback ChangeSet as LSP edits right away, with the new bytes
    // taken from the post-op text only (UndoProperty.ChangesReplayAsLsp).
    bool replayHistoryOps = false;
};

struct UndoDriverStats {
    std::uint64_t ops = 0;
    std::uint64_t edits = 0; // Recorded or not, accepted.
    std::uint64_t noOps = 0; // Accepted edits that changed nothing.
    std::uint64_t undos = 0;
    std::uint64_t redos = 0;
    std::uint64_t groups = 0; // Outermost groups begun.
    std::uint64_t rollbacks = 0;
    std::uint64_t commits = 0;
    std::uint64_t cancels = 0;
    std::uint64_t preedits = 0;
    std::uint64_t anchorOps = 0;
    std::uint64_t rejected = 0; // Ops that failed as the oracle expected.
    std::uint64_t fullChecks = 0;
    std::uint64_t maxBytes = 0;
    std::uint64_t maxNodes = 0;
    std::uint64_t replayChecks = 0; // History ChangeSets replayed as LSP edits (replayHistoryOps).
};

// What the last step did, for the property and concurrency tests.
struct UndoStepInfo {
    const char* kind = "";
    bool changed = false;                           // The text or the anchors changed.
    std::optional<ChangeSet> changeSet;             // From an accepted op that returns one.
    std::optional<SelectionSet> recordedSelections; // The selections the op recorded on the current node.
    std::uint64_t nodeBefore = 0;
    std::uint64_t nodeAfter = 0;
};

class UndoDriver {
public:
    // Loads `initial` (well formed, within the buffer limit) and creates the tracked anchors.
    UndoDriver(UndoDriverOptions options, std::string label, const std::string& initial);
    UndoDriver(const UndoDriver&) = delete ("the Document refers to the driver's clock");
    UndoDriver& operator=(const UndoDriver&) = delete ("the Document refers to the driver's clock");

    // One op chosen by `source`, with the per-op checks and the periodic full check.
    [[nodiscard]] Result<void> step(ByteSource& source);
    // Ends an open group (endGroup or rollbackGroup) and a composition (commit or cancel), chosen
    // by `source`, so the history is navigable. Checked like ops.
    [[nodiscard]] Result<void> settle(ByteSource& source);
    // One undo, or one redo (along redoChild without a branch), through both models, checked.
    [[nodiscard]] Result<void> undoOnce();
    [[nodiscard]] Result<void> redoOnce(std::optional<std::uint32_t> branch = std::nullopt);
    // The full check described above.
    [[nodiscard]] Result<void> verifyFull();

    [[nodiscard]] bool done() const noexcept { return m_stats.ops >= m_options.maxOps; }
    [[nodiscard]] const UndoDriverStats& stats() const noexcept { return m_stats; }
    [[nodiscard]] const Document& document() const noexcept { return m_document; }
    [[nodiscard]] const UndoOracle& oracle() const noexcept { return m_oracle; }
    [[nodiscard]] const UndoStepInfo& lastStep() const noexcept { return m_step; }
    [[nodiscard]] std::span<const std::uint64_t> trackedAnchors() const noexcept { return m_tracked; }
    [[nodiscard]] const SelectionSet& selections() const noexcept { return m_selections; }
    // White-box views of the undo tree: the current node's id, its depth and the redoChild chain.
    [[nodiscard]] std::uint64_t currentNodeId() const noexcept;
    [[nodiscard]] std::uint32_t currentDepth() const noexcept;
    [[nodiscard]] std::vector<std::uint64_t> redoChain() const;
    // The selections the Document last recorded for node `id` (the selectionsAfter of its last
    // edit, commit or endGroup, or the selections of the redo that made it current), and the
    // selections an undo of node `id` last returned; none when not seen.
    [[nodiscard]] std::optional<SelectionSet> recordedSelectionsOf(std::uint64_t id) const;
    [[nodiscard]] std::optional<SelectionSet> undoSelectionsOf(std::uint64_t id) const;

private:
    struct DocState {
        std::uint64_t version = 0;
        std::uint64_t size = 0;
        UndoState undo;
        std::optional<ByteRange> composition;
        std::size_t anchors = 0;
    };
    // The bytes a text-changing op replaced, for the full check's LSP replay.
    struct LastChange {
        bool valid = false;
        ChangeOrigin origin = ChangeOrigin::Edit;
        std::string oldText;
        std::vector<RawEdit> edits;
        std::vector<std::string> inserted;
        std::vector<TextChange> changes;
    };

    [[nodiscard]] Error mismatch(const std::string& what) const;
    void record(std::string description);
    [[nodiscard]] std::chrono::nanoseconds now() const noexcept;
    [[nodiscard]] DocState state() const;
    [[nodiscard]] std::uint64_t randomBoundary(ByteSource& source) const;
    [[nodiscard]] std::uint64_t room(std::uint64_t removed) const noexcept;
    [[nodiscard]] std::uint64_t primaryCaret(ByteSource& source) const;
    void setSelections(SelectionSet selections, OracleSelections oracle);

    // Compares a ChangeSet-returning op; `onAccepted` runs when both accepted it.
    [[nodiscard]] Result<void> compare(const char* what, const Result<ChangeSet>& got,
                                       const OracleResult<OracleChangeSet>& expected, const DocState& before,
                                       std::string oldText);
    [[nodiscard]] Result<void> compareVoid(const char* what, const Result<void>& got,
                                           const std::optional<ErrorCode>& expected, const DocState& before);
    [[nodiscard]] Result<void> checkRejected(const char* what, ErrorCode got, ErrorCode expected,
                                             const DocState& before);
    [[nodiscard]] Result<void> checkChangeSet(const ChangeSet& got, const OracleChangeSet& expected);
    [[nodiscard]] Result<void> checkAfterOp();

    [[nodiscard]] Result<void> runEdit(const char* what, std::vector<OracleEdit> edits, EditKind kind,
                                       const SelectionSet& after, const OracleSelections& oracleAfter, bool viaReplace);
    [[nodiscard]] Result<void> typing(ByteSource& source);
    [[nodiscard]] Result<void> deletion(ByteSource& source);
    [[nodiscard]] Result<void> replaceOp(ByteSource& source);
    [[nodiscard]] Result<void> multiCaret(ByteSource& source);
    [[nodiscard]] Result<void> moveSelections(ByteSource& source);
    [[nodiscard]] Result<void> undoOp();
    [[nodiscard]] Result<void> redoOp(std::optional<std::uint32_t> branch);
    [[nodiscard]] Result<void> groupOp(ByteSource& source);
    [[nodiscard]] Result<void> endGroupOp();
    [[nodiscard]] Result<void> rollbackOp();
    [[nodiscard]] Result<void> imeOp(ByteSource& source);
    [[nodiscard]] Result<void> commitOp(ByteSource& source, bool withRange);
    [[nodiscard]] Result<void> cancelOp();
    [[nodiscard]] Result<void> createAnchorOp(ByteSource& source);
    [[nodiscard]] Result<void> releaseAnchorOp(ByteSource& source);
    [[nodiscard]] Result<void> invalidOp(ByteSource& source);
    [[nodiscard]] Result<void> dispatch(ByteSource& source);
    // After an op: files m_step's recorded and undo selections under the nodes they belong to.
    void noteStep(std::uint64_t nodeBefore);

    UndoDriverOptions m_options;
    std::string m_label;
    ManualClock m_clock; // Before m_document, which refers to it.
    Document m_document;
    UndoOracle m_oracle;
    SelectionSet m_selections;
    OracleSelections m_oracleSelections;
    std::vector<std::uint64_t> m_tracked;
    std::vector<std::uint64_t> m_churn;
    std::vector<std::uint64_t> m_released;
    std::vector<std::uint64_t> m_edges; // Offsets at the edges of recent edits (may be stale).
    LastChange m_last;
    UndoStepInfo m_step;
    UndoDriverStats m_stats;
    std::array<std::string, 32> m_recent{};
    std::uint64_t m_opIndex = 0;
    std::map<std::uint64_t, SelectionSet> m_recordedByNode;
    std::map<std::uint64_t, SelectionSet> m_undoneByNode;
};

/*
 * One seeded run (UndoDifferential, UndoStress): the seed picks maxPieceBytes from
 * {4, 5, 7, 16, 4096}[seed % 5], maxTransactions from {1000, 8, 1}[seed % 3], a small buffer limit
 * for one seed in seven (seedBufferLimit, seed % 7 == 3) and an initial text of 0-4 KiB (empty when
 * (seed / 15) % 4 == 0). 3, 5 and 7 are coprime, so every combination occurs. Then `ops` driver
 * steps from SplitMixSource(seed) and a final full check.
 */
struct UndoSeedRun {
    std::uint64_t seed = 0;
    std::uint32_t ops = 1000;
    std::uint32_t checkEvery = 32;

    friend constexpr bool operator==(const UndoSeedRun&, const UndoSeedRun&) noexcept = default;
};
[[nodiscard]] UndoDriverOptions undoSeedOptions(std::uint64_t seed);
[[nodiscard]] std::string undoSeedInitialText(std::uint64_t seed, const UndoDriverOptions& options);
// The driver's stats, or its first mismatch (ErrorCode::Internal, labelled "seed <s>").
[[nodiscard]] Result<UndoDriverStats> runUndoSeed(const UndoSeedRun& run);

/*
 * UndoDifferential.Quick's settings: the seeds 0..199, or only FROST_UNDO_SEED, each run as
 * UndoSeedRun{seed, FROST_UNDO_OPS (default 1000), FROST_UNDO_CHECK_EVERY (32)} on
 * FROST_UNDO_THREADS worker threads (default min(4, hardware threads / 2); 1 with
 * FROST_UNDO_SEED). Errors: InvalidArgument naming a variable whose value is not a decimal number
 * in range (ops and checks 32-bit, threads 1..1024).
 */
struct UndoQuickSettings {
    std::optional<std::uint64_t> onlySeed;
    UndoSeedRun run;
    std::optional<unsigned> threads;
};
[[nodiscard]] Result<UndoQuickSettings> undoQuickSettings(const EnvironmentLookup& lookup);
// The command that re-runs exactly `run` through UndoDifferential.Quick (undoQuickSettings on its
// assignments yields `run` again; UndoDifferential.ReproduceCommandRoundTrips).
[[nodiscard]] std::string reproduceCommand(const UndoSeedRun& run);

// Replays `changes` (an undo, redo or rollback ChangeSet's, in application order) as LSP edits on
// `oldText`, taking each change's new bytes from `newText` by the extraction rule of the spec's
// section 3.9 only; checks the points and that no bound is CRLF-interior. Returns the first
// failure, or nothing. O(changes × bytes).
[[nodiscard]] std::optional<std::string> checkLspReplayByExtraction(std::string_view oldText, std::string_view newText,
                                                                    std::span<const TextChange> changes);

// Conversions between the Document's and the oracle's selection values.
[[nodiscard]] OracleSelections toOracle(const SelectionSet& selections);
[[nodiscard]] SelectionSet toDocument(const OracleSelections& selections);

} // namespace frost::text::test
