// SPDX-License-Identifier: Apache-2.0
//
// The editor's colours (ADR 0037): the Editor.* and Syntax.* theme keys resolved into QColors, and
// the mapping from frost::syntax highlight kinds to token formats (colour and style).
#pragma once

#include "frost/syntax/highlight.hpp"
#include "frost/ui/model/rgba.hpp"
#include "frost/ui/model/theme.hpp"
#include "frost/ui/model/theme_registry.hpp"

#include <QColor>

#include <array>

namespace frost::ui {

// A Rgba theme colour as a QColor. O(1).
[[nodiscard]] QColor toQColor(Rgba color) noexcept;

// How a highlight kind is painted.
struct TokenFormat {
    QColor color;
    bool italic = false;

    friend bool operator==(const TokenFormat&, const TokenFormat&) = default;
};

/*
 * The theme key each highlight kind takes its colour from. Several kinds share a key (Bracket and
 * Punctuation, Boolean and Keyword, Character and String, Quote and Comment, ListMarker and
 * Punctuation); the Syntax.* keys fall back to each other and finally to Editor.foreground, so a
 * theme that sets only a few token colours still paints every kind. O(1).
 */
[[nodiscard]] ColorKey colorKeyFor(syntax::HighlightKind kind) noexcept;
// Doc comments are italic; every other kind is upright. O(1).
[[nodiscard]] bool isItalic(syntax::HighlightKind kind) noexcept;

// Every colour an EditorView paints with. A plain value; copy freely.
struct EditorColors {
    QColor background;
    QColor foreground;
    QColor caret;
    QColor selection;
    QColor currentLine;
    QColor gutterBackground;
    QColor lineNumber;
    QColor lineNumberCurrent;
    QColor searchMatch;
    QColor rightMargin;
    QColor errorMarker;
    QColor warningMarker;
    std::array<TokenFormat, syntax::kHighlightKindCount> tokens{};

    // The colours of `theme`. O(kinds).
    [[nodiscard]] static EditorColors fromTheme(const Theme& theme);
    // Built-in dark colours for an editor created without any theme (tests, tools).
    [[nodiscard]] static EditorColors defaults();
    // The format of `kind`. Precondition: a valid kind.
    [[nodiscard]] const TokenFormat& format(syntax::HighlightKind kind) const noexcept {
        return tokens[static_cast<std::size_t>(kind)];
    }
};

} // namespace frost::ui
