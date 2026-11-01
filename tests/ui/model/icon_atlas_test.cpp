// SPDX-License-Identifier: Apache-2.0
//
// Tests of the FIA v1 icon atlas format (frost/ui/model/icon_atlas_format.hpp): writer/parser
// round trips, binary search against a linear scan, corruption sweeps over the header and
// tables, the shelf packer's properties and the icon source selection rule against a brute-force
// oracle.

#include "frost/ui/model/fnv1a.hpp"
#include "frost/ui/model/icon_atlas_format.hpp"

#include "model_test_support.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace frost::ui {
namespace {

using test::SplitMix64;

struct TestIcon {
    std::string id;
    std::uint8_t designSize;
    bool tinted;
    IconRole role;
    AtlasCell cell;
    std::optional<AtlasCell> dark;
};

AtlasCell randomCell(SplitMix64& rng, std::uint16_t px, bool tinted) {
    AtlasCell cell{px, px, {}};
    cell.pixels.resize(std::size_t{px} * px * (tinted ? 1U : 4U));
    for (std::byte& b : cell.pixels) {
        b = static_cast<std::byte>(rng.next());
    }
    return cell;
}

// A random icon set at `scale`: unique (id, size) pairs, a mix of tinted and untinted icons, some
// with dark variants.
std::vector<TestIcon> randomIcons(SplitMix64& rng, std::uint16_t scale, std::size_t ids) {
    static constexpr std::string_view kGroups[] = {"actions", "nav", "status", "nodes", "run"};
    std::vector<TestIcon> icons;
    std::set<std::string> used;
    while (used.size() < ids) {
        std::string id = std::string(kGroups[rng.below(5)]) + "." + std::to_string(rng.below(1000));
        if (rng.coin()) {
            id += "-x";
        }
        if (!used.insert(id).second) {
            continue;
        }
        const bool tinted = rng.below(4) != 0;
        for (const std::uint8_t size : kIconDesignSizes) {
            if (rng.below(3) == 0) {
                continue;
            }
            const std::uint16_t px = iconDevicePx(size, scale);
            TestIcon icon{id,
                          size,
                          tinted,
                          static_cast<IconRole>(rng.below(kIconRoleCount)),
                          randomCell(rng, px, tinted),
                          std::nullopt};
            if (!tinted && rng.coin()) {
                icon.dark = randomCell(rng, px, false);
            }
            icons.push_back(std::move(icon));
        }
    }
    return icons;
}

std::vector<std::byte> writeAtlas(const std::vector<TestIcon>& icons, std::uint16_t scale, std::uint64_t hash = 42,
                                  std::uint16_t maxPage = kMaxAtlasPage) {
    AtlasWriter writer(scale, hash, maxPage);
    for (const TestIcon& icon : icons) {
        auto added = writer.add(icon.id, icon.designSize, icon.tinted, icon.role, icon.cell, icon.dark);
        EXPECT_TRUE(added) << added.error().describe();
    }
    auto bytes = writer.finish();
    EXPECT_TRUE(bytes) << bytes.error().describe();
    return bytes ? *bytes : std::vector<std::byte>{};
}

// The cell `pixels` of `entry` copied out of its page (tightly packed rows).
std::vector<std::byte> cellPixels(const IconAtlasView& view, std::uint16_t page, std::uint16_t x, std::uint16_t y,
                                  std::uint16_t w, std::uint16_t h) {
    const AtlasPageView p = view.page(page);
    const std::uint32_t bpp = bytesPerPixel(p.format);
    std::vector<std::byte> out;
    for (std::size_t row = 0; row < h; ++row) {
        const auto begin = p.pixels.begin() + static_cast<std::ptrdiff_t>((y + row) * p.stride + std::size_t{x} * bpp);
        out.insert(out.end(), begin, begin + static_cast<std::ptrdiff_t>(std::size_t{w} * bpp));
    }
    return out;
}

template<class T>
T peek(const std::vector<std::byte>& bytes, std::size_t offset) {
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

template<class T>
void poke(std::vector<std::byte>& bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

// Recomputes the table checksum after a deliberate change, so parse reaches the field checks.
void reseal(std::vector<std::byte>& bytes) {
    const std::uint64_t tablesEnd =
        std::uint64_t{peek<std::uint32_t>(bytes, 32)} + std::uint64_t{peek<std::uint16_t>(bytes, 28)} * 16;
    if (tablesEnd > bytes.size()) {
        return;
    }
    std::vector<std::byte> header(bytes.begin(), bytes.begin() + 64);
    std::fill(header.begin() + 48, header.begin() + 56, std::byte{0});
    const std::uint64_t sum = fnv1a64(std::span<const std::byte>(bytes).subspan(64, tablesEnd - 64), fnv1a64(header));
    poke(bytes, 48, sum);
}

TEST(IconAtlas, RoundTrip) {
    SplitMix64 rng(1);
    for (const std::uint16_t scale : kAtlasScales) {
        std::vector<TestIcon> icons = randomIcons(rng, scale, 30);
        const std::vector<std::byte> bytes = writeAtlas(icons, scale, 0x1234'5678'9abc'def0ULL);
        const auto view = IconAtlasView::parse(bytes);
        ASSERT_TRUE(view) << view.error().describe();
        EXPECT_EQ(view->scalePercent(), scale);
        EXPECT_EQ(view->sourceHash(), 0x1234'5678'9abc'def0ULL);
        ASSERT_EQ(view->entryCount(), icons.size());
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(view->bytes().data()),
                  reinterpret_cast<std::uintptr_t>(bytes.data()));
        for (const TestIcon& icon : icons) {
            const auto index = view->find(icon.id, icon.designSize);
            ASSERT_TRUE(index) << icon.id;
            const AtlasEntryView e = view->entry(*index);
            EXPECT_EQ(e.id, icon.id);
            EXPECT_EQ(e.tinted, icon.tinted);
            EXPECT_EQ(e.role, icon.role);
            EXPECT_EQ(e.hasDark, icon.dark.has_value());
            EXPECT_EQ(e.w, icon.cell.width);
            EXPECT_EQ(view->page(e.page).format, icon.tinted ? PixelFormat::Alpha8 : PixelFormat::Argb32Premultiplied);
            EXPECT_EQ(cellPixels(*view, e.page, e.x, e.y, e.w, e.h), icon.cell.pixels);
            if (icon.dark) {
                EXPECT_EQ(cellPixels(*view, e.darkPage, e.darkX, e.darkY, e.w, e.h), icon.dark->pixels);
            }
            for (std::size_t p = 0; p < view->pageCount(); ++p) {
                EXPECT_EQ(reinterpret_cast<std::uintptr_t>(view->page(p).pixels.data()) % kAtlasPageAlignment,
                          reinterpret_cast<std::uintptr_t>(bytes.data()) % kAtlasPageAlignment)
                    << "pages are 64-byte aligned within the file";
            }
        }
        // Order-independent: the same icons added in reverse give the same bytes.
        std::reverse(icons.begin(), icons.end());
        EXPECT_EQ(writeAtlas(icons, scale, 0x1234'5678'9abc'def0ULL), bytes);
    }
}

TEST(IconAtlas, EmptyAtlasAndDesignSizes) {
    const std::vector<std::byte> empty = writeAtlas({}, 100);
    const auto view = IconAtlasView::parse(empty);
    ASSERT_TRUE(view) << view.error().describe();
    EXPECT_EQ(view->entryCount(), 0U);
    EXPECT_EQ(view->pageCount(), 0U);
    EXPECT_FALSE(view->find("actions.search", 16));

    SplitMix64 rng(2);
    std::vector<TestIcon> icons;
    for (const std::uint8_t size : {std::uint8_t{20}, std::uint8_t{16}}) {
        icons.push_back({"actions.search", size, true, IconRole::Default,
                         randomCell(rng, iconDevicePx(size, 150), true), std::nullopt});
    }
    const std::vector<std::byte> bytes = writeAtlas(icons, 150);
    const auto two = IconAtlasView::parse(bytes);
    ASSERT_TRUE(two);
    EXPECT_EQ(two->designSizesOf("actions.search"), (std::vector<std::uint8_t>{16, 20}));
    EXPECT_TRUE(two->designSizesOf("actions.searc").empty());
    EXPECT_TRUE(two->designSizesOf("actions.search2").empty());
    EXPECT_FALSE(two->find("actions.search", 14));
}

TEST(IconAtlas, DevicePxRounding) {
    EXPECT_EQ(iconDevicePx(14, 125), 18) << "17.5 rounds up";
    EXPECT_EQ(iconDevicePx(14, 175), 25) << "24.5 rounds up";
    EXPECT_EQ(iconDevicePx(16, 150), 24);
    EXPECT_EQ(iconDevicePx(20, 300), 60);
    EXPECT_EQ(iconDevicePx(16, 100), 16);
}

TEST(IconAtlas, FindMatchesLinearScan) {
    SplitMix64 rng(3);
    for (int round = 0; round < 500; ++round) {
        const std::uint16_t scale = kAtlasScales[rng.below(kAtlasScales.size())];
        const std::vector<TestIcon> icons = randomIcons(rng, scale, 1 + rng.below(12));
        const std::vector<std::byte> bytes = writeAtlas(icons, scale);
        const auto view = IconAtlasView::parse(bytes);
        ASSERT_TRUE(view) << view.error().describe();
        std::vector<std::string> probes;
        for (const TestIcon& icon : icons) {
            probes.push_back(icon.id);
            probes.push_back(icon.id + "a");
            probes.push_back(icon.id.substr(0, icon.id.size() - 1));
        }
        probes.emplace_back("aaa.0");
        probes.emplace_back("zzz.9");
        for (const std::string& id : probes) {
            for (const std::uint8_t size : {std::uint8_t{14}, std::uint8_t{16}, std::uint8_t{20}, std::uint8_t{0}}) {
                std::optional<std::uint32_t> linear;
                for (std::size_t i = 0; i < view->entryCount(); ++i) {
                    if (view->entry(i).id == id && view->entry(i).designSize == size) {
                        linear = static_cast<std::uint32_t>(i);
                    }
                }
                ASSERT_EQ(view->find(id, size), linear) << id << "@" << int{size};
            }
        }
    }
}

TEST(IconAtlas, TruncationSweep) {
    SplitMix64 rng(4);
    const std::vector<std::byte> bytes = writeAtlas(randomIcons(rng, 125, 6), 125);
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        const auto view = IconAtlasView::parse(std::span(bytes).first(n));
        ASSERT_FALSE(view) << n;
        ASSERT_EQ(view.error().code(), ErrorCode::Corrupt) << n;
    }
}

TEST(IconAtlas, EveryBitFlipInTablesRejected) {
    SplitMix64 rng(5);
    const std::vector<std::byte> bytes = writeAtlas(randomIcons(rng, 200, 8), 200);
    const std::size_t tablesEnd = peek<std::uint32_t>(bytes, 32) + std::size_t{peek<std::uint16_t>(bytes, 28)} * 16;
    // Header and tables only: pixels inside cells are not covered by design.
    for (std::size_t bit = 0; bit < tablesEnd * 8; ++bit) {
        std::vector<std::byte> flipped = bytes;
        flipped[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
        const auto view = IconAtlasView::parse(flipped);
        ASSERT_FALSE(view) << "bit " << bit;
        ASSERT_TRUE(view.error().code() == ErrorCode::Corrupt || view.error().code() == ErrorCode::Unsupported)
            << "bit " << bit;
    }
}

TEST(IconAtlas, FieldCorruptions) {
    SplitMix64 rng(6);
    std::vector<TestIcon> icons;
    icons.push_back({"actions.add", 16, true, IconRole::Default, randomCell(rng, 20, true), std::nullopt});
    icons.push_back({"actions.search", 16, true, IconRole::Accent, randomCell(rng, 20, true), std::nullopt});
    icons.push_back(
        {"nodes.file", 16, false, IconRole::Default, randomCell(rng, 20, false), randomCell(rng, 20, false)});
    const std::vector<std::byte> good = writeAtlas(icons, 125);
    ASSERT_TRUE(IconAtlasView::parse(good));
    // Entry i starts at 64 + 32 i; entries are sorted: actions.add, actions.search, nodes.file.
    constexpr std::size_t kE0 = 64, kE1 = 96, kE2 = 128;
    const std::size_t pageTable = peek<std::uint32_t>(good, 32);
    const auto expect = [&](auto mutate, ErrorCode code, std::string_view what) {
        std::vector<std::byte> bytes = good;
        mutate(bytes);
        reseal(bytes);
        const auto view = IconAtlasView::parse(bytes);
        ASSERT_FALSE(view) << what;
        EXPECT_EQ(view.error().code(), code) << what << ": " << view.error().describe();
    };
    using V = std::vector<std::byte>;
    expect([](V& b) { poke<std::uint16_t>(b, 8, 2); }, ErrorCode::Unsupported, "version");
    expect([](V& b) { poke<std::uint16_t>(b, 10, 110); }, ErrorCode::Corrupt, "unbaked scale");
    expect([](V& b) { poke<std::uint64_t>(b, 56, 1); }, ErrorCode::Corrupt, "reserved");
    expect([](V& b) { poke<std::uint16_t>(b, 30, 8); }, ErrorCode::Corrupt, "page limit");
    expect([](V& b) { poke<std::uint32_t>(b, 16, 96); }, ErrorCode::Corrupt, "entry offset");
    expect([](V& b) { poke<std::uint32_t>(b, 20, peek<std::uint32_t>(b, 20) - 1); }, ErrorCode::Corrupt,
           "overlapping tables");
    expect([](V& b) { poke<std::uint32_t>(b, 12, 0xffff'fff0U); }, ErrorCode::Corrupt, "entry count near UINT32_MAX");
    expect([](V& b) { poke<std::uint32_t>(b, 24, 0xffff'ffffU); }, ErrorCode::Corrupt, "id table size overflow");
    expect([](V& b) { poke<std::uint32_t>(b, 32, 0xffff'fff0U); }, ErrorCode::Corrupt, "page table near UINT32_MAX");
    expect([](V& b) { poke<std::uint16_t>(b, 28, 0xffff); }, ErrorCode::Corrupt, "page count");
    expect([&](V& b) { poke<std::uint32_t>(b, pageTable, 0xffff'ffc0U); }, ErrorCode::Corrupt, "page offset");
    expect([&](V& b) { poke<std::uint32_t>(b, pageTable + 8, peek<std::uint32_t>(b, pageTable + 8) + 4); },
           ErrorCode::Corrupt, "non-canonical stride");
    expect([&](V& b) { b[pageTable + 12] = std::byte{9}; }, ErrorCode::Corrupt, "unknown format");
    expect([&](V& b) { b[pageTable + 13] = std::byte{1}; }, ErrorCode::Corrupt, "page reserved bytes");
    expect([&](V& b) { poke<std::uint16_t>(b, pageTable + 4, 2049); }, ErrorCode::Corrupt, "page too wide");
    expect([](V& b) { poke<std::uint16_t>(b, kE0 + 12, 2000); }, ErrorCode::Corrupt, "cell outside its page");
    expect([](V& b) { poke<std::uint16_t>(b, kE0 + 16, 21); }, ErrorCode::Corrupt, "cell size");
    expect([](V& b) { poke<std::uint16_t>(b, kE0 + 12, 2); }, ErrorCode::Corrupt, "cell not where the packer puts it");
    expect([](V& b) { b[kE0 + 6] = std::byte{15}; }, ErrorCode::Corrupt, "design size");
    expect([](V& b) { b[kE0 + 8] = static_cast<std::byte>(kIconRoleCount); }, ErrorCode::Corrupt, "role");
    expect([](V& b) { b[kE0 + 7] = std::byte{4 | 1}; }, ErrorCode::Corrupt, "flag bits");
    expect([](V& b) { b[kE0 + 7] = std::byte{3}; }, ErrorCode::Corrupt, "hasDark on a tinted entry");
    expect([](V& b) { b[kE0 + 9] = std::byte{1}; }, ErrorCode::Corrupt, "entry reserved byte");
    expect([](V& b) { b[kE0 + 31] = std::byte{1}; }, ErrorCode::Corrupt, "entry padding");
    expect([](V& b) { b[kE0 + 7] = std::byte{0}; }, ErrorCode::Corrupt, "tinted flag cleared: wrong page format");
    expect([](V& b) { poke<std::uint16_t>(b, kE2 + 22, 5000); }, ErrorCode::Corrupt, "dark cell out of bounds");
    expect([](V& b) { b[kE2 + 7] = std::byte{0}; }, ErrorCode::Corrupt, "dark fields without hasDark");
    expect([](V& b) { poke<std::uint16_t>(b, kE1 + 4, 0); }, ErrorCode::Corrupt, "empty id");
    expect([](V& b) { poke<std::uint32_t>(b, kE1, 0); }, ErrorCode::Corrupt, "id table not canonical");
    // Swap the first two ids' bytes in the id table: "actions.add" < "actions.search" breaks.
    const std::size_t idTable = peek<std::uint32_t>(good, 20);
    expect([&](V& b) { b[idTable + 8] = std::byte{'z'}; }, ErrorCode::Corrupt, "unsorted ids");
    expect([&](V& b) { b[idTable] = std::byte{'A'}; }, ErrorCode::Corrupt, "bad id character");
    expect([&](V& b) { b[idTable + 7] = std::byte{'_'}; }, ErrorCode::Corrupt, "bad id separator");
    // Padding: between the tables and the first page, at a row end, and a gap pixel between cells.
    const std::uint32_t firstPage = peek<std::uint32_t>(good, pageTable);
    if (firstPage > pageTable + std::size_t{peek<std::uint16_t>(good, 28)} * 16) {
        expect([&](V& b) { b[firstPage - 1] = std::byte{1}; }, ErrorCode::Corrupt, "pre-page padding");
    }
    expect([&](V& b) { b[firstPage] = std::byte{1}; }, ErrorCode::Corrupt, "gap pixel at the page corner");
    expect(
        [&](V& b) {
            b.push_back(std::byte{0});
            poke<std::uint32_t>(b, 36, static_cast<std::uint32_t>(b.size()));
        },
        ErrorCode::Corrupt, "trailing bytes");
    // A non-zero byte at the end of a row of a page whose width is not a multiple of 4.
    for (std::uint16_t p = 0; p < peek<std::uint16_t>(good, 28); ++p) {
        const std::size_t record = pageTable + std::size_t{p} * 16;
        const std::uint16_t width = peek<std::uint16_t>(good, record + 4);
        const std::uint32_t stride = peek<std::uint32_t>(good, record + 8);
        if (static_cast<std::uint8_t>(good[record + 12]) == 1 && stride != width) {
            const std::uint32_t offset = peek<std::uint32_t>(good, record);
            expect([&](V& b) { b[offset + stride - 1] = std::byte{1}; }, ErrorCode::Corrupt, "row padding");
        }
    }
    // A flipped pixel inside a cell is accepted: pixels are not checked by design.
    std::vector<std::byte> pixelFlip = good;
    const std::uint16_t cellX = peek<std::uint16_t>(good, kE0 + 12);
    const std::uint16_t cellY = peek<std::uint16_t>(good, kE0 + 14);
    const std::uint32_t stride0 = peek<std::uint32_t>(good, pageTable + 8);
    pixelFlip[firstPage + std::size_t{cellY} * stride0 + cellX] ^= std::byte{0x80};
    EXPECT_TRUE(IconAtlasView::parse(pixelFlip));
}

TEST(IconAtlas, WriterRejectsInvalidIcons) {
    SplitMix64 rng(7);
    AtlasWriter writer(100, 0);
    const auto cell = [&](std::uint16_t px, bool tinted) { return randomCell(rng, px, tinted); };
    EXPECT_TRUE(writer.add("actions.ok", 16, true, IconRole::Default, cell(16, true)));
    EXPECT_FALSE(writer.add("actions.ok", 16, true, IconRole::Default, cell(16, true))) << "duplicate";
    EXPECT_FALSE(writer.add("Actions.ok", 20, true, IconRole::Default, cell(20, true))) << "upper case";
    EXPECT_FALSE(writer.add("actions", 16, true, IconRole::Default, cell(16, true))) << "one segment";
    EXPECT_FALSE(writer.add("-a.b", 16, true, IconRole::Default, cell(16, true))) << "dash in the first segment";
    EXPECT_FALSE(writer.add("a..b", 16, true, IconRole::Default, cell(16, true))) << "empty segment";
    EXPECT_FALSE(writer.add("a.b", 18, true, IconRole::Default, cell(18, true))) << "design size";
    EXPECT_FALSE(writer.add("a.b", 16, true, IconRole::kCount, cell(16, true))) << "role";
    EXPECT_FALSE(writer.add("a.b", 16, true, IconRole::Default, cell(17, true))) << "cell not devicePx";
    AtlasCell shortCell = cell(16, true);
    shortCell.pixels.pop_back();
    EXPECT_FALSE(writer.add("a.b", 16, true, IconRole::Default, shortCell)) << "pixel count";
    EXPECT_FALSE(writer.add("a.b", 16, true, IconRole::Default, cell(16, true), cell(16, false))) << "tinted dark";
    EXPECT_FALSE(writer.add("a.b", 16, false, IconRole::Default, cell(16, false), cell(16, true))) << "dark format";
    EXPECT_TRUE(writer.add("a.b-c", 16, false, IconRole::Default, cell(16, false), cell(16, false)));
    EXPECT_TRUE(writer.finish());
    EXPECT_FALSE(AtlasWriter(110, 0).finish()) << "unbaked scale";
    EXPECT_FALSE(AtlasWriter(100, 0, 8).finish()) << "page limit below the minimum";
    EXPECT_TRUE(isValidIconId("nav.chevron-right"));
    EXPECT_TRUE(isValidIconId("a1.b2.c-3"));
    EXPECT_FALSE(isValidIconId("nav."));
    EXPECT_FALSE(isValidIconId(".nav"));
    EXPECT_FALSE(isValidIconId(""));
}

TEST(IconAtlas, SmallPageLimitMakesManyPages) {
    SplitMix64 rng(8);
    std::vector<TestIcon> icons = randomIcons(rng, 300, 20);
    const std::vector<std::byte> bytes = writeAtlas(icons, 300, 1, 128);
    const auto view = IconAtlasView::parse(bytes);
    ASSERT_TRUE(view) << view.error().describe();
    EXPECT_GT(view->pageCount(), 2U);
    for (std::size_t p = 0; p < view->pageCount(); ++p) {
        EXPECT_LE(view->page(p).width, 128);
        EXPECT_LE(view->page(p).height, 128);
    }
    // A cell that cannot fit the limit with its padding.
    AtlasWriter writer(300, 0, 16);
    ASSERT_TRUE(writer.add("a.b", 16, true, IconRole::Default, randomCell(rng, 48, true)));
    const auto tooBig = writer.finish();
    ASSERT_FALSE(tooBig);
    EXPECT_EQ(tooBig.error().code(), ErrorCode::OutOfRange);
}

TEST(PackShelves, DeterministicAndNoOverlap) {
    SplitMix64 rng(9);
    for (int round = 0; round < 300; ++round) {
        std::vector<std::string> ids;
        std::vector<CellRequest> requests;
        const std::size_t n = 1 + rng.below(80);
        ids.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            ids.push_back("i." + std::to_string(i));
        }
        const auto maxPage = static_cast<std::uint16_t>(64 + rng.below(200));
        for (std::size_t i = 0; i < n; ++i) {
            const auto edge = static_cast<std::uint16_t>(1 + rng.below(60));
            requests.push_back({edge, static_cast<std::uint16_t>(1 + rng.below(60)),
                                static_cast<std::uint8_t>(rng.below(2)), ids[i], 16, 0});
        }
        const auto layout = packShelves(requests, maxPage);
        ASSERT_TRUE(layout) << layout.error().describe();
        // Deterministic and independent of the request order.
        std::vector<std::size_t> perm(n);
        std::iota(perm.begin(), perm.end(), std::size_t{0});
        std::reverse(perm.begin(), perm.end());
        std::vector<CellRequest> reversed;
        for (const std::size_t i : perm) {
            reversed.push_back(requests[i]);
        }
        const auto again = packShelves(reversed, maxPage);
        ASSERT_TRUE(again);
        ASSERT_EQ(again->pages.size(), layout->pages.size());
        for (std::size_t k = 0; k < n; ++k) {
            const PackedCell& a = layout->cells[perm[k]];
            const PackedCell& b = again->cells[k];
            ASSERT_EQ(std::tie(a.page, a.x, a.y), std::tie(b.page, b.x, b.y));
        }
        // Inside the page with a 1 px margin, on a page of its class, and never overlapping
        // (including the 1 px gap between cells).
        for (std::size_t i = 0; i < n; ++i) {
            const PackedCell& c = layout->cells[i];
            const PackedPage& page = layout->pages[c.page];
            ASSERT_EQ(page.pageClass, requests[i].pageClass);
            ASSERT_GE(c.x, 1);
            ASSERT_GE(c.y, 1);
            ASSERT_LE(c.x + requests[i].width + 1, page.width);
            ASSERT_LE(c.y + requests[i].height + 1, page.height);
            ASSERT_LE(page.width, maxPage);
            ASSERT_LE(page.height, maxPage);
            for (std::size_t j = i + 1; j < n; ++j) {
                const PackedCell& d = layout->cells[j];
                if (d.page != c.page) {
                    continue;
                }
                const bool apart = c.x + requests[i].width + 1 <= d.x || d.x + requests[j].width + 1 <= c.x ||
                                   c.y + requests[i].height + 1 <= d.y || d.y + requests[j].height + 1 <= c.y;
                ASSERT_TRUE(apart) << i << " and " << j;
            }
        }
    }
    const CellRequest tooBig{100, 10, 0, "a.b", 16, 0};
    EXPECT_EQ(packShelves(std::span(&tooBig, 1), 101).error().code(), ErrorCode::OutOfRange);
    EXPECT_TRUE(packShelves(std::span(&tooBig, 1), 102));
}

TEST(IconSelection, Table) {
    // 16 and 20 designs at 100/125/150%: device sizes 16, 20, 20, 25, 24, 30.
    const IconCandidate c[] = {{16, 100, 16}, {20, 100, 20}, {16, 125, 20},
                               {20, 125, 25}, {16, 150, 24}, {20, 150, 30}};
    EXPECT_EQ(selectIconSource(c, 16, 16), 0U) << "rule 1";
    EXPECT_EQ(selectIconSource(c, 16, 20), 2U) << "rule 1 beats the larger design at the same size";
    EXPECT_EQ(selectIconSource(c, 14, 20), 1U) << "rule 2: larger design first";
    EXPECT_EQ(selectIconSource(c, 16, 22), 4U) << "rule 3: smallest larger with the wanted design";
    EXPECT_EQ(selectIconSource(c, 14, 22), 4U) << "rule 4: smallest larger (24) with any design";
    EXPECT_EQ(selectIconSource(c, 20, 26), 5U) << "rule 3";
    EXPECT_EQ(selectIconSource(c, 16, 40), 5U) << "rule 5: the largest";
    const IconCandidate tie[] = {{16, 200, 32}, {20, 150, 30}, {16, 150, 24}, {14, 200, 28}, {20, 160, 32}};
    EXPECT_EQ(selectIconSource(tie, 20, 40), 4U) << "rule 5: equal largest, the wanted design first";
    EXPECT_EQ(selectIconSource(tie, 14, 40), 0U) << "rule 5: equal largest, lower index";
    EXPECT_FALSE(selectIconSource({}, 16, 16));
}

// An independent formulation of the rule: sort every candidate by a key tuple and take the first.
std::size_t bruteForce(std::span<const IconCandidate> c, std::uint8_t design, std::uint16_t px) {
    std::vector<std::size_t> order(c.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    const auto key = [&](std::size_t i) {
        const IconCandidate& x = c[i];
        int rule = 5;
        if (x.devicePx == px && x.designSize == design) {
            rule = 1;
        } else if (x.devicePx == px) {
            rule = 2;
        } else if (x.devicePx > px && x.designSize == design) {
            rule = 3;
        } else if (x.devicePx > px) {
            rule = 4;
        }
        int primary = 0;   // Smaller is better.
        int secondary = 0; // Smaller is better.
        switch (rule) {
        case 2: primary = -x.designSize; break;
        case 3: primary = x.devicePx; break;
        case 4:
            primary = x.devicePx;
            secondary = -x.designSize;
            break;
        case 5:
            primary = -x.devicePx;
            secondary = x.designSize == design ? 0 : 1;
            break;
        default: break;
        }
        return std::tuple(rule, primary, secondary, i);
    };
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return key(a) < key(b); });
    return order.front();
}

TEST(IconSelection, BruteForceOracle) {
    // Consistency: the oracle is a second implementation of the same rule, not an external truth.
    SplitMix64 rng(10);
    for (int round = 0; round < 10'000; ++round) {
        std::vector<IconCandidate> candidates(1 + rng.below(12));
        for (IconCandidate& c : candidates) {
            c.designSize = kIconDesignSizes[rng.below(kIconDesignSizes.size())];
            c.scalePercent = kAtlasScales[rng.below(kAtlasScales.size())];
            c.devicePx = rng.below(4) == 0 ? static_cast<std::uint16_t>(10 + rng.below(50))
                                           : iconDevicePx(c.designSize, c.scalePercent);
        }
        const std::uint8_t design = kIconDesignSizes[rng.below(kIconDesignSizes.size())];
        const auto px = static_cast<std::uint16_t>(10 + rng.below(60));
        ASSERT_EQ(selectIconSource(candidates, design, px), bruteForce(candidates, design, px)) << round;
    }
}

} // namespace
} // namespace frost::ui
