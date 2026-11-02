// SPDX-License-Identifier: Apache-2.0
//
// MergePanes: the Qt-free layout of the minimal merge view (PLAN.md section 7 M1 "external-change
// handling with a minimal compare/merge view"; ADR 0042). It puts the three sides of a
// diff::MergeModel (yours, base, disk) into one display map each and aligns every changed chunk
// across the panes with filler rows (text::BlockDecoration, ADR 0026), so row r shows the same
// chunk in all three panes and one scroll position fits them all.
/*
 * Alignment. Chunk c spans H = max(lines in yours, base, disk) rows in every pane: a pane whose side
 * has fewer lines gets H - n filler rows, below the chunk's last line, or above the line after the
 * chunk when the side is empty there (above the trailing empty line when the side ends with a line
 * break). Stable chunks are equal on all sides and need none. Filler tags are chunk index + 1.
 *
 * Lifetime: the panes copy the three texts and borrow `shaper` (it must outlive them). Not
 * thread-safe (one owner, as DisplayMap).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/diff/merge.hpp"
#include "frost/text/display_map.hpp"
#include "frost/text/text_shaper.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace frost::ui::nav {

// The panes, left to right.
enum class MergePane : std::uint8_t { Yours, Base, Disk };

class MergePanes {
public:
    // Errors: InvalidArgument when a side is not valid UTF-8; the display map's (too many rows).
    // O(text + chunks log n).
    [[nodiscard]] static Result<MergePanes> create(const diff::MergeModel& model, const text::ITextShaper& shaper);

    [[nodiscard]] const text::DisplayMap& map(MergePane pane) const noexcept { return m_maps[index(pane)]; }
    // The most rows of any pane: they differ at most by the trailing empty line of a side that ends
    // with a line break where another does not. O(panes).
    [[nodiscard]] std::uint32_t rowCount() const noexcept;
    // The number of chunks of the model the panes were built from.
    [[nodiscard]] std::size_t chunkCount() const noexcept { return m_chunks.size(); }
    [[nodiscard]] const diff::MergeChunk& chunk(std::size_t index) const noexcept { return m_chunks[index]; }
    // The first display row of `chunk` in `pane` (equal in every pane). Precondition: chunk < chunkCount().
    [[nodiscard]] text::DisplayRow chunkRow(MergePane pane, std::size_t chunk) const;
    // The chunk shown at `row` of `pane`, or nullopt past the end. O(log n).
    [[nodiscard]] std::optional<std::size_t> chunkAt(MergePane pane, text::DisplayRow row) const;
    // The number of filler rows `pane` got.
    [[nodiscard]] std::uint32_t fillerRows(MergePane pane) const noexcept { return m_fillerRows[index(pane)]; }

private:
    [[nodiscard]] static constexpr std::size_t index(MergePane pane) noexcept { return static_cast<std::size_t>(pane); }
    MergePanes() = default;

    std::vector<text::DisplayMap> m_maps;
    std::vector<diff::MergeChunk> m_chunks;
    // Per pane: the chunk of every line of the side (the trailing empty display line maps to none).
    std::array<std::vector<std::uint32_t>, 3> m_chunkOfLine{};
    // Per pane and chunk: the filler block, when the pane got one.
    std::array<std::vector<std::optional<text::BlockId>>, 3> m_fillers{};
    std::array<std::uint32_t, 3> m_fillerRows{};
};

} // namespace frost::ui::nav
