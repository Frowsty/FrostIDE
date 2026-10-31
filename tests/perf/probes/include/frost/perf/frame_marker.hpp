// SPDX-License-Identifier: Apache-2.0
//
// Frame markers: a one-row strip of black and white cells, painted by the
// window under test, that encodes a 16-bit frame sequence number. The Desktop
// Duplication probe reads the strip back from the composed desktop image, so
// the harness learns exactly which application frame reached the screen and
// when, whatever renders it (raster QPainter, QRhi, GDI). Cells are large and
// binary, so the code survives scaling-free colour transforms (colour
// management, night light) that would corrupt an RGB-coded counter.
//
// Layout, left to right, kMarkerCells cells of cellPx x cellPx pixels each:
//   cell 0      white   sync
//   cell 1      black   sync
//   cells 2-17  data    sequence number, least significant bit first
//   cells 18-21 check   XOR of the four data nibbles, least significant bit first
// White means 1. A strip that fails the sync or the check decodes as nothing,
// which rejects torn or half-composed frames instead of misreading them.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace frost::perf {

inline constexpr int kMarkerCells = 22;

// The marker cell pattern, true = white.
using MarkerCells = std::array<bool, kMarkerCells>;

// Encodes `seq`. O(1), thread-safe.
[[nodiscard]] MarkerCells encodeMarker(std::uint16_t seq) noexcept;

// Decodes a cell pattern; nullopt when the sync cells or the check nibble do not match.
[[nodiscard]] std::optional<std::uint16_t> decodeMarker(const MarkerCells& cells) noexcept;

// Pixel layouts the decoder reads. Only luminance matters, so BGRA and RGBA
// orders are equivalent.
enum class MarkerPixelFormat : std::uint8_t {
    Unorm8x4,  // 8-bit channels (DXGI B8G8R8A8_UNORM, R8G8B8A8_UNORM, QImage ARGB32).
    Float16x4, // IEEE half-float channels, linear scRGB (DXGI R16G16B16A16_FLOAT, HDR desktops).
};

// Decodes a marker from a pixel buffer. `topLeft` points at the strip's first
// pixel, `rowPitch` is the row stride in bytes and the strip spans
// kMarkerCells * cellPx pixels by cellPx rows. Each cell is sampled at its centre
// and is white when its mean colour channel is at least half of SDR white
// (128 for 8-bit, 0.5 for scRGB, where SDR white is 1.0 or brighter).
// Precondition: cellPx >= 1 and the buffer covers the strip.
[[nodiscard]] std::optional<std::uint16_t>
decodeMarkerPixels(const std::uint8_t* topLeft, std::size_t rowPitch, int cellPx,
                   MarkerPixelFormat format = MarkerPixelFormat::Unorm8x4) noexcept;

// Converts an IEEE 754 binary16 value to float (exact; handles subnormals, infinities and NaN).
[[nodiscard]] float halfToFloat(std::uint16_t half) noexcept;

} // namespace frost::perf
