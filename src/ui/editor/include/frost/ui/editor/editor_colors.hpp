// SPDX-License-Identifier: Apache-2.0
//
// The editor's colours (ADR 0037): the Editor.* and Syntax.* theme keys resolved into QColors, and
// the mapping from frost::syntax highlight kinds to token formats (colour and style); the Semantic.*
// keys and the insight colours (inactive code, the error stripe, usages, the inspection widget;
// ADR 0047).
#pragma once

#include "frost/syntax/highlight.hpp"
#include "frost/ui/editor/editor_insight.hpp"
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
// The Semantic.* key of a semantic kind. O(1).
[[nodiscard]] ColorKey colorKeyFor(SemanticKind kind) noexcept;

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
    // Insight (ADR 0047).
    QColor weakWarningMarker;
    QColor infoMarker;
    QColor cleanMarker;
    QColor inactiveCode;
    QColor stripeBackground;
    QColor searchMatchStripe;
    QColor usage;
    QColor writeUsage;
    QColor usageStripe;
    QColor inspectionBackground;
    QColor inspectionForeground;
    QColor semanticConstant; // Readonly globals and static fields.
    std::array<TokenFormat, kSemanticKindCount> semantic{};

    // The colours of `theme`. O(kinds).
    [[nodiscard]] static EditorColors fromTheme(const Theme& theme);
    // Built-in dark colours for an editor created without any theme (tests, tools).
    [[nodiscard]] static EditorColors defaults();
    // The format of `kind`. Precondition: a valid kind.
    [[nodiscard]] const TokenFormat& format(syntax::HighlightKind kind) const noexcept {
        return tokens[static_cast<std::size_t>(kind)];
    }
    /*
     * The format of a semantic span: its kind's colour; readonly globals and static fields take
     * Semantic.constant; static fields and methods are italic (IntelliJ's convention for statics).
     * Deprecation is a strike-out decoration, not part of the format. O(1).
     */
    [[nodiscard]] TokenFormat semanticFormat(SemanticKind kind, std::uint16_t modifiers) const noexcept;
    // The marker colour of a severity (squiggles, stripe marks, gutter dots). O(1).
    [[nodiscard]] QColor severityColor(InsightSeverity severity) const noexcept;
    // The colour of a stripe mark. O(1).
    [[nodiscard]] QColor stripeColor(StripeMarkKind kind) const noexcept;
};

} // namespace frost::ui
