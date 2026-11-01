// SPDX-License-Identifier: Apache-2.0
//
// The icon atlas fuzz harness (icon_atlas_harness.hpp).

#include "icon_atlas_harness.hpp"

#include "frost/ui/model/icon_atlas_format.hpp"

#include <algorithm>
#include <format>
#include <string>

namespace frost::ui::fuzz {

namespace {

AtlasCell copyCell(const IconAtlasView& view, std::uint16_t page, std::uint16_t x, std::uint16_t y, std::uint16_t w,
                   std::uint16_t h) {
    const AtlasPageView p = view.page(page);
    const std::uint32_t bpp = bytesPerPixel(p.format);
    AtlasCell cell{w, h, {}};
    cell.pixels.reserve(std::size_t{w} * h * bpp);
    for (std::size_t row = 0; row < h; ++row) {
        const auto rowBytes = p.pixels.subspan((y + row) * p.stride + std::size_t{x} * bpp, std::size_t{w} * bpp);
        cell.pixels.insert(cell.pixels.end(), rowBytes.begin(), rowBytes.end());
    }
    return cell;
}

} // namespace

Result<void> runIconAtlasInput(std::span<const std::uint8_t> data) {
    const std::span<const std::byte> bytes = std::as_bytes(data);
    auto view = IconAtlasView::parse(bytes);
    if (!view) {
        const ErrorCode code = view.error().code();
        if (code != ErrorCode::Corrupt && code != ErrorCode::Unsupported) {
            return fail(ErrorCode::Internal, std::format("parse failed with {}", toString(code)));
        }
        return {};
    }
    const auto* begin = bytes.data();
    const auto* end = bytes.data() + bytes.size();
    for (std::size_t p = 0; p < view->pageCount(); ++p) {
        const AtlasPageView page = view->page(p);
        if (page.pixels.data() < begin || page.pixels.data() + page.pixels.size() > end) {
            return fail(ErrorCode::Internal, std::format("page {} lies outside the input", p));
        }
    }
    AtlasWriter writer(view->scalePercent(), view->sourceHash(), view->pageLimit());
    for (std::size_t i = 0; i < view->entryCount(); ++i) {
        const AtlasEntryView e = view->entry(i);
        if (view->find(e.id, e.designSize) != i) {
            return fail(ErrorCode::Internal,
                        std::format("find({}, {}) does not return entry {}", e.id, e.designSize, i));
        }
        std::optional<AtlasCell> dark;
        if (e.hasDark) {
            dark = copyCell(*view, e.darkPage, e.darkX, e.darkY, e.w, e.h);
        }
        auto added = writer.add(std::string(e.id), e.designSize, e.tinted, e.role,
                                copyCell(*view, e.page, e.x, e.y, e.w, e.h), std::move(dark));
        if (!added) {
            return fail(ErrorCode::Internal, "an accepted entry is refused by AtlasWriter: " + added.error().message());
        }
    }
    auto rewritten = writer.finish();
    if (!rewritten) {
        return fail(ErrorCode::Internal, "an accepted atlas cannot be re-written: " + rewritten.error().message());
    }
    if (!std::ranges::equal(*rewritten, bytes)) {
        return fail(ErrorCode::Internal, "an accepted atlas does not re-write to the same bytes");
    }
    return {};
}

} // namespace frost::ui::fuzz
