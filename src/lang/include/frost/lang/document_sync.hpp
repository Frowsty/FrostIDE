// SPDX-License-Identifier: Apache-2.0
//
// Text-document synchronisation helpers (LSP 3.17 "Text Document Synchronization"): positions in
// the negotiated encoding, incremental didChange content changes built from frost::text
// ChangeSets (ADR 0017's D8 TextChanges), and their JSON form.
/*
 * Encodings. clangd negotiates "utf-8" (columns are UTF-8 bytes, frost::text's own ByteColumn);
 * other servers may keep LSP's default "utf-16". Both are supported; "utf-32" is never offered.
 *
 * Changes. A ChangeSet's TextChanges are in application order, each in its own step's coordinates,
 * and frost::text applies a batch last to first, so change i+1 lies before change i. That is the
 * order LSP replays contentChanges in, so each TextChange becomes one range change: the range is
 * its old points (UTF-16 columns are computed in the snapshot before the batch, which agrees with
 * step i's text up to the change because every earlier step lies after it), and the new text is
 * read from the snapshot after the batch, shifted by the later steps' size deltas (ADR 0017,
 * "New-text extraction"). When the shape does not allow that (changes overlapping after D8's
 * widening, out of order, or sizes that do not add up), the result is one full-text change, which
 * is always correct. All functions are pure and thread-safe.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/strong_type.hpp"
#include "frost/lang/json.hpp"
#include "frost/text/document.hpp"
#include "frost/text/snapshot.hpp"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::lang {

enum class PositionEncoding : std::uint8_t { Utf8, Utf16 };

// "utf-8" or "utf-16" (LSP's PositionEncodingKind).
[[nodiscard]] std::string_view toString(PositionEncoding encoding) noexcept;
// Parses a PositionEncodingKind; nullopt for "utf-32" and unknown kinds.
[[nodiscard]] std::optional<PositionEncoding> parsePositionEncoding(std::string_view kind) noexcept;

// An LSP Position: a zero-based line and a column in the encoding's units.
struct LspPosition {
    std::uint32_t line = 0;
    std::uint32_t character = 0;
    friend constexpr auto operator<=>(const LspPosition&, const LspPosition&) noexcept = default;
};

struct LspRange {
    LspPosition start;
    LspPosition end;
    friend constexpr bool operator==(const LspRange&, const LspRange&) noexcept = default;
};

[[nodiscard]] Json toJson(LspPosition position);
[[nodiscard]] Json toJson(const LspRange& range);
// Reads {"line", "character"}; nullopt for anything else (negative or missing members).
[[nodiscard]] std::optional<LspPosition> parsePosition(const Json& json);
[[nodiscard]] std::optional<LspRange> parseRange(const Json& json);

// The LSP position of `offset` in `snapshot`. Precondition: offset <= snapshot.size(). O(log n).
[[nodiscard]] LspPosition toLspPosition(const text::Snapshot& snapshot, ByteOffset offset,
                                        PositionEncoding encoding) noexcept;
// The offset of an LSP position, with Snapshot::offsetAt's clamping rules (a column past the line
// end clamps to it). Errors: OutOfRange (line >= lineCount()). O(log n).
[[nodiscard]] Result<ByteOffset> fromLspPosition(const text::Snapshot& snapshot, LspPosition position,
                                                 PositionEncoding encoding) noexcept;

// One TextDocumentContentChangeEvent: a range edit, or the whole new text when `range` is empty.
struct ContentChange {
    std::optional<LspRange> range;
    std::string text;
    friend bool operator==(const ContentChange&, const ContentChange&) = default;
};

[[nodiscard]] Json toJson(const ContentChange& change);

/*
 * The didChange content changes that take the server from `before` (the snapshot at
 * changes.baseVersion) to `after` (at changes.version); see the header comment. Empty when the
 * ChangeSet changed nothing. Precondition: the snapshots belong to the ChangeSet. O(changes ×
 * log n + new bytes), or O(size of after) for the full-text fallback.
 */
[[nodiscard]] std::vector<ContentChange> contentChanges(const text::ChangeSet& changes, const text::Snapshot& before,
                                                        const text::Snapshot& after, PositionEncoding encoding);

// The languageId for a path by extension: "c" (.c), "objective-c" (.m), "objective-cpp" (.mm),
// "cuda" (.cu), and "cpp" for everything else (C++ sources and headers, .h included).
[[nodiscard]] std::string_view languageIdForPath(std::string_view path) noexcept;

} // namespace frost::lang
