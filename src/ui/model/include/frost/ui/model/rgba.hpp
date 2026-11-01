// SPDX-License-Identifier: Apache-2.0
//
// Rgba: a packed 8-bit-per-channel colour (non-premultiplied ARGB) for the theme model,
// with the "#RRGGBB" / "#RRGGBBAA" parser of the .theme.json format and source-over
// compositing (ADR 0015). Qt-free; FrostStyle converts to QColor at the boundary.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace frost::ui {

// A colour as 0xAARRGGBB, straight (not premultiplied) alpha. Every member is constexpr,
// O(1), noexcept and thread-safe.
struct Rgba {
    std::uint32_t argb = 0xff000000U;

    [[nodiscard]] static constexpr Rgba fromChannels(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                                     std::uint8_t a = 255) noexcept {
        return Rgba{(std::uint32_t{a} << 24) | (std::uint32_t{r} << 16) | (std::uint32_t{g} << 8) | std::uint32_t{b}};
    }
    [[nodiscard]] constexpr std::uint8_t a() const noexcept { return static_cast<std::uint8_t>(argb >> 24); }
    [[nodiscard]] constexpr std::uint8_t r() const noexcept { return static_cast<std::uint8_t>(argb >> 16); }
    [[nodiscard]] constexpr std::uint8_t g() const noexcept { return static_cast<std::uint8_t>(argb >> 8); }
    [[nodiscard]] constexpr std::uint8_t b() const noexcept { return static_cast<std::uint8_t>(argb); }
    [[nodiscard]] constexpr bool opaque() const noexcept { return a() == 255; }

    // "#RRGGBB" for opaque colours, "#RRGGBBAA" otherwise (upper-case hex). Allocates.
    [[nodiscard]] std::string toHex() const;

    friend constexpr bool operator==(Rgba, Rgba) noexcept = default;
};

namespace detail {
[[nodiscard]] constexpr int hexDigit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}
} // namespace detail

// Parses exactly "#RRGGBB" or "#RRGGBBAA" (alpha last, as in the theme format), hex digits in
// either case. Anything else, including "#RGB", "#RRGGBBA", a missing '#' or surrounding
// spaces, yields nullopt.
[[nodiscard]] constexpr std::optional<Rgba> parseHexColor(std::string_view text) noexcept {
    if ((text.size() != 7 && text.size() != 9) || text[0] != '#') {
        return std::nullopt;
    }
    std::uint32_t channels[4] = {0, 0, 0, 255};
    for (std::size_t i = 0; i * 2 + 1 < text.size(); ++i) {
        const int hi = detail::hexDigit(text[1 + i * 2]);
        const int lo = detail::hexDigit(text[2 + i * 2]);
        if (hi < 0 || lo < 0) {
            return std::nullopt;
        }
        channels[i] = static_cast<std::uint32_t>(hi * 16 + lo);
    }
    return Rgba{(channels[3] << 24) | (channels[0] << 16) | (channels[1] << 8) | channels[2]};
}

// Source-over compositing of `top` onto an opaque `bottom`, per channel
// round((top * a + bottom * (255 - a)) / 255). Precondition: bottom.opaque() (not checked;
// a translucent bottom is treated as opaque). The result is opaque.
[[nodiscard]] constexpr Rgba compositeOver(Rgba top, Rgba bottom) noexcept {
    const std::uint32_t a = top.a();
    const auto mix = [a](std::uint32_t t, std::uint32_t u) {
        return static_cast<std::uint8_t>((t * a + u * (255 - a) + 127) / 255);
    };
    return Rgba::fromChannels(mix(top.r(), bottom.r()), mix(top.g(), bottom.g()), mix(top.b(), bottom.b()), 255);
}

} // namespace frost::ui
