// SPDX-License-Identifier: Apache-2.0
//
// The piece-tree item (ADR 0012, D3-D5): a Piece names a run of bytes in the text storage and
// carries its TextSummary; PieceTraits makes SumTree<PieceTraits, 8> the piece tree. Not public
// API: snapshot.hpp needs the complete types for its members.
#pragma once

#include "frost/text/sum_tree.hpp"
#include "frost/text/text_summary.hpp"

#include <cstddef>
#include <string_view>

namespace frost::text::detail {

/*
 * A run of 1..maxPieceBytes bytes of well-formed UTF-8 in the storage (the original text or an
 * add chunk). The bytes never move or change while any owner of the storage lives (D5), so the
 * raw pointer stays valid as long as the tree holding the piece is reachable from a Snapshot or
 * TextBuffer. Its length is summary.bytes. Trivially copyable; 72 bytes.
 */
struct Piece {
    const char* data = nullptr;
    TextSummary summary{};

    [[nodiscard]] std::size_t size() const noexcept { return static_cast<std::size_t>(summary.bytes); }
    [[nodiscard]] std::string_view view() const noexcept { return {data, size()}; }
};
static_assert(sizeof(Piece) == 72, "Piece is 72 bytes (ADR 0012: leaf size)");

// SumTree traits of the piece tree: a piece's summary is its cached TextSummary, combined with
// text::combine.
struct PieceTraits {
    using Item = Piece;
    using Summary = TextSummary;

    [[nodiscard]] static const TextSummary& summary(const Piece& piece) noexcept { return piece.summary; }
    [[nodiscard]] static TextSummary combine(const TextSummary& a, const TextSummary& b) noexcept {
        return text::combine(a, b);
    }
};

// The piece tree: 8..16 pieces per leaf, 8..16 children per internal node (ADR 0012, fan-out).
inline constexpr int kPieceTreeFanout = 8;
using PieceTree = SumTree<PieceTraits, kPieceTreeFanout>;

class Storage; // src/text/src/storage.hpp; owned through Ref<Storage> by TextBuffer and Snapshots.

} // namespace frost::text::detail
