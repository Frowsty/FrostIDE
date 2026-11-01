// SPDX-License-Identifier: Apache-2.0
//
// UndoOracle: a naive std::string model of frost::text::Document, the independent oracle of the
// undo and anchor differential, stress and concurrency tests (ADR 0017, section 7.1 of the slice
// spec). No gtest dependency and no private header.
/*
 * Independence. The oracle shares no code with Document, AnchorSet, SelectionSet or the undo tree;
 * it uses only the plain value types (RawEdit, Bias, EditKind, ChangeOrigin, ErrorCode) and the
 * TextOracle helpers (UTF-8 boundaries, the edit check order, D8 normalisation):
 *   - every state is a node holding its full text; nodes hold parent, children, redoChild,
 *     sequence, stateId, kind, lastEdit, the open flag, both selection sets and both anchor maps;
 *   - the open (or group) transaction keeps an `origin` vector (for each byte of its text, its X
 *     index or -1), updated per raw step with vector erase and insert; sealing derives the
 *     regions from it by the definition (gaps of the alignment of kept bytes) and drops it;
 *   - anchors are a vector mapped by applying decision 1's rule edit by edit, last to first; undo
 *     and redo apply decision 10 literally (an anchor present in anchorsBefore/After takes that
 *     offset, every other one is mapped), never the closed-range scan;
 *   - selections are normalised by brute force: re-sort and merge the lowest mergeable adjacent
 *     pair until none remains;
 *   - coalescing, groups, IME (with replaceInBase), the ChangeSet table, stateId and pruning with
 *     redoChild repair are reimplemented from the slice spec's section 3.
 * Errors are ErrorCodes in the shared order (section 3.8). Not thread-safe.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/text/anchor.hpp"
#include "frost/text/document.hpp"

#include "text/support/text_oracle.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::text::test {

// A selection as two plain offsets.
struct OracleSelection {
    std::uint64_t anchor = 0;
    std::uint64_t head = 0;

    [[nodiscard]] std::uint64_t lo() const noexcept { return anchor < head ? anchor : head; }
    [[nodiscard]] std::uint64_t hi() const noexcept { return anchor < head ? head : anchor; }
    [[nodiscard]] bool empty() const noexcept { return anchor == head; }
    [[nodiscard]] bool reversed() const noexcept { return head < anchor; }
    friend bool operator==(const OracleSelection&, const OracleSelection&) = default;
};

// A normalised selection set (primary 0 when empty).
struct OracleSelections {
    std::vector<OracleSelection> items;
    std::size_t primary = 0;
    friend bool operator==(const OracleSelections&, const OracleSelections&) = default;
};

// Brute-force normalisation (section 3.2). With `origins`, the input index of each result's first
// member in sweep order. Precondition: primary < input.size() when non-empty. O(n^3) worst case.
[[nodiscard]] OracleSelections oracleNormalise(std::span<const OracleSelection> input, std::size_t primary,
                                               std::vector<std::uint32_t>* origins = nullptr);
// Decision 1's rule for one replace of [a, b) by n bytes.
[[nodiscard]] std::uint64_t oracleMapOffset(std::uint64_t x, Bias bias, std::uint64_t a, std::uint64_t b,
                                            std::uint64_t n) noexcept;
// A batch, sorted as applyEdits requires: the rule edit by edit, last to first.
[[nodiscard]] std::uint64_t oracleMapBatch(std::uint64_t x, Bias bias, std::span<const RawEdit> batch) noexcept;
// Selections through a batch with derived biases (caret: both Right; else lower Left, upper Right),
// directions kept, then normalised by brute force.
[[nodiscard]] OracleSelections oracleTransform(const OracleSelections& selections, std::span<const RawEdit> batch,
                                               std::vector<std::uint32_t>* origins = nullptr);

struct OracleAnchor {
    std::uint64_t id = 0;
    std::uint64_t offset = 0;
    Bias bias = Bias::Left;
    friend bool operator==(const OracleAnchor&, const OracleAnchor&) = default;
};

struct OracleRegion {
    std::uint64_t beforeBegin = 0;
    std::uint64_t beforeEnd = 0;
    std::uint64_t afterBegin = 0;
    std::uint64_t afterEnd = 0;
    friend bool operator==(const OracleRegion&, const OracleRegion&) = default;
};

// The regions of the alignment `origin` (for each byte of Y its X index, or -1) over an X of
// xSize bytes, by the definition in section 3.3. O(|Y|).
[[nodiscard]] std::vector<OracleRegion> oracleRegions(std::span<const std::int32_t> origin, std::uint64_t xSize);

// One requested edit.
struct OracleEdit {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::string text;
};

// What the oracle expects an operation to report. `inserted` holds the bytes each raw edit inserts,
// for the driver's LSP replay of the steps.
struct OracleChangeSet {
    ChangeOrigin origin = ChangeOrigin::Edit;
    std::uint64_t baseVersion = 0;
    std::uint64_t version = 0;
    std::vector<RawEdit> edits;
    std::vector<ByteChange> changes;
    OracleSelections selections;
    std::vector<std::string> inserted; // Per raw edit, ascending like `edits`.
};

struct OracleUndoState {
    bool canUndo = false;
    bool canRedo = false;
    std::uint32_t redoBranches = 0;
    std::uint32_t transactions = 0;
    bool groupOpen = false;
    bool composing = false;
    std::uint64_t stateId = 0;
    friend bool operator==(const OracleUndoState&, const OracleUndoState&) = default;
};

template<class T>
using OracleResult = std::expected<T, ErrorCode>;

class UndoOracle {
public:
    // `text` must be well formed and fit maxBufferBytes.
    UndoOracle(std::string text, std::uint64_t maxBufferBytes, std::chrono::nanoseconds coalesceWindow,
               std::uint32_t maxTransactions);

    // The text including the preedit.
    [[nodiscard]] const std::string& text() const noexcept { return m_textOracle.text(); }
    // The text without the preedit.
    [[nodiscard]] const std::string& committedText() const noexcept;
    [[nodiscard]] std::uint64_t version() const noexcept { return m_version; }
    [[nodiscard]] OracleUndoState undoState() const;
    // Live anchors, sorted by id.
    [[nodiscard]] const std::vector<OracleAnchor>& anchors() const noexcept { return m_anchors; }
    [[nodiscard]] std::optional<OracleAnchor> anchor(std::uint64_t id) const;
    [[nodiscard]] bool composing() const noexcept { return m_composition.has_value(); }
    [[nodiscard]] bool groupOpen() const noexcept { return m_groupDepth > 0; }
    [[nodiscard]] std::uint32_t groupDepth() const noexcept { return m_groupDepth; }
    // [start, start + length) of the preedit.
    [[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>> compositionRange() const;
    // The composition base's size (committedText().size()).
    [[nodiscard]] std::uint64_t committedSize() const noexcept { return committedText().size(); }
    [[nodiscard]] const TextOracle& textOracle() const noexcept { return m_textOracle; }
    [[nodiscard]] std::uint64_t maxBufferBytes() const noexcept { return m_maxBufferBytes; }

    // The current node's id, its children's count, the node count excluding the root, the current
    // depth and the redoChild chain from the current node (ids).
    [[nodiscard]] std::uint64_t currentNode() const noexcept { return m_current; }
    [[nodiscard]] std::uint32_t currentDepth() const;
    [[nodiscard]] std::vector<std::uint64_t> redoChain() const;
    [[nodiscard]] std::uint64_t nodeCount() const noexcept { return m_nodeCount; }
    // The current node's regions (derived from the origin vector when its transaction is still
    // extendable); empty for the root.
    [[nodiscard]] std::vector<OracleRegion> currentRegions() const;

    // Operations, in the shared error order. `now` is the IClock time of the call.
    [[nodiscard]] OracleResult<std::uint64_t> createAnchor(std::uint64_t offset, Bias bias);
    [[nodiscard]] std::optional<ErrorCode> releaseAnchor(std::uint64_t id);
    // Document::edit, and Document::replace as a one-edit batch (the same check order).
    [[nodiscard]] OracleResult<OracleChangeSet> edit(std::span<const OracleEdit> edits, EditKind kind,
                                                     const OracleSelections& before, const OracleSelections& after,
                                                     std::chrono::nanoseconds now);
    void breakCoalescing();
    [[nodiscard]] std::optional<ErrorCode> beginGroup(EditKind kind, const OracleSelections& before);
    [[nodiscard]] std::optional<ErrorCode> endGroup(const OracleSelections& after);
    [[nodiscard]] OracleResult<OracleChangeSet> rollbackGroup();
    [[nodiscard]] OracleResult<OracleChangeSet> undo();
    [[nodiscard]] OracleResult<OracleChangeSet> redo(std::optional<std::uint32_t> branch);
    [[nodiscard]] std::optional<ErrorCode> beginComposition(std::uint64_t at);
    [[nodiscard]] OracleResult<OracleChangeSet> updateComposition(std::string_view preedit);
    [[nodiscard]] OracleResult<OracleChangeSet>
    commitComposition(std::string_view text, const OracleSelections& before, const OracleSelections& after,
                      std::optional<std::pair<std::uint64_t, std::uint64_t>> replaceInBase,
                      std::chrono::nanoseconds now);
    [[nodiscard]] OracleResult<OracleChangeSet> cancelComposition();

private:
    struct Node {
        bool alive = false;
        std::uint64_t parent = 0;
        std::vector<std::uint64_t> children;
        std::optional<std::uint64_t> redoChild;
        std::uint64_t sequence = 0;
        std::uint64_t stateId = 0;
        std::string text;
        EditKind kind = EditKind::Other;
        OracleSelections selectionsBefore;
        OracleSelections selectionsAfter;
        std::vector<OracleAnchor> anchorsBefore;
        std::vector<OracleAnchor> anchorsAfter;
        std::chrono::nanoseconds lastEdit{0};
        bool open = false;
        bool extendable = false;           // Receives edits (open, or the group node): `origin` is live.
        std::vector<std::int32_t> origin;  // While extendable.
        std::vector<OracleRegion> regions; // Once sealed.
    };
    struct Composition {
        std::uint64_t start = 0;
        std::uint64_t length = 0;
        std::string base;
        std::vector<OracleAnchor> baseAnchors;
    };

    [[nodiscard]] Node& node(std::uint64_t id);
    [[nodiscard]] const Node& node(std::uint64_t id) const;
    [[nodiscard]] std::vector<OracleRegion> regionsOf(const Node& n) const;
    void seal(Node& n);
    void sealCurrent();
    // Applies `edits` (validated, ascending) to m_text, mapping the anchors; returns the expected
    // D8 changes in application order. Bumps the version when something changed.
    [[nodiscard]] std::vector<ByteChange> apply(std::span<const RawEdit> raw, std::span<const std::string> inserted);
    void record(const std::string& beforeText, std::span<const RawEdit> raw, EditKind kind,
                const OracleSelections& before, const OracleSelections& after,
                const std::vector<OracleAnchor>& anchorsBefore, std::chrono::nanoseconds now);
    [[nodiscard]] OracleChangeSet undoNode(std::uint64_t id, ChangeOrigin origin);
    void restoreAnchors(const std::vector<OracleAnchor>& from);
    std::uint64_t createChild(std::uint64_t parent);
    void prune();
    void eraseSubtree(std::uint64_t id);
    [[nodiscard]] OracleResult<OracleChangeSet> compositionChange(std::string_view text);

    std::uint64_t m_maxBufferBytes;
    std::chrono::nanoseconds m_window;
    std::uint32_t m_maxTransactions;
    TextOracle m_textOracle; // The current text, with the boundary and check-order helpers.
    std::uint64_t m_version = 0;
    std::vector<OracleAnchor> m_anchors;
    std::uint64_t m_nextAnchorId = 1;
    std::vector<Node> m_nodes; // Indexed by id; dead nodes stay as tombstones.
    std::uint64_t m_root = 0;
    std::uint64_t m_current = 0;
    std::uint64_t m_nextSequence = 1;
    std::uint64_t m_nodeCount = 1;
    std::uint64_t m_stateCounter = 0;
    std::uint32_t m_groupDepth = 0;
    std::optional<std::uint64_t> m_groupNode;
    EditKind m_groupKind = EditKind::Other;
    OracleSelections m_groupBefore;
    std::optional<std::uint64_t> m_groupPreviousRedo;
    std::optional<Composition> m_composition;
};

} // namespace frost::text::test
