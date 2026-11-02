// SPDX-License-Identifier: Apache-2.0
//
// TerminalColors (frost/ui/terminal/terminal_colors.hpp). The 16 ANSI colours, the cube levels and
// the grey ramp are xterm's documented defaults (xterm's 256colres.h, MIT licence).

#include "frost/ui/terminal/terminal_colors.hpp"

#include "frost/ui/model/theme_registry.hpp"

#include <algorithm>

namespace frost::ui {

namespace {

constexpr std::array<unsigned, 16> kAnsi{
    0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
    0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff,
};
constexpr std::array<int, 6> kCubeLevels{0, 95, 135, 175, 215, 255};

QColor fromRgba(Rgba color) noexcept { return QColor(color.r(), color.g(), color.b(), color.a()); }

std::array<QColor, 256> xtermPalette() {
    std::array<QColor, 256> palette{};
    for (int i = 0; i < 256; ++i) {
        palette[static_cast<std::size_t>(i)] = xtermPaletteColor(i);
    }
    return palette;
}

} // namespace

QColor xtermPaletteColor(int index) noexcept {
    index = std::clamp(index, 0, 255);
    if (index < 16) {
        return QColor::fromRgb(kAnsi[static_cast<std::size_t>(index)]);
    }
    if (index < 232) {
        const int cube = index - 16;
        return QColor(kCubeLevels[static_cast<std::size_t>(cube / 36)],
                      kCubeLevels[static_cast<std::size_t>((cube / 6) % 6)],
                      kCubeLevels[static_cast<std::size_t>(cube % 6)]);
    }
    const int grey = 8 + (10 * (index - 232));
    return QColor(grey, grey, grey);
}

TerminalColors TerminalColors::fromTheme(const Theme& theme) {
    TerminalColors colors;
    colors.foreground = fromRgba(theme.color(ColorKey::EditorForeground));
    colors.background = fromRgba(theme.color(ColorKey::EditorBackground));
    colors.cursor = fromRgba(theme.color(ColorKey::EditorCaretColor));
    colors.selection = fromRgba(theme.color(ColorKey::EditorSelectionBackground));
    colors.palette = xtermPalette();
    return colors;
}

TerminalColors TerminalColors::defaults() {
    TerminalColors colors;
    colors.foreground = QColor(0xbc, 0xbe, 0xc4);
    colors.background = QColor(0x1e, 0x1f, 0x22);
    colors.cursor = QColor(0xce, 0xd0, 0xd6);
    colors.selection = QColor(0x21, 0x42, 0x83);
    colors.palette = xtermPalette();
    return colors;
}

QColor TerminalColors::resolve(const term::CellColor& color, bool isForeground) const {
    switch (color.kind) {
    case term::CellColor::Kind::Default: return isForeground ? foreground : background;
    case term::CellColor::Kind::Indexed: return palette[color.index];
    case term::CellColor::Kind::Rgb: return QColor(color.red, color.green, color.blue);
    }
    return isForeground ? foreground : background;
}

} // namespace frost::ui
