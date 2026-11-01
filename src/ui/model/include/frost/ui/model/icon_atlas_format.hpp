// SPDX-License-Identifier: Apache-2.0
//
// The icon atlas format (FIA v1, one file per scale, icons-<pct>.fia): the validating
// zero-copy view used at run time, the deterministic writer and shelf packer used by the
// build-time baker, and the rule that picks which baked cell serves a requested size
// (ADR 0015 decision 7). Qt-free.
//
/*
 * Layout, little-endian:
 *
 *   Header, 64 bytes
 *     [0,8)   magic "FROSTICO"
 *     [8,10)  version = 1             [10,12) scalePercent (one of kAtlasScales)
 *     [12,16) entryCount              [16,20) entryOffset = 64
 *     [20,24) idTableOffset           [24,28) idTableSize
 *     [28,30) pageCount               [30,32) pageLimit: the packer's maximum page edge
 *     [32,36) pageTableOffset         [36,40) totalSize
 *     [40,48) sourceHash: FNV-1a 64 over the format version, the manifest and every SVG, in
 *             manifest order (equal in every atlas of one bake)
 *     [48,56) tableChecksum: FNV-1a 64 over the header with this field zeroed, then the entry
 *             table, the id table and the page table (pixels are not covered)
 *     [56,64) reserved, 0
 *   Entries, 32 bytes each, sorted by (id, designSize), unique
 *     [0,4) idOffset into the id table  [4,6) idLen  [6] designSize  [7] flags (bit 0 tinted,
 *     bit 1 hasDark)  [8] role  [9] reserved  [10,12) page  [12..20) x, y, w, h
 *     [20..26) darkPage, darkX, darkY  [26,32) zero
 *   Id table: each distinct id once, in entry order, no separators.
 *   Pages, 16 bytes each: [0,4) offset (64-byte aligned)  [4,6) width  [6,8) height
 *     [8,12) stride = align4(width x bytes per pixel)  [12] format  [13,16) zero
 *   Pixels: each page's rows at its offset; zero padding between tables and pages and at the
 *     end of every row.
 *
 * Alpha8 pages hold tint masks (one byte of coverage per pixel). Argb32Premultiplied pages
 * hold full-colour icons as little-endian 32-bit 0xAARRGGBB values, premultiplied: the memory
 * order of QImage::Format_ARGB32_Premultiplied on little-endian hosts, so QImage can wrap the
 * rows without converting them.
 *
 * Only the canonical encoding parses: tables at the offsets above in that order, each page
 * right after the previous one at the next 64-byte boundary, totalSize equal to the end of
 * the last page, every cell where packShelves(pageLimit) puts it and exactly devicePx square,
 * every page exactly the packer's size, and all padding and every pixel outside the cells
 * zero. So parsing a file and re-writing its content gives identical bytes (the fuzz harness
 * checks this).
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/ui/model/theme_registry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::ui {

enum class PixelFormat : std::uint8_t { Alpha8 = 1, Argb32Premultiplied = 2 };

inline constexpr std::uint16_t kIconAtlasVersion = 1;
inline constexpr std::size_t kAtlasHeaderSize = 64;
inline constexpr std::size_t kAtlasEntrySize = 32;
inline constexpr std::size_t kAtlasPageRecordSize = 16;
// Page pixel data starts on 64-byte boundaries (cache lines; QImage needs only 4).
inline constexpr std::size_t kAtlasPageAlignment = 64;
// The largest page edge in pixels, and the smallest page limit a file may declare.
inline constexpr std::uint16_t kMaxAtlasPage = 2048;
inline constexpr std::uint16_t kMinAtlasPageLimit = 16;
// Atlas files above this size are refused before they are read.
inline constexpr std::uint64_t kMaxAtlasFileBytes = 16 * 1024 * 1024;
// The baked scales, in percent of the design size (PLAN.md section 2, "Icons and fonts").
inline constexpr std::array<std::uint16_t, 7> kAtlasScales{100, 125, 150, 175, 200, 250, 300};
// The icon design grids in logical pixels.
inline constexpr std::array<std::uint8_t, 3> kIconDesignSizes{14, 16, 20};

// Bytes per pixel of `format`: 1 or 4.
[[nodiscard]] constexpr std::uint32_t bytesPerPixel(PixelFormat format) noexcept {
    return format == PixelFormat::Alpha8 ? 1U : 4U;
}

// The device size of a design size at a scale: lround(designSize x scalePercent / 100), so a
// 14 px icon is 18 px at 125% and 25 px at 175%.
[[nodiscard]] constexpr std::uint16_t iconDevicePx(std::uint8_t designSize, std::uint16_t scalePercent) noexcept {
    return static_cast<std::uint16_t>((std::uint32_t{designSize} * scalePercent + 50) / 100);
}

// True when `id` matches [a-z0-9]+(\.[a-z0-9-]+)+ ("actions.search", "nav.chevron-right"). Pure.
[[nodiscard]] bool isValidIconId(std::string_view id) noexcept;

// One decoded entry. `id` views into the atlas bytes.
struct AtlasEntryView {
    std::string_view id;
    std::uint8_t designSize = 0;
    bool tinted = false;
    IconRole role = IconRole::Default;
    bool hasDark = false;
    std::uint16_t page = 0, x = 0, y = 0, w = 0, h = 0;
    std::uint16_t darkPage = 0, darkX = 0, darkY = 0;
};

// One decoded page. `pixels` views stride x height bytes of the atlas.
struct AtlasPageView {
    PixelFormat format = PixelFormat::Alpha8;
    std::uint16_t width = 0, height = 0;
    std::uint32_t stride = 0;
    std::span<const std::byte> pixels;
};

/*
 * A validated, non-owning view of one atlas file. The bytes must outlive the view (IconAtlasSet
 * owns them). Immutable after parse; concurrent const use from any thread is safe.
 */
class IconAtlasView {
public:
    /*
     * Validates every header field, table and page of `bytes` (the canonical layout above): the
     * size, magic, version, a known scale, the table checksum, table offsets and sizes (64-bit
     * arithmetic, no overflow), ids matching isValidIconId and strictly ascending (id, size)
     * order, design sizes in kIconDesignSizes, roles below IconRole::kCount, flag bits, the dark
     * cell rules (untinted entries only, same size, inside its page, zero fields otherwise),
     * every cell devicePx square, inside its page and on a page of the right format (tinted on
     * Alpha8, untinted on ARGB), canonical strides, zero padding and reserved fields, and the
     * canonical packing (see the layout notes). Pixels inside cells are neither hashed nor
     * checked. Errors: Corrupt (any of the above), Unsupported (another version).
     * O(n log n + page pixels); about 0.1 ms for a 240 KB atlas. Budget: B1.
     */
    [[nodiscard]] static Result<IconAtlasView> parse(std::span<const std::byte> bytes);

    [[nodiscard]] std::uint16_t scalePercent() const noexcept { return m_scalePercent; }
    [[nodiscard]] std::uint64_t sourceHash() const noexcept { return m_sourceHash; }
    // The page limit the file was packed with (header [30,32)).
    [[nodiscard]] std::uint16_t pageLimit() const noexcept { return m_pageLimit; }
    [[nodiscard]] std::size_t entryCount() const noexcept { return m_entryCount; }
    [[nodiscard]] std::size_t pageCount() const noexcept { return m_pageCount; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return m_bytes; }

    // Entry `index` (< entryCount()). O(1).
    [[nodiscard]] AtlasEntryView entry(std::size_t index) const noexcept;
    // Page `index` (< pageCount()). O(1).
    [[nodiscard]] AtlasPageView page(std::size_t index) const noexcept;

    // The index of the entry for (id, designSize), by binary search. O(log n).
    [[nodiscard]] std::optional<std::uint32_t> find(std::string_view id, std::uint8_t designSize) const noexcept;

    // The design sizes baked for `id`, ascending (empty for an unknown id). O(log n).
    [[nodiscard]] std::vector<std::uint8_t> designSizesOf(std::string_view id) const;

private:
    IconAtlasView() = default;
    [[nodiscard]] static Result<void> checkCanonicalLayout(const IconAtlasView& view, std::uint16_t maxPage);
    // The index of the first entry whose id is not less than `id`.
    [[nodiscard]] std::size_t lowerBound(std::string_view id, std::uint8_t designSize) const noexcept;

    std::span<const std::byte> m_bytes;
    std::uint16_t m_scalePercent = 0;
    std::uint64_t m_sourceHash = 0;
    std::uint16_t m_pageLimit = 0;
    std::size_t m_entryCount = 0;
    std::size_t m_pageCount = 0;
    std::size_t m_idTableOffset = 0;
    std::size_t m_pageTableOffset = 0;
};

// One cell's pixels for AtlasWriter: `width` x `height`, rows packed tightly
// (width x bytesPerPixel bytes per row).
struct AtlasCell {
    std::uint16_t width = 0, height = 0;
    std::vector<std::byte> pixels;
};

// A packing request: a cell of width x height on a page of class `pageClass` (cells of
// different classes never share a page). id, designSize and variant only order the packing.
struct CellRequest {
    std::uint16_t width = 0, height = 0;
    std::uint8_t pageClass = 0;
    std::string_view id;
    std::uint8_t designSize = 0;
    std::uint8_t variant = 0; // 0 for an icon's cell, 1 for its dark variant.
};

struct PackedCell {
    std::uint16_t page = 0, x = 0, y = 0;
};

struct PackedPage {
    std::uint16_t width = 0, height = 0;
    std::uint8_t pageClass = 0;
};

// packShelves' result: cells[i] places request i; pages lists every page created.
struct PackedLayout {
    std::vector<PackedCell> cells;
    std::vector<PackedPage> pages;
};

/*
 * A deterministic shelf packer. Requests are placed in the order (pageClass, height descending,
 * id, designSize, variant, request index) left to right on shelves as tall as their first cell,
 * with a 1 px transparent gap around every cell (and at the page edges); a cell that does not
 * fit the shelf starts a new shelf, one that does not fit the page starts a new page of the same
 * class. Page sizes are the extent actually used. Errors: OutOfRange when a cell plus its
 * padding exceeds maxPage in either direction. O(n log n).
 */
[[nodiscard]] Result<PackedLayout> packShelves(std::span<const CellRequest> requests,
                                               std::uint16_t maxPage = kMaxAtlasPage);

/*
 * Builds one canonical atlas file. Baker-only (build time). Not thread-safe.
 * add() validates and stores an icon: tinted icons are Alpha8 masks, untinted ones ARGB32
 * premultiplied and may carry a same-sized `dark` variant. finish() packs every cell
 * (packShelves) and writes the file; the result is identical for the same added content in any
 * order.
 */
class AtlasWriter {
public:
    AtlasWriter(std::uint16_t scalePercent, std::uint64_t sourceHash, std::uint16_t maxPage = kMaxAtlasPage);

    // Errors: InvalidArgument for an invalid id, a design size outside kIconDesignSizes, a role
    // at or above kCount, a duplicate (id, designSize), a cell that is not
    // iconDevicePx(designSize, scalePercent) square, a pixel buffer whose size does not match
    // its dimensions, or a dark variant on a tinted icon or of another size. O(icons). The cell
    // pixels are copied verbatim by finish().
    [[nodiscard]] Result<void> add(std::string id, std::uint8_t designSize, bool tinted, IconRole role, AtlasCell cell,
                                   std::optional<AtlasCell> dark = std::nullopt);

    // Errors: InvalidArgument (a scale outside kAtlasScales or a page limit outside
    // [kMinAtlasPageLimit, kMaxAtlasPage]), OutOfRange (a cell larger than a page, or a file over
    // 4 GiB). O(pixels + n log n).
    [[nodiscard]] Result<std::vector<std::byte>> finish() const;

private:
    struct Item {
        std::string id;
        std::uint8_t designSize;
        bool tinted;
        IconRole role;
        AtlasCell cell;
        std::optional<AtlasCell> dark;
    };

    std::uint16_t m_scalePercent;
    std::uint64_t m_sourceHash;
    std::uint16_t m_maxPage;
    std::vector<Item> m_items;
};

// One baked cell that could serve a request: its design size, the atlas scale and its size in
// device pixels.
struct IconCandidate {
    std::uint8_t designSize = 0;
    std::uint16_t scalePercent = 0;
    std::uint16_t devicePx = 0;
};

/*
 * Picks the candidate that serves an icon of `wantedDesignSize` drawn at `wantedDevicePx` device
 * pixels. Rules, first match wins:
 *   1. devicePx == wanted and designSize == wantedDesignSize;
 *   2. devicePx == wanted with any design size, the larger design size first;
 *   3. the smallest devicePx above wanted with designSize == wantedDesignSize;
 *   4. the smallest devicePx above wanted with any design size (larger design size first);
 *   5. the largest devicePx available (then upsampled), wanted design size first.
 * Remaining ties go to the lower index. nullopt only for an empty span. O(candidates).
 */
[[nodiscard]] std::optional<std::size_t> selectIconSource(std::span<const IconCandidate> candidates,
                                                          std::uint8_t wantedDesignSize,
                                                          std::uint16_t wantedDevicePx) noexcept;

} // namespace frost::ui
