// SPDX-License-Identifier: Apache-2.0
//
// IconAtlasSet (icon_atlas_set.hpp): lazy atlas loading, source selection, tinting and the
// pixmap cache.

#include "frost/ui/icon_atlas_set.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/path.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QThread>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <format>
#include <utility>
#include <vector>

namespace frost::ui {

struct IconAtlasSet::Loaded {
    std::string bytes;                 // owned; never moved after `view` is parsed over it
    std::optional<IconAtlasView> view; // set once parsed (IconAtlasView has no default state)
};

namespace {

std::optional<std::size_t> scaleSlot(std::uint16_t percent) noexcept {
    const auto it = std::ranges::find(kAtlasScales, percent);
    if (it == kAtlasScales.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - kAtlasScales.begin());
}

std::string atlasFileName(std::uint16_t percent) { return std::format("icons-{}.fia", percent); }

} // namespace

std::uint16_t preloadScaleFor(double devicePixelRatio, std::uint16_t zoomPercent) noexcept {
    const double effective = devicePixelRatio * zoomPercent;
    for (const std::uint16_t scale : kAtlasScales) {
        if (scale + 0.5 >= effective) {
            return scale;
        }
    }
    return kAtlasScales.back();
}

IconAtlasSet::IconAtlasSet(io::IFileSystem& fs, std::string iconsDir) : m_fs(fs), m_iconsDir(std::move(iconsDir)) {}

IconAtlasSet::~IconAtlasSet() = default;

Result<std::shared_ptr<IconAtlasSet>> IconAtlasSet::open(io::IFileSystem& fs, std::string iconsDir,
                                                         std::span<const std::uint16_t> preloadPercents) {
    if (preloadPercents.empty()) {
        return fail(ErrorCode::InvalidArgument, "IconAtlasSet::open needs at least one scale to preload");
    }
    for (const std::uint16_t percent : preloadPercents) {
        if (!scaleSlot(percent)) {
            return fail(ErrorCode::InvalidArgument, std::format("{}% is not a baked icon scale", percent));
        }
    }
    // Not make_shared: the constructor is private.
    std::shared_ptr<IconAtlasSet> set(new IconAtlasSet(fs, std::move(iconsDir)));
    for (const std::uint16_t percent : preloadPercents) {
        auto loaded = set->load(*scaleSlot(percent));
        if (!loaded) {
            return std::unexpected(std::move(loaded).error());
        }
    }
    return set;
}

Result<void> IconAtlasSet::load(std::size_t slot) const {
    if (m_atlases[slot]) {
        return {};
    }
    const std::uint16_t percent = kAtlasScales[slot];
    auto path = io::joinPath(m_iconsDir, atlasFileName(percent));
    if (!path) {
        return std::unexpected(std::move(path).error());
    }
    const std::string context = "while loading icon atlas " + *path;
    auto status = m_fs.status(*path);
    if (!status) {
        return std::unexpected(std::move(status).error().withContext(context));
    }
    if (status->size > kMaxAtlasFileBytes) {
        return fail(ErrorCode::Corrupt, std::format("{} is {} bytes, over the {}-byte atlas limit", *path, status->size,
                                                    kMaxAtlasFileBytes));
    }
    auto bytes = m_fs.readFile(*path);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error().withContext(context));
    }
    auto loaded = std::make_unique<Loaded>();
    loaded->bytes = std::move(*bytes);
    auto view = IconAtlasView::parse(std::as_bytes(std::span(loaded->bytes)));
    if (!view) {
        return std::unexpected(std::move(view).error().withContext(context));
    }
    if (view->scalePercent() != percent) {
        return fail(ErrorCode::Corrupt, std::format("{} holds the {}% atlas", *path, view->scalePercent()));
    }
    const bool first = std::ranges::none_of(m_atlases, [](const auto& atlas) { return atlas != nullptr; });
    if (!first && view->sourceHash() != m_sourceHash) {
        return fail(ErrorCode::Corrupt, std::format("{}: atlas set from different bakes (source hash {:016x}, "
                                                    "expected {:016x})",
                                                    *path, view->sourceHash(), m_sourceHash));
    }
    if (first) {
        // Only open() loads the first atlas, before the set is shared.
        m_sourceHash = view->sourceHash();
    }
    loaded->view.emplace(*view);
    m_atlases[slot] = std::move(loaded);
    return {};
}

const IconAtlasView* IconAtlasSet::atlas(std::size_t slot) const {
    if (m_atlases[slot]) {
        return &*m_atlases[slot]->view;
    }
    if (m_failed[slot]) {
        return nullptr;
    }
    if (!load(slot)) {
        // A failed lazy load is not retried; pixmap() falls back to the loaded atlases.
        m_failed[slot] = true;
        ++m_failedAtlasOpens;
        return nullptr;
    }
    return &*m_atlases[slot]->view;
}

const IconAtlasView& IconAtlasSet::anyAtlas() const noexcept {
    for (const auto& loaded : m_atlases) {
        if (loaded) {
            return *loaded->view;
        }
    }
    // open() loads at least one atlas before the set is shared.
    FROST_ASSERT(false);
    std::abort();
}

bool IconAtlasSet::isLoaded(std::uint16_t scalePercent) const noexcept {
    const auto slot = scaleSlot(scalePercent);
    return slot && m_atlases[*slot] != nullptr;
}

bool IconAtlasSet::contains(std::string_view id) const { return !anyAtlas().designSizesOf(id).empty(); }

std::vector<std::uint8_t> IconAtlasSet::designSizesOf(std::string_view id) const {
    return anyAtlas().designSizesOf(id);
}

QPixmap IconAtlasSet::pixmap(std::string_view id, IconSlot slot, int logicalPx, qreal dpr, QIcon::Mode mode,
                             const Theme* theme, Density density, std::optional<IconRole> roleOverride) const {
    // GUI thread only: the caches are unsynchronised.
    FROST_ASSERT(QCoreApplication::instance() == nullptr || QThread::isMainThread());
    if (logicalPx <= 0 || !(dpr > 0.0)) {
        return {};
    }
    const std::vector<std::uint8_t> sizes = anyAtlas().designSizesOf(id);
    if (sizes.empty()) {
        ++m_missingLookups;
        return {};
    }
    const double designMetric = theme != nullptr ? theme->metric(slotMetric(slot), density) : 16.0;
    const auto wantedDesign = static_cast<std::uint8_t>(std::clamp(std::lround(designMetric), 1L, 255L));
    const auto wantedPx = static_cast<std::uint16_t>(std::clamp(std::lround(logicalPx * dpr), 1L, 4096L));

    // Every (scale, design size) is a candidate: one manifest serves every atlas, so the set is
    // known before the chosen atlas is loaded.
    struct Option {
        std::size_t slot;
        IconCandidate candidate;
    };
    std::vector<Option> options;
    options.reserve(kAtlasScales.size() * sizes.size());
    for (std::size_t s = 0; s < kAtlasScales.size(); ++s) {
        for (const std::uint8_t size : sizes) {
            options.push_back({s, {size, kAtlasScales[s], iconDevicePx(size, kAtlasScales[s])}});
        }
    }
    const auto choose = [&](bool loadedOnly) -> std::optional<Option> {
        std::vector<IconCandidate> candidates;
        std::vector<std::size_t> indices;
        for (std::size_t i = 0; i < options.size(); ++i) {
            if (!loadedOnly || m_atlases[options[i].slot]) {
                candidates.push_back(options[i].candidate);
                indices.push_back(i);
            }
        }
        const auto pick = selectIconSource(candidates, wantedDesign, wantedPx);
        return pick ? std::optional<Option>(options[indices[*pick]]) : std::nullopt;
    };
    std::optional<Option> chosen = choose(false);
    const IconAtlasView* view = chosen ? atlas(chosen->slot) : nullptr;
    if (view == nullptr) {
        chosen = choose(true);
        view = chosen ? atlas(chosen->slot) : nullptr;
    }
    const auto index = view != nullptr ? view->find(id, chosen->candidate.designSize) : std::nullopt;
    if (!index) {
        ++m_missingLookups;
        return {};
    }
    const AtlasEntryView entry = view->entry(*index);

    IconRole role = roleOverride.value_or(entry.role);
    if (mode == QIcon::Disabled) {
        role = IconRole::Disabled;
    } else if (mode == QIcon::Selected) {
        role = IconRole::Selected;
    }
    const Rgba tint = theme != nullptr ? theme->iconColor(role) : Rgba{0xff000000U};
    const bool dark = !entry.tinted && entry.hasDark && theme != nullptr && theme->isDark();
    const CacheKey key{chosen->candidate.scalePercent,
                       *index,
                       wantedPx,
                       static_cast<std::uint8_t>(mode),
                       entry.tinted ? tint.argb : 0U,
                       dark,
                       std::bit_cast<std::uint64_t>(static_cast<double>(dpr))};
    // Budget: B5. A hit is this one lookup.
    if (const QPixmap* hit = m_cache.object(key)) {
        return *hit;
    }

    const AtlasPageView page = view->page(dark ? entry.darkPage : entry.page);
    const QImage pageImage(reinterpret_cast<const uchar*>(page.pixels.data()), page.width, page.height,
                           static_cast<qsizetype>(page.stride),
                           page.format == PixelFormat::Alpha8 ? QImage::Format_Alpha8
                                                              : QImage::Format_ARGB32_Premultiplied);
    QImage cell = pageImage.copy(dark ? entry.darkX : entry.x, dark ? entry.darkY : entry.y, entry.w, entry.h);
    if (cell.width() != wantedPx || cell.height() != wantedPx) {
        cell = cell.scaled(wantedPx, wantedPx, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    QImage image;
    if (entry.tinted) {
        image = QImage(wantedPx, wantedPx, QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor::fromRgba(tint.argb));
        QPainter painter(&image);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.drawImage(0, 0, cell);
    } else {
        image = cell.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    QPixmap result = QPixmap::fromImage(std::move(image));
    result.setDevicePixelRatio(dpr);
    // Budget: B19. The cost is the pixel bytes; QCache evicts least recently used entries.
    m_cache.insert(key, new QPixmap(result), static_cast<qsizetype>(wantedPx) * wantedPx * 4);
    return result;
}

} // namespace frost::ui
