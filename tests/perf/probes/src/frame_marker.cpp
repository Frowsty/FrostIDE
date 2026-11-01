// SPDX-License-Identifier: Apache-2.0
//
// Implementation of the frame-marker codec (see frost/perf/frame_marker.hpp).

#include "frost/perf/frame_marker.hpp"

#include "frost/core/assert.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace frost::perf {

namespace {

constexpr int kDataFirst = 2;
constexpr int kDataBits = 16;
constexpr int kCheckFirst = kDataFirst + kDataBits;
constexpr int kCheckBits = 4;
static_assert(kCheckFirst + kCheckBits == kMarkerCells, "marker layout must fill every cell");

constexpr unsigned checkNibble(std::uint16_t seq) noexcept {
    return (seq ^ (seq >> 4U) ^ (seq >> 8U) ^ (seq >> 12U)) & 0xFU;
}

} // namespace

MarkerCells encodeMarker(std::uint16_t seq) noexcept {
    MarkerCells cells{};
    cells[0] = true;
    cells[1] = false;
    for (int bit = 0; bit < kDataBits; ++bit) {
        cells[static_cast<std::size_t>(kDataFirst + bit)] = ((seq >> bit) & 1U) != 0;
    }
    const unsigned check = checkNibble(seq);
    for (int bit = 0; bit < kCheckBits; ++bit) {
        cells[static_cast<std::size_t>(kCheckFirst + bit)] = ((check >> bit) & 1U) != 0;
    }
    return cells;
}

std::optional<std::uint16_t> decodeMarker(const MarkerCells& cells) noexcept {
    if (!cells[0] || cells[1]) {
        return std::nullopt;
    }
    unsigned seq = 0;
    for (int bit = 0; bit < kDataBits; ++bit) {
        if (cells[static_cast<std::size_t>(kDataFirst + bit)]) {
            seq |= 1U << bit;
        }
    }
    unsigned check = 0;
    for (int bit = 0; bit < kCheckBits; ++bit) {
        if (cells[static_cast<std::size_t>(kCheckFirst + bit)]) {
            check |= 1U << bit;
        }
    }
    const auto value = static_cast<std::uint16_t>(seq);
    if (check != checkNibble(value)) {
        return std::nullopt;
    }
    return value;
}

float halfToFloat(std::uint16_t half) noexcept {
    const unsigned sign = (half >> 15U) & 1U;
    const unsigned exponent = (half >> 10U) & 0x1FU;
    const unsigned mantissa = half & 0x3FFU;
    float magnitude = 0.0F;
    if (exponent == 0) {
        magnitude = std::ldexp(static_cast<float>(mantissa), -24); // Subnormal: m * 2^-24.
    } else if (exponent == 31) {
        magnitude = mantissa == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
    } else {
        magnitude = std::ldexp(static_cast<float>(mantissa | 0x400U), static_cast<int>(exponent) - 25);
    }
    return sign != 0 ? -magnitude : magnitude;
}

std::optional<std::uint16_t> decodeMarkerPixels(const std::uint8_t* topLeft, std::size_t rowPitch, int cellPx,
                                                MarkerPixelFormat format) noexcept {
    FROST_PRE(topLeft != nullptr);
    FROST_PRE(cellPx >= 1);
    const std::size_t pixelBytes = format == MarkerPixelFormat::Float16x4 ? 8 : 4;
    const auto centre = static_cast<std::size_t>(cellPx / 2);
    const std::uint8_t* row = topLeft + centre * rowPitch;
    MarkerCells cells{};
    for (int cell = 0; cell < kMarkerCells; ++cell) {
        const std::size_t x = static_cast<std::size_t>(cell) * static_cast<std::size_t>(cellPx) + centre;
        const std::uint8_t* pixel = row + x * pixelBytes;
        // Mean of the three colour channels; the fourth (alpha or padding) is ignored.
        bool white = false;
        if (format == MarkerPixelFormat::Float16x4) {
            float sum = 0.0F;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                std::uint16_t half = 0;
                std::memcpy(&half, pixel + channel * 2, sizeof(half));
                sum += halfToFloat(half);
            }
            white = sum / 3.0F >= 0.5F; // NaN compares false: black.
        } else {
            const unsigned luminance = (unsigned{pixel[0]} + unsigned{pixel[1]} + unsigned{pixel[2]}) / 3U;
            white = luminance >= 128U;
        }
        cells[static_cast<std::size_t>(cell)] = white;
    }
    return decodeMarker(cells);
}

} // namespace frost::perf
