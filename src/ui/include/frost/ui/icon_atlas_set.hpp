// SPDX-License-Identifier: Apache-2.0
//
// IconAtlasSet: the baked icon atlases (icons-<pct>.fia, FIA v1) of one bake, loaded through
// IFileSystem, and the tinted, cached pixmaps FrostIconEngine paints (ADR 0015 decision 7).
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/ui/model/icon_atlas_format.hpp"
#include "frost/ui/model/theme.hpp"

#include <QCache>
#include <QHashFunctions>
#include <QIcon>
#include <QPixmap>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace frost::ui {

// Where an icon is shown, which picks the theme metric that gives its design size.
enum class IconSlot : std::uint8_t {
    Small,   // menus, trees, buttons: Icon.size
    Toolbar, // tool bars and tool-window stripes: Toolbar.iconSize
};

// The metric that sizes `slot`. Pure.
[[nodiscard]] constexpr MetricKey slotMetric(IconSlot slot) noexcept {
    return slot == IconSlot::Toolbar ? MetricKey::ToolbarIconSize : MetricKey::IconSize;
}

// The baked scale to preload for a screen of `devicePixelRatio` at `zoomPercent`: the smallest
// kAtlasScales value at or above the effective scale, or the largest. Pure.
[[nodiscard]] std::uint16_t preloadScaleFor(double devicePixelRatio, std::uint16_t zoomPercent) noexcept;

/*
 * The atlases of one bake, shared through std::shared_ptr<const IconAtlasSet>: every icon engine
 * holds a reference, so the buffers live as long as any QIcon made from them.
 *
 * Threading: GUI thread only (FROST_ASSERT in debug builds). The set is logically const; its
 * lazy atlas loads, pixmap cache and counters are mutable.
 *
 * Lifetime: the IFileSystem passed to open() must outlive the set and every QIcon from it;
 * realFileSystem() is process-wide.
 */
class IconAtlasSet {
public:
    // The pixmap cache's cap, in bytes of pixel data. Budget: B19.
    static constexpr std::size_t kPixmapCacheBytes = 8 * 1024 * 1024;

    /*
     * Opens the atlases in `iconsDir` (absolute UTF-8) and reads icons-<pct>.fia for every scale
     * in `preloadPercents` (each in kAtlasScales; at least one): status() first (a file over
     * kMaxAtlasFileBytes is Corrupt), then readFile into an owned buffer, then
     * IconAtlasView::parse. Other scales load on first use. Every loaded atlas must carry the
     * first one's sourceHash; a mismatch is Corrupt ("atlas set from different bakes").
     * Atlases are read, not mapped: they are small (about 40-350 KB) and IFileSystem keeps the
     * reads fault-injectable and long-path safe (ADR 0015 decision 7).
     * TODO(M1): use the IFileSystem mapping API when m1/large-file lands (PLAN.md 3.4 says
     * "mapped").
     * Errors: InvalidArgument (no or unknown preload scale), NotFound, PermissionDenied, Busy,
     * IoError, Corrupt, each with the path as context. Budget: B1.
     */
    [[nodiscard]] static Result<std::shared_ptr<IconAtlasSet>> open(io::IFileSystem& fs, std::string iconsDir,
                                                                    std::span<const std::uint16_t> preloadPercents);

    IconAtlasSet(const IconAtlasSet&) = delete ("the set owns the atlas buffers its views point into");
    IconAtlasSet& operator=(const IconAtlasSet&) = delete ("the set owns the atlas buffers its views point into");
    ~IconAtlasSet();

    // True when the bake has an icon `id` (in any design size). O(log n).
    [[nodiscard]] bool contains(std::string_view id) const;
    // The design sizes baked for `id`, ascending; empty for an unknown id. O(log n).
    [[nodiscard]] std::vector<std::uint8_t> designSizesOf(std::string_view id) const;
    // The bake's source hash (every atlas carries the same one).
    [[nodiscard]] std::uint64_t sourceHash() const noexcept { return m_sourceHash; }

    /*
     * The pixmap for `id` drawn `logicalPx` logical pixels square at `dpr`:
     *   - devicePx = lround(logicalPx x dpr); the design size is the slot's metric in `theme` at
     *     `density` (16 without a theme);
     *   - the source cell is chosen by selectIconSource over every (baked scale, baked design
     *     size); its atlas loads lazily, and when that load fails failedAtlasOpens() grows and
     *     the best candidate among the loaded atlases is used instead;
     *   - the cell is copied, resampled with Qt::SmoothTransformation unless it is an exact
     *     match, and (tinted icons) filled with the role colour through CompositionMode_
     *     DestinationIn with the mask. Normal and Active use roleOverride or the entry's role,
     *     Disabled uses IconRole::Disabled, Selected uses IconRole::Selected; without a theme the
     *     tint is opaque black. Untinted icons use their dark cell when the theme is dark;
     *   - the result has devicePixelRatio `dpr` and is cached under (scale, entry, devicePx,
     *     mode, tint, dark) with its byte size as cost (kPixmapCacheBytes cap). Budget: B5 for
     *     a cache hit (one hash lookup); Budget: B19 for the cache.
     * An unknown id returns a null QPixmap and increments missingLookups(). GUI thread only.
     */
    [[nodiscard]] QPixmap pixmap(std::string_view id, IconSlot slot, int logicalPx, qreal dpr, QIcon::Mode mode,
                                 const Theme* theme, Density density,
                                 std::optional<IconRole> roleOverride = std::nullopt) const;

    // Diagnostics for tests and the perf probes.
    [[nodiscard]] std::size_t cacheBytes() const noexcept { return static_cast<std::size_t>(m_cache.totalCost()); }
    [[nodiscard]] std::size_t missingLookups() const noexcept { return m_missingLookups; }
    [[nodiscard]] std::size_t failedAtlasOpens() const noexcept { return m_failedAtlasOpens; }
    // True once the atlas for `scalePercent` is loaded.
    [[nodiscard]] bool isLoaded(std::uint16_t scalePercent) const noexcept;
    // Drops every cached pixmap (the atlases stay loaded).
    void clearCache() const { m_cache.clear(); }

private:
    struct Loaded;
    struct CacheKey {
        std::uint16_t scale;
        std::uint32_t entry;
        std::uint16_t devicePx;
        std::uint8_t mode;
        std::uint32_t tint;
        bool dark;
        std::uint64_t dprBits; // the pixmap's devicePixelRatio, so a hit needs no detaching copy
        friend bool operator==(const CacheKey&, const CacheKey&) = default;
        friend std::size_t qHash(const CacheKey& key, std::size_t seed = 0) noexcept {
            return qHashMulti(seed, key.scale, key.entry, key.devicePx, key.mode, key.tint, key.dark, key.dprBits);
        }
    };

    IconAtlasSet(io::IFileSystem& fs, std::string iconsDir);
    // Loads the atlas for kAtlasScales[slot] if needed; nullptr after a failure (cached).
    [[nodiscard]] const IconAtlasView* atlas(std::size_t slot) const;
    [[nodiscard]] Result<void> load(std::size_t slot) const;
    // Any loaded atlas (the first one), for id lookups.
    [[nodiscard]] const IconAtlasView& anyAtlas() const noexcept;

    io::IFileSystem& m_fs;
    std::string m_iconsDir;
    mutable std::uint64_t m_sourceHash = 0; // set by the first load, inside open()
    mutable std::array<std::unique_ptr<Loaded>, kAtlasScales.size()> m_atlases;
    mutable std::array<bool, kAtlasScales.size()> m_failed{};
    mutable QCache<CacheKey, QPixmap> m_cache{static_cast<qsizetype>(kPixmapCacheBytes)};
    mutable std::size_t m_missingLookups = 0;
    mutable std::size_t m_failedAtlasOpens = 0;
};

} // namespace frost::ui
