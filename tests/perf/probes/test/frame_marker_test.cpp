// SPDX-License-Identifier: Apache-2.0
//
// Tests for the frame-marker codec (frost/perf/frame_marker.hpp): every value
// round-trips, every single-cell error is rejected, and the pixel decoder reads
// strips with padding, scaled cells and non-pure colours.

#include "frost/perf/frame_marker.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using frost::perf::decodeMarker;
using frost::perf::decodeMarkerPixels;
using frost::perf::encodeMarker;
using frost::perf::kMarkerCells;

TEST(FrameMarker, EveryValueRoundTrips) {
    for (unsigned value = 0; value <= 0xFFFFU; ++value) {
        const auto seq = static_cast<std::uint16_t>(value);
        const auto decoded = decodeMarker(encodeMarker(seq));
        ASSERT_TRUE(decoded.has_value()) << value;
        ASSERT_EQ(*decoded, seq);
    }
}

TEST(FrameMarker, SyncCellsAreWhiteThenBlack) {
    const auto cells = encodeMarker(0x1234);
    EXPECT_TRUE(cells[0]);
    EXPECT_FALSE(cells[1]);
}

TEST(FrameMarker, AnySingleCellErrorIsRejected) {
    for (const std::uint16_t seq :
         {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0x00FF}, std::uint16_t{0xBEEF}, std::uint16_t{0xFFFF}}) {
        for (int cell = 0; cell < kMarkerCells; ++cell) {
            auto cells = encodeMarker(seq);
            cells[static_cast<std::size_t>(cell)] = !cells[static_cast<std::size_t>(cell)];
            EXPECT_FALSE(decodeMarker(cells).has_value()) << "seq " << seq << " cell " << cell;
        }
    }
}

TEST(FrameMarker, AllWhiteAndAllBlackStripsAreRejected) {
    frost::perf::MarkerCells white{};
    white.fill(true);
    frost::perf::MarkerCells black{};
    EXPECT_FALSE(decodeMarker(white).has_value());
    EXPECT_FALSE(decodeMarker(black).has_value());
}

// Paints `seq` into a BGRA buffer the way a window would, with the given cell size,
// row padding and colours.
std::vector<std::uint8_t> paintStrip(std::uint16_t seq, int cellPx, std::size_t rowPitch, std::uint8_t on,
                                     std::uint8_t off) {
    std::vector<std::uint8_t> pixels(rowPitch * static_cast<std::size_t>(cellPx), 0x77);
    const auto cells = encodeMarker(seq);
    for (int y = 0; y < cellPx; ++y) {
        for (int x = 0; x < kMarkerCells * cellPx; ++x) {
            const bool white = cells[static_cast<std::size_t>(x / cellPx)];
            std::uint8_t* p = pixels.data() + static_cast<std::size_t>(y) * rowPitch + static_cast<std::size_t>(x) * 4;
            p[0] = white ? on : off;
            p[1] = white ? on : off;
            p[2] = white ? static_cast<std::uint8_t>(on - 10) : static_cast<std::uint8_t>(off + 10);
            p[3] = 0xFF;
        }
    }
    return pixels;
}

TEST(FrameMarker, DecodesPixelsWithPaddingAndOffWhiteColours) {
    const int cellPx = 4;
    const std::size_t rowPitch = static_cast<std::size_t>(kMarkerCells * cellPx * 4 + 64);
    const auto pixels = paintStrip(0xA5C3, cellPx, rowPitch, 230, 25);
    const auto decoded = decodeMarkerPixels(pixels.data(), rowPitch, cellPx);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, 0xA5C3);
}

TEST(FrameMarker, DecodesScaledCells) {
    // 150 % scaling makes 4 px cells 6 px wide.
    const int cellPx = 6;
    const std::size_t rowPitch = static_cast<std::size_t>(kMarkerCells * cellPx * 4);
    const auto pixels = paintStrip(4242, cellPx, rowPitch, 255, 0);
    const auto decoded = decodeMarkerPixels(pixels.data(), rowPitch, cellPx);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, 4242);
}

TEST(FrameMarker, HalfFloatConversion) {
    using frost::perf::halfToFloat;
    EXPECT_EQ(halfToFloat(0x0000), 0.0F);
    EXPECT_EQ(halfToFloat(0x3C00), 1.0F);
    EXPECT_EQ(halfToFloat(0xC000), -2.0F);
    EXPECT_EQ(halfToFloat(0x3800), 0.5F);
    EXPECT_EQ(halfToFloat(0x7BFF), 65504.0F);
    EXPECT_EQ(halfToFloat(0x0001), 5.9604644775390625e-8F); // Smallest subnormal, 2^-24.
    EXPECT_TRUE(std::isinf(halfToFloat(0x7C00)));
    EXPECT_TRUE(std::isnan(halfToFloat(0x7E00)));
}

TEST(FrameMarker, DecodesHalfFloatScRgbPixels) {
    // An HDR desktop composes SDR white at 2.5 (200 nits in scRGB) and black at 0.
    const int cellPx = 4;
    const std::size_t rowPitch = static_cast<std::size_t>(kMarkerCells * cellPx * 8 + 16);
    std::vector<std::uint8_t> pixels(rowPitch * static_cast<std::size_t>(cellPx), 0);
    const auto cells = encodeMarker(0x7E57);
    for (int y = 0; y < cellPx; ++y) {
        for (int x = 0; x < kMarkerCells * cellPx; ++x) {
            const std::uint16_t value = cells[static_cast<std::size_t>(x / cellPx)] ? 0x4100 : 0x0000; // 2.5 or 0.
            std::uint8_t* p = pixels.data() + static_cast<std::size_t>(y) * rowPitch + static_cast<std::size_t>(x) * 8;
            for (int channel = 0; channel < 4; ++channel) {
                p[channel * 2] = static_cast<std::uint8_t>(value & 0xFFU);
                p[channel * 2 + 1] = static_cast<std::uint8_t>(value >> 8U);
            }
        }
    }
    const auto decoded = decodeMarkerPixels(pixels.data(), rowPitch, cellPx, frost::perf::MarkerPixelFormat::Float16x4);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, 0x7E57);
    // The same bytes read as 8-bit pixels do not decode to a value.
    EXPECT_FALSE(decodeMarkerPixels(pixels.data(), rowPitch, cellPx).has_value());
}

TEST(FrameMarker, MidGreyStripDoesNotDecode) {
    const int cellPx = 4;
    const std::size_t rowPitch = static_cast<std::size_t>(kMarkerCells * cellPx * 4);
    const std::vector<std::uint8_t> grey(rowPitch * static_cast<std::size_t>(cellPx), 0x80);
    EXPECT_FALSE(decodeMarkerPixels(grey.data(), rowPitch, cellPx).has_value());
}

} // namespace
