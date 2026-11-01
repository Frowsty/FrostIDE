// SPDX-License-Identifier: Apache-2.0
//
// FIA v1: the validating atlas view, the shelf packer, the canonical writer and the icon
// source selection rule (frost/ui/model/icon_atlas_format.hpp documents the layout).

#include "frost/ui/model/icon_atlas_format.hpp"

#include "frost/ui/model/fnv1a.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <limits>
#include <numeric>
#include <tuple>

namespace frost::ui {

static_assert(std::endian::native == std::endian::little,
              "the icon atlas is little-endian and read with memcpy; a big-endian port needs byte swaps");

namespace {

constexpr std::string_view kMagic = "FROSTICO";
constexpr std::size_t kChecksumOffset = 48;
constexpr std::uint8_t kFlagTinted = 1;
constexpr std::uint8_t kFlagHasDark = 2;
// Ids longer than this are refused (the id table entries are u16-sized; real ids are short).
constexpr std::size_t kMaxIconIdBytes = 128;

template<class T>
T get(std::span<const std::byte> in, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, in.data() + offset, sizeof(T));
    return value;
}

template<class T>
void put(std::vector<std::byte>& out, std::size_t offset, T value) {
    std::memcpy(out.data() + offset, &value, sizeof(T));
}

constexpr std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

bool allZero(std::span<const std::byte> bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](std::byte b) { return b == std::byte{0}; });
}

Result<IconAtlasView> corrupt(std::string message) {
    return fail(ErrorCode::Corrupt, "icon atlas: " + std::move(message));
}

bool knownScale(std::uint16_t scale) noexcept {
    return std::find(kAtlasScales.begin(), kAtlasScales.end(), scale) != kAtlasScales.end();
}

bool knownDesignSize(std::uint8_t size) noexcept {
    return std::find(kIconDesignSizes.begin(), kIconDesignSizes.end(), size) != kIconDesignSizes.end();
}

// The checksum of the tables: the header with its checksum field zeroed, then [64, tablesEnd).
std::uint64_t tableChecksum(std::span<const std::byte> bytes, std::size_t tablesEnd) noexcept {
    std::array<std::byte, kAtlasHeaderSize> header{};
    std::memcpy(header.data(), bytes.data(), kAtlasHeaderSize);
    std::memset(header.data() + kChecksumOffset, 0, 8);
    return fnv1a64(bytes.subspan(kAtlasHeaderSize, tablesEnd - kAtlasHeaderSize), fnv1a64(header));
}

} // namespace

bool isValidIconId(std::string_view id) noexcept {
    if (id.empty() || id.size() > kMaxIconIdBytes) {
        return false;
    }
    std::size_t segments = 0;
    std::size_t start = 0;
    while (start <= id.size()) {
        const std::size_t end = std::min(id.find('.', start), id.size());
        const std::string_view segment = id.substr(start, end - start);
        if (segment.empty()) {
            return false;
        }
        for (const char c : segment) {
            const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
            if (!alnum && !(c == '-' && segments > 0)) {
                return false;
            }
        }
        ++segments;
        start = end + 1;
    }
    return segments >= 2;
}

Result<IconAtlasView> IconAtlasView::parse(std::span<const std::byte> bytes) {
    const std::uint64_t size = bytes.size();
    if (size < kAtlasHeaderSize) {
        return corrupt(std::format("{} bytes is shorter than the {}-byte header", size, kAtlasHeaderSize));
    }
    if (std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) != 0) {
        return corrupt("bad magic");
    }
    if (get<std::uint32_t>(bytes, 36) != size) {
        return corrupt("totalSize differs from the file size");
    }
    // Table extents, in 64-bit arithmetic: the canonical layout fixes every offset.
    const std::uint64_t entryCount = get<std::uint32_t>(bytes, 12);
    const std::uint64_t entryOffset = get<std::uint32_t>(bytes, 16);
    const std::uint64_t idTableOffset = get<std::uint32_t>(bytes, 20);
    const std::uint64_t idTableSize = get<std::uint32_t>(bytes, 24);
    const std::uint64_t pageCount = get<std::uint16_t>(bytes, 28);
    const std::uint64_t pageTableOffset = get<std::uint32_t>(bytes, 32);
    if (entryOffset != kAtlasHeaderSize || idTableOffset != entryOffset + entryCount * kAtlasEntrySize ||
        pageTableOffset != idTableOffset + idTableSize) {
        return corrupt("tables are not at their canonical offsets");
    }
    const std::uint64_t tablesEnd = pageTableOffset + pageCount * kAtlasPageRecordSize;
    if (tablesEnd > size) {
        return corrupt("tables extend past the end of the file");
    }
    if (get<std::uint64_t>(bytes, kChecksumOffset) != tableChecksum(bytes, static_cast<std::size_t>(tablesEnd))) {
        return corrupt("table checksum mismatch");
    }
    if (const auto version = get<std::uint16_t>(bytes, 8); version != kIconAtlasVersion) {
        return fail(ErrorCode::Unsupported,
                    std::format("icon atlas version {} (expected {})", version, kIconAtlasVersion));
    }
    if (get<std::uint64_t>(bytes, 56) != 0) {
        return corrupt("reserved header fields are not zero");
    }
    const std::uint16_t maxPage = get<std::uint16_t>(bytes, 30);
    if (maxPage < kMinAtlasPageLimit || maxPage > kMaxAtlasPage) {
        return corrupt(std::format("page limit {} is out of range", maxPage));
    }
    IconAtlasView view;
    view.m_bytes = bytes;
    view.m_scalePercent = get<std::uint16_t>(bytes, 10);
    view.m_sourceHash = get<std::uint64_t>(bytes, 40);
    view.m_pageLimit = maxPage;
    view.m_entryCount = static_cast<std::size_t>(entryCount);
    view.m_pageCount = static_cast<std::size_t>(pageCount);
    view.m_idTableOffset = static_cast<std::size_t>(idTableOffset);
    view.m_pageTableOffset = static_cast<std::size_t>(pageTableOffset);
    if (!knownScale(view.m_scalePercent)) {
        return corrupt(std::format("unknown scale {}%", view.m_scalePercent));
    }
    if (entryCount > 0 && pageCount == 0) {
        return corrupt("entries but no pages");
    }

    // Pages: canonical offsets and strides, inside the file, zero padding everywhere.
    std::uint64_t cursor = tablesEnd;
    for (std::size_t i = 0; i < view.m_pageCount; ++i) {
        const std::size_t record = view.m_pageTableOffset + i * kAtlasPageRecordSize;
        const std::uint64_t offset = get<std::uint32_t>(bytes, record);
        const std::uint32_t width = get<std::uint16_t>(bytes, record + 4);
        const std::uint32_t height = get<std::uint16_t>(bytes, record + 6);
        const std::uint64_t stride = get<std::uint32_t>(bytes, record + 8);
        const auto format = static_cast<std::uint8_t>(bytes[record + 12]);
        if (!allZero(bytes.subspan(record + 13, 3))) {
            return corrupt(std::format("page {}: reserved bytes are not zero", i));
        }
        if (format != static_cast<std::uint8_t>(PixelFormat::Alpha8) &&
            format != static_cast<std::uint8_t>(PixelFormat::Argb32Premultiplied)) {
            return corrupt(std::format("page {}: unknown pixel format {}", i, format));
        }
        if (width == 0 || height == 0 || width > maxPage || height > maxPage) {
            return corrupt(std::format("page {}: size {}x{} is out of range", i, width, height));
        }
        const std::uint32_t bpp = bytesPerPixel(static_cast<PixelFormat>(format));
        if (stride != alignUp(std::uint64_t{width} * bpp, 4)) {
            return corrupt(std::format("page {}: stride {} is not canonical", i, stride));
        }
        if (offset != alignUp(cursor, kAtlasPageAlignment)) {
            return corrupt(std::format("page {}: offset {} is not canonical", i, offset));
        }
        const std::uint64_t end = offset + stride * height;
        if (end > size) {
            return corrupt(std::format("page {} extends past the end of the file", i));
        }
        if (!allZero(bytes.subspan(static_cast<std::size_t>(cursor), static_cast<std::size_t>(offset - cursor)))) {
            return corrupt(std::format("padding before page {} is not zero", i));
        }
        const std::uint64_t rowBytes = std::uint64_t{width} * bpp;
        if (rowBytes != stride) {
            for (std::uint64_t y = 0; y < height; ++y) {
                const std::uint64_t row = offset + y * stride;
                if (!allZero(bytes.subspan(static_cast<std::size_t>(row + rowBytes),
                                           static_cast<std::size_t>(stride - rowBytes)))) {
                    return corrupt(std::format("page {}: row {} padding is not zero", i, y));
                }
            }
        }
        cursor = end;
    }
    if (cursor != size) {
        return corrupt("trailing bytes after the last page");
    }

    // Entries: canonical id table, strict (id, size) order, cells inside pages of the right format.
    std::uint64_t idCursor = 0;
    std::string_view previousId;
    std::uint8_t previousSize = 0;
    std::uint64_t previousIdOffset = 0;
    for (std::size_t i = 0; i < view.m_entryCount; ++i) {
        const std::size_t e = kAtlasHeaderSize + i * kAtlasEntrySize;
        const std::uint64_t idOffset = get<std::uint32_t>(bytes, e);
        const std::uint64_t idLen = get<std::uint16_t>(bytes, e + 4);
        if (idLen == 0 || idOffset + idLen > idTableSize) {
            return corrupt(std::format("entry {}: id outside the id table", i));
        }
        const std::string_view id(reinterpret_cast<const char*>(bytes.data() + idTableOffset + idOffset),
                                  static_cast<std::size_t>(idLen));
        const auto designSize = static_cast<std::uint8_t>(bytes[e + 6]);
        const auto flags = static_cast<std::uint8_t>(bytes[e + 7]);
        const auto role = static_cast<std::uint8_t>(bytes[e + 8]);
        if (i > 0 && id == previousId) {
            if (idOffset != previousIdOffset || designSize <= previousSize) {
                return corrupt(std::format("entry {}: duplicate or unsorted ({}, {})", i, id, designSize));
            }
        } else {
            if (idOffset != idCursor) {
                return corrupt(std::format("entry {}: id table is not canonical", i));
            }
            idCursor += idLen;
            if (!isValidIconId(id)) {
                return corrupt(std::format("entry {}: invalid id", i));
            }
            if (i > 0 && !(previousId < id)) {
                return corrupt(std::format("entry {}: ids are not ascending", i));
            }
        }
        previousId = id;
        previousSize = designSize;
        previousIdOffset = idOffset;
        if (!knownDesignSize(designSize)) {
            return corrupt(std::format("entry {}: design size {}", i, designSize));
        }
        if ((flags & ~(kFlagTinted | kFlagHasDark)) != 0 || bytes[e + 9] != std::byte{0} ||
            !allZero(bytes.subspan(e + 26, 6))) {
            return corrupt(std::format("entry {}: reserved bits are not zero", i));
        }
        if (role >= kIconRoleCount) {
            return corrupt(std::format("entry {}: role {}", i, role));
        }
        const bool tinted = (flags & kFlagTinted) != 0;
        const bool hasDark = (flags & kFlagHasDark) != 0;
        const std::uint32_t page = get<std::uint16_t>(bytes, e + 10);
        const std::uint32_t x = get<std::uint16_t>(bytes, e + 12);
        const std::uint32_t y = get<std::uint16_t>(bytes, e + 14);
        const std::uint32_t w = get<std::uint16_t>(bytes, e + 16);
        const std::uint32_t h = get<std::uint16_t>(bytes, e + 18);
        const std::uint32_t darkPage = get<std::uint16_t>(bytes, e + 20);
        const std::uint32_t darkX = get<std::uint16_t>(bytes, e + 22);
        const std::uint32_t darkY = get<std::uint16_t>(bytes, e + 24);
        const PixelFormat wanted = tinted ? PixelFormat::Alpha8 : PixelFormat::Argb32Premultiplied;
        const auto cellFits = [&](std::uint32_t p, std::uint32_t cx, std::uint32_t cy) {
            if (p >= view.m_pageCount) {
                return false;
            }
            const AtlasPageView pv = view.page(p);
            return pv.format == wanted && cx + w <= pv.width && cy + h <= pv.height;
        };
        const std::uint16_t devicePx = iconDevicePx(designSize, view.m_scalePercent);
        if (w != devicePx || h != devicePx) {
            return corrupt(std::format("entry {}: a {}-px design at {}% must be {} px square", i, designSize,
                                       view.m_scalePercent, devicePx));
        }
        if (!cellFits(page, x, y)) {
            return corrupt(std::format("entry {}: cell is outside its page or on a page of the wrong format", i));
        }
        if (hasDark) {
            if (tinted || !cellFits(darkPage, darkX, darkY)) {
                return corrupt(std::format("entry {}: invalid dark cell", i));
            }
        } else if (darkPage != 0 || darkX != 0 || darkY != 0) {
            return corrupt(std::format("entry {}: dark fields set without a dark cell", i));
        }
    }
    if (idCursor != idTableSize) {
        return corrupt("id table has unreferenced bytes");
    }
    if (auto canonical = checkCanonicalLayout(view, maxPage); !canonical) {
        return std::unexpected(std::move(canonical.error()));
    }
    return view;
}

/*
 * The parts of canonicity the field checks cannot see: every cell where packShelves (with the
 * file's page limit) puts it, every page exactly as large and of the class the packer makes,
 * and every pixel outside the cells zero. O(n log n + page pixels); the page pixels are at most
 * the file size.
 */
Result<void> IconAtlasView::checkCanonicalLayout(const IconAtlasView& view, std::uint16_t maxPage) {
    std::vector<CellRequest> requests;
    std::vector<AtlasEntryView> entries;
    entries.reserve(view.m_entryCount);
    for (std::size_t i = 0; i < view.m_entryCount; ++i) {
        const AtlasEntryView e = view.entry(i);
        entries.push_back(e);
        requests.push_back({e.w, e.h, static_cast<std::uint8_t>(e.tinted ? 0 : 1), e.id, e.designSize, 0});
        if (e.hasDark) {
            requests.push_back({e.w, e.h, 1, e.id, e.designSize, 1});
        }
    }
    auto layout = packShelves(requests, maxPage);
    if (!layout || layout->pages.size() != view.m_pageCount) {
        return fail(ErrorCode::Corrupt, "icon atlas: the page layout is not the canonical packing");
    }
    for (std::size_t p = 0; p < view.m_pageCount; ++p) {
        const AtlasPageView page = view.page(p);
        const PackedPage& packed = layout->pages[p];
        const PixelFormat format = packed.pageClass == 0 ? PixelFormat::Alpha8 : PixelFormat::Argb32Premultiplied;
        if (page.width != packed.width || page.height != packed.height || page.format != format) {
            return fail(ErrorCode::Corrupt, std::format("icon atlas: page {} is not the canonical packing", p));
        }
    }
    // Coverage masks: 1 where a cell lies. A page's mask has one byte per pixel.
    std::vector<std::vector<std::uint8_t>> covered(view.m_pageCount);
    for (std::size_t p = 0; p < view.m_pageCount; ++p) {
        const AtlasPageView page = view.page(p);
        covered[p].assign(std::size_t{page.width} * page.height, 0);
    }
    const auto cover = [&](std::size_t p, std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
        const std::uint32_t width = view.page(p).width;
        for (std::uint32_t row = y; row < y + h; ++row) {
            std::fill_n(covered[p].begin() + static_cast<std::ptrdiff_t>(std::size_t{row} * width + x), w,
                        std::uint8_t{1});
        }
    };
    std::size_t r = 0;
    for (const AtlasEntryView& e : entries) {
        const PackedCell& at = layout->cells[r++];
        if (at.page != e.page || at.x != e.x || at.y != e.y) {
            return fail(ErrorCode::Corrupt,
                        std::format("icon atlas: cell {}@{} is not where the packer puts it", e.id, e.designSize));
        }
        cover(e.page, e.x, e.y, e.w, e.h);
        if (e.hasDark) {
            const PackedCell& dark = layout->cells[r++];
            if (dark.page != e.darkPage || dark.x != e.darkX || dark.y != e.darkY) {
                return fail(ErrorCode::Corrupt, std::format("icon atlas: dark cell {}@{} is not where the packer "
                                                            "puts it",
                                                            e.id, e.designSize));
            }
            cover(e.darkPage, e.darkX, e.darkY, e.w, e.h);
        }
    }
    for (std::size_t p = 0; p < view.m_pageCount; ++p) {
        const AtlasPageView page = view.page(p);
        const std::uint32_t bpp = bytesPerPixel(page.format);
        for (std::size_t y = 0; y < page.height; ++y) {
            for (std::size_t x = 0; x < page.width; ++x) {
                if (covered[p][y * page.width + x] != 0) {
                    continue;
                }
                const std::size_t at = y * page.stride + x * bpp;
                if (!allZero(page.pixels.subspan(at, bpp))) {
                    return fail(ErrorCode::Corrupt,
                                std::format("icon atlas: page {} has a non-zero pixel outside every cell", p));
                }
            }
        }
    }
    return {};
}

AtlasEntryView IconAtlasView::entry(std::size_t index) const noexcept {
    const std::size_t e = kAtlasHeaderSize + index * kAtlasEntrySize;
    const std::uint32_t idOffset = get<std::uint32_t>(m_bytes, e);
    const std::uint16_t idLen = get<std::uint16_t>(m_bytes, e + 4);
    const auto flags = static_cast<std::uint8_t>(m_bytes[e + 7]);
    AtlasEntryView out;
    out.id = std::string_view(reinterpret_cast<const char*>(m_bytes.data() + m_idTableOffset + idOffset), idLen);
    out.designSize = static_cast<std::uint8_t>(m_bytes[e + 6]);
    out.tinted = (flags & kFlagTinted) != 0;
    out.hasDark = (flags & kFlagHasDark) != 0;
    out.role = static_cast<IconRole>(m_bytes[e + 8]);
    out.page = get<std::uint16_t>(m_bytes, e + 10);
    out.x = get<std::uint16_t>(m_bytes, e + 12);
    out.y = get<std::uint16_t>(m_bytes, e + 14);
    out.w = get<std::uint16_t>(m_bytes, e + 16);
    out.h = get<std::uint16_t>(m_bytes, e + 18);
    out.darkPage = get<std::uint16_t>(m_bytes, e + 20);
    out.darkX = get<std::uint16_t>(m_bytes, e + 22);
    out.darkY = get<std::uint16_t>(m_bytes, e + 24);
    return out;
}

AtlasPageView IconAtlasView::page(std::size_t index) const noexcept {
    const std::size_t record = m_pageTableOffset + index * kAtlasPageRecordSize;
    AtlasPageView out;
    const std::uint32_t offset = get<std::uint32_t>(m_bytes, record);
    out.width = get<std::uint16_t>(m_bytes, record + 4);
    out.height = get<std::uint16_t>(m_bytes, record + 6);
    out.stride = get<std::uint32_t>(m_bytes, record + 8);
    out.format = static_cast<PixelFormat>(m_bytes[record + 12]);
    out.pixels = m_bytes.subspan(offset, std::size_t{out.stride} * out.height);
    return out;
}

std::size_t IconAtlasView::lowerBound(std::string_view id, std::uint8_t designSize) const noexcept {
    std::size_t low = 0;
    std::size_t high = m_entryCount;
    while (low < high) {
        const std::size_t mid = low + (high - low) / 2;
        const AtlasEntryView e = entry(mid);
        if (std::tie(e.id, e.designSize) < std::tie(id, designSize)) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}

std::optional<std::uint32_t> IconAtlasView::find(std::string_view id, std::uint8_t designSize) const noexcept {
    const std::size_t index = lowerBound(id, designSize);
    if (index < m_entryCount) {
        const AtlasEntryView e = entry(index);
        if (e.id == id && e.designSize == designSize) {
            return static_cast<std::uint32_t>(index);
        }
    }
    return std::nullopt;
}

std::vector<std::uint8_t> IconAtlasView::designSizesOf(std::string_view id) const {
    std::vector<std::uint8_t> sizes;
    for (std::size_t i = lowerBound(id, 0); i < m_entryCount; ++i) {
        const AtlasEntryView e = entry(i);
        if (e.id != id) {
            break;
        }
        sizes.push_back(e.designSize);
    }
    return sizes;
}

Result<PackedLayout> packShelves(std::span<const CellRequest> requests, std::uint16_t maxPage) {
    std::vector<std::size_t> order(requests.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const CellRequest& x = requests[a];
        const CellRequest& y = requests[b];
        return std::tuple(x.pageClass, -int{x.height}, x.id, x.designSize, x.variant, a) <
               std::tuple(y.pageClass, -int{y.height}, y.id, y.designSize, y.variant, b);
    });
    PackedLayout layout;
    layout.cells.resize(requests.size());
    std::uint32_t x = 1;
    std::uint32_t y = 1;
    std::uint32_t shelf = 0;
    for (const std::size_t index : order) {
        const CellRequest& r = requests[index];
        if (r.width == 0 || r.height == 0 || std::uint32_t{r.width} + 2 > maxPage ||
            std::uint32_t{r.height} + 2 > maxPage) {
            return fail(ErrorCode::OutOfRange,
                        std::format("cell {} ({}x{}) does not fit a {}-pixel page", r.id, r.width, r.height, maxPage));
        }
        const bool newClass = layout.pages.empty() || layout.pages.back().pageClass != r.pageClass;
        if (!newClass && x + r.width + 1 > maxPage) {
            y += shelf;
            x = 1;
            shelf = 0;
        }
        if (newClass || y + r.height + 1 > maxPage) {
            if (layout.pages.size() == kMaxAtlasPageCount) {
                // The header's page count and every entry's page index are 16-bit fields.
                return fail(ErrorCode::OutOfRange, std::format("cell {} needs page {}, beyond the {}-page atlas limit",
                                                               r.id, layout.pages.size() + 1, kMaxAtlasPageCount));
            }
            layout.pages.push_back({0, 0, r.pageClass});
            x = 1;
            y = 1;
            shelf = 0;
        }
        PackedPage& page = layout.pages.back();
        layout.cells[index] = {static_cast<std::uint16_t>(layout.pages.size() - 1), static_cast<std::uint16_t>(x),
                               static_cast<std::uint16_t>(y)};
        x += r.width + 1U;
        shelf = std::max(shelf, std::uint32_t{r.height} + 1);
        page.width = static_cast<std::uint16_t>(std::max<std::uint32_t>(page.width, x));
        page.height = static_cast<std::uint16_t>(std::max<std::uint32_t>(page.height, y + r.height + 1));
    }
    return layout;
}

AtlasWriter::AtlasWriter(std::uint16_t scalePercent, std::uint64_t sourceHash, std::uint16_t maxPage)
    : m_scalePercent(scalePercent), m_sourceHash(sourceHash), m_maxPage(maxPage) {}

Result<void> AtlasWriter::add(std::string id, std::uint8_t designSize, bool tinted, IconRole role, AtlasCell cell,
                              std::optional<AtlasCell> dark) {
    if (!isValidIconId(id)) {
        return fail(ErrorCode::InvalidArgument, std::format("invalid icon id '{}'", id));
    }
    if (!knownDesignSize(designSize)) {
        return fail(ErrorCode::InvalidArgument, std::format("{}: design size {} is not 14, 16 or 20", id, designSize));
    }
    if (static_cast<std::size_t>(role) >= kIconRoleCount) {
        return fail(ErrorCode::InvalidArgument, std::format("{}: invalid role", id));
    }
    const std::uint32_t bpp = tinted ? 1U : 4U;
    const auto sizeOk = [bpp](const AtlasCell& c) {
        return c.width > 0 && c.height > 0 && c.pixels.size() == std::size_t{c.width} * c.height * bpp;
    };
    const std::uint16_t devicePx = iconDevicePx(designSize, m_scalePercent);
    if (cell.width != devicePx || cell.height != devicePx) {
        return fail(ErrorCode::InvalidArgument, std::format("{}@{}: the cell must be {} px square at {}%", id,
                                                            designSize, devicePx, m_scalePercent));
    }
    if (!sizeOk(cell)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{}@{}: pixel buffer does not match its size", id, designSize));
    }
    if (dark && (tinted || !sizeOk(*dark) || dark->width != cell.width || dark->height != cell.height)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("{}@{}: a dark variant needs an untinted icon of the same size", id, designSize));
    }
    for (const Item& item : m_items) {
        if (item.id == id && item.designSize == designSize) {
            return fail(ErrorCode::InvalidArgument, std::format("{}@{} added twice", id, designSize));
        }
    }
    m_items.push_back({std::move(id), designSize, tinted, role, std::move(cell), std::move(dark)});
    return {};
}

Result<std::vector<std::byte>> AtlasWriter::finish() const {
    if (!knownScale(m_scalePercent) || m_maxPage < kMinAtlasPageLimit || m_maxPage > kMaxAtlasPage) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("icon atlas: scale {}% or page limit {} is not supported", m_scalePercent, m_maxPage));
    }
    std::vector<const Item*> items;
    items.reserve(m_items.size());
    for (const Item& item : m_items) {
        items.push_back(&item);
    }
    std::sort(items.begin(), items.end(), [](const Item* a, const Item* b) {
        return std::tie(a->id, a->designSize) < std::tie(b->id, b->designSize);
    });

    // One request per cell; dark variants follow their icon.
    std::vector<CellRequest> requests;
    std::vector<std::pair<std::size_t, bool>> owners; // (item index, is dark)
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& item = *items[i];
        const std::uint8_t pageClass = item.tinted ? 0 : 1;
        requests.push_back({item.cell.width, item.cell.height, pageClass, item.id, item.designSize, 0});
        owners.emplace_back(i, false);
        if (item.dark) {
            requests.push_back({item.dark->width, item.dark->height, 1, item.id, item.designSize, 1});
            owners.emplace_back(i, true);
        }
    }
    auto layout = packShelves(requests, m_maxPage);
    if (!layout) {
        return std::unexpected(std::move(layout.error()));
    }

    // Distinct ids, in order, and each item's offset into the id table.
    std::string idTable;
    std::vector<std::uint32_t> idOffsets(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0 && items[i]->id == items[i - 1]->id) {
            idOffsets[i] = idOffsets[i - 1];
        } else {
            idOffsets[i] = static_cast<std::uint32_t>(idTable.size());
            idTable += items[i]->id;
        }
    }
    const std::uint64_t idTableOffset = kAtlasHeaderSize + items.size() * kAtlasEntrySize;
    const std::uint64_t pageTableOffset = idTableOffset + idTable.size();
    const std::uint64_t tablesEnd = pageTableOffset + layout->pages.size() * kAtlasPageRecordSize;
    std::vector<std::uint64_t> pageOffsets;
    std::vector<std::uint32_t> strides;
    std::uint64_t cursor = tablesEnd;
    for (const PackedPage& page : layout->pages) {
        const std::uint32_t bpp = page.pageClass == 0 ? 1U : 4U;
        const auto stride = static_cast<std::uint32_t>(alignUp(std::uint64_t{page.width} * bpp, 4));
        cursor = alignUp(cursor, kAtlasPageAlignment);
        pageOffsets.push_back(cursor);
        strides.push_back(stride);
        cursor += std::uint64_t{stride} * page.height;
    }
    if (cursor > std::numeric_limits<std::uint32_t>::max()) {
        return fail(ErrorCode::OutOfRange, "icon atlas would exceed 4 GiB");
    }
    std::vector<std::byte> out(static_cast<std::size_t>(cursor), std::byte{0});

    std::memcpy(out.data(), kMagic.data(), kMagic.size());
    put<std::uint16_t>(out, 8, kIconAtlasVersion);
    put<std::uint16_t>(out, 10, m_scalePercent);
    put<std::uint32_t>(out, 12, static_cast<std::uint32_t>(items.size()));
    put<std::uint32_t>(out, 16, static_cast<std::uint32_t>(kAtlasHeaderSize));
    put<std::uint32_t>(out, 20, static_cast<std::uint32_t>(idTableOffset));
    put<std::uint32_t>(out, 24, static_cast<std::uint32_t>(idTable.size()));
    put<std::uint16_t>(out, 28, static_cast<std::uint16_t>(layout->pages.size()));
    put<std::uint16_t>(out, 30, m_maxPage);
    put<std::uint32_t>(out, 32, static_cast<std::uint32_t>(pageTableOffset));
    put<std::uint32_t>(out, 36, static_cast<std::uint32_t>(cursor));
    put<std::uint64_t>(out, 40, m_sourceHash);

    for (std::size_t p = 0; p < layout->pages.size(); ++p) {
        const std::size_t record = static_cast<std::size_t>(pageTableOffset) + p * kAtlasPageRecordSize;
        const PackedPage& page = layout->pages[p];
        put<std::uint32_t>(out, record, static_cast<std::uint32_t>(pageOffsets[p]));
        put<std::uint16_t>(out, record + 4, page.width);
        put<std::uint16_t>(out, record + 6, page.height);
        put<std::uint32_t>(out, record + 8, strides[p]);
        out[record + 12] =
            static_cast<std::byte>(page.pageClass == 0 ? PixelFormat::Alpha8 : PixelFormat::Argb32Premultiplied);
    }
    std::memcpy(out.data() + idTableOffset, idTable.data(), idTable.size());

    const auto blit = [&](const AtlasCell& cell, const PackedCell& at, std::uint32_t bpp) {
        const std::size_t rowBytes = std::size_t{cell.width} * bpp;
        for (std::size_t row = 0; row < cell.height; ++row) {
            const std::uint64_t dst =
                pageOffsets[at.page] + (std::uint64_t{at.y} + row) * strides[at.page] + std::uint64_t{at.x} * bpp;
            std::memcpy(out.data() + dst, cell.pixels.data() + row * rowBytes, rowBytes);
        }
    };
    std::vector<PackedCell> main(items.size());
    std::vector<std::optional<PackedCell>> darkCells(items.size());
    for (std::size_t r = 0; r < requests.size(); ++r) {
        const auto [itemIndex, isDark] = owners[r];
        const Item& item = *items[itemIndex];
        if (isDark) {
            darkCells[itemIndex] = layout->cells[r];
            blit(*item.dark, layout->cells[r], 4U);
        } else {
            main[itemIndex] = layout->cells[r];
            blit(item.cell, layout->cells[r], item.tinted ? 1U : 4U);
        }
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Item& item = *items[i];
        const std::size_t e = kAtlasHeaderSize + i * kAtlasEntrySize;
        put<std::uint32_t>(out, e, idOffsets[i]);
        put<std::uint16_t>(out, e + 4, static_cast<std::uint16_t>(item.id.size()));
        out[e + 6] = static_cast<std::byte>(item.designSize);
        out[e + 7] = static_cast<std::byte>((item.tinted ? kFlagTinted : 0) | (item.dark ? kFlagHasDark : 0));
        out[e + 8] = static_cast<std::byte>(item.role);
        put<std::uint16_t>(out, e + 10, main[i].page);
        put<std::uint16_t>(out, e + 12, main[i].x);
        put<std::uint16_t>(out, e + 14, main[i].y);
        put<std::uint16_t>(out, e + 16, item.cell.width);
        put<std::uint16_t>(out, e + 18, item.cell.height);
        if (darkCells[i]) {
            put<std::uint16_t>(out, e + 20, darkCells[i]->page);
            put<std::uint16_t>(out, e + 22, darkCells[i]->x);
            put<std::uint16_t>(out, e + 24, darkCells[i]->y);
        }
    }
    put<std::uint64_t>(out, kChecksumOffset, tableChecksum(out, static_cast<std::size_t>(tablesEnd)));
    return out;
}

std::optional<std::size_t> selectIconSource(std::span<const IconCandidate> candidates, std::uint8_t wantedDesignSize,
                                            std::uint16_t wantedDevicePx) noexcept {
    std::optional<std::size_t> best;
    const auto at = [&](std::size_t i) -> const IconCandidate& { return candidates[i]; };
    // 1. Exact device size and design size.
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (at(i).devicePx == wantedDevicePx && at(i).designSize == wantedDesignSize) {
            return i;
        }
    }
    // 2. Exact device size, any design size, larger design first.
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (at(i).devicePx == wantedDevicePx && (!best || at(i).designSize > at(*best).designSize)) {
            best = i;
        }
    }
    if (best) {
        return best;
    }
    // 3. The smallest larger device size with the wanted design size.
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (at(i).devicePx > wantedDevicePx && at(i).designSize == wantedDesignSize &&
            (!best || at(i).devicePx < at(*best).devicePx)) {
            best = i;
        }
    }
    if (best) {
        return best;
    }
    // 4. The smallest larger device size with any design size, larger design first.
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (at(i).devicePx > wantedDevicePx &&
            (!best || at(i).devicePx < at(*best).devicePx ||
             (at(i).devicePx == at(*best).devicePx && at(i).designSize > at(*best).designSize))) {
            best = i;
        }
    }
    if (best) {
        return best;
    }
    // 5. The largest available (it will be upsampled), the wanted design size first.
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        if (!best || at(i).devicePx > at(*best).devicePx ||
            (at(i).devicePx == at(*best).devicePx && at(i).designSize == wantedDesignSize &&
             at(*best).designSize != wantedDesignSize)) {
            best = i;
        }
    }
    return best;
}

} // namespace frost::ui
