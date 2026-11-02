// SPDX-License-Identifier: Apache-2.0
//
// The terminal's colours (ADR 0051): the theme's editor colours for the default foreground,
// background, cursor and selection, and the 256-colour xterm palette for indexed cell colours.
#pragma once

#include "frost/term/vt_screen.hpp"
#include "frost/ui/model/theme.hpp"

#include <QColor>

#include <array>

namespace frost::ui {

// Every colour a TerminalView paints with. A plain value; copy freely.
struct TerminalColors {
    QColor foreground;
    QColor background;
    QColor cursor;
    QColor selection;
    // 0-15 the ANSI colours, 16-231 the 6x6x6 cube, 232-255 the grey ramp (xterm's values).
    std::array<QColor, 256> palette{};

    // Editor.foreground, Editor.background, Editor.caretColor and Editor.selectionBackground of
    // `theme`, with the xterm palette. O(1).
    [[nodiscard]] static TerminalColors fromTheme(const Theme& theme);
    // Built-in dark colours for a terminal created without a theme (tests). O(1).
    [[nodiscard]] static TerminalColors defaults();
    // The colour of a cell's foreground (`isForeground`) or background. O(1).
    [[nodiscard]] QColor resolve(const term::CellColor& color, bool isForeground) const;
};

// xterm's 256-colour palette entry `index`. O(1).
[[nodiscard]] QColor xtermPaletteColor(int index) noexcept;

} // namespace frost::ui
