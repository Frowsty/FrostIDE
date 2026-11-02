// SPDX-License-Identifier: Apache-2.0
//
// Editor colours (frost/ui/editor/editor_colors.hpp): the highlight-kind table and the theme lookup.

#include "frost/ui/editor/editor_colors.hpp"

namespace frost::ui {

QColor toQColor(Rgba color) noexcept { return QColor(color.r(), color.g(), color.b(), color.a()); }

ColorKey colorKeyFor(syntax::HighlightKind kind) noexcept {
    using K = syntax::HighlightKind;
    switch (kind) {
    case K::Comment: return ColorKey::SyntaxComment;
    case K::DocComment: return ColorKey::SyntaxDocComment;
    case K::Keyword: return ColorKey::SyntaxKeyword;
    case K::Directive: return ColorKey::SyntaxDirective;
    case K::Operator: return ColorKey::SyntaxOperator;
    case K::Punctuation: return ColorKey::SyntaxPunctuation;
    case K::Bracket: return ColorKey::SyntaxPunctuation;
    case K::String: return ColorKey::SyntaxString;
    case K::StringEscape: return ColorKey::SyntaxStringEscape;
    case K::Character: return ColorKey::SyntaxString;
    case K::Number: return ColorKey::SyntaxNumber;
    case K::Boolean: return ColorKey::SyntaxKeyword;
    case K::Constant: return ColorKey::SyntaxConstant;
    case K::Type: return ColorKey::SyntaxType;
    case K::BuiltinType: return ColorKey::SyntaxBuiltinType;
    case K::Namespace: return ColorKey::SyntaxNamespace;
    case K::Function: return ColorKey::SyntaxFunction;
    case K::FunctionCall: return ColorKey::SyntaxFunctionCall;
    case K::Macro: return ColorKey::SyntaxMacro;
    case K::Variable: return ColorKey::SyntaxVariable;
    case K::Parameter: return ColorKey::SyntaxParameter;
    case K::Property: return ColorKey::SyntaxProperty;
    case K::Attribute: return ColorKey::SyntaxAttribute;
    case K::Label: return ColorKey::SyntaxLabel;
    case K::Tag: return ColorKey::SyntaxTag;
    case K::TagAttribute: return ColorKey::SyntaxTagAttribute;
    case K::Heading: return ColorKey::SyntaxHeading;
    case K::Link: return ColorKey::SyntaxLink;
    case K::Raw: return ColorKey::SyntaxRaw;
    case K::ListMarker: return ColorKey::SyntaxPunctuation;
    case K::Quote: return ColorKey::SyntaxComment;
    }
    return ColorKey::EditorForeground;
}

bool isItalic(syntax::HighlightKind kind) noexcept { return kind == syntax::HighlightKind::DocComment; }

EditorColors EditorColors::fromTheme(const Theme& theme) {
    const auto c = [&](ColorKey key) { return toQColor(theme.color(key)); };
    EditorColors colors;
    colors.background = c(ColorKey::EditorBackground);
    colors.foreground = c(ColorKey::EditorForeground);
    colors.caret = c(ColorKey::EditorCaretColor);
    colors.selection = c(ColorKey::EditorSelectionBackground);
    colors.currentLine = c(ColorKey::EditorCurrentLineBackground);
    colors.gutterBackground = c(ColorKey::EditorGutterBackground);
    colors.lineNumber = c(ColorKey::EditorLineNumberForeground);
    colors.lineNumberCurrent = c(ColorKey::EditorLineNumberCurrentForeground);
    colors.searchMatch = c(ColorKey::EditorSearchMatchBackground);
    colors.rightMargin = c(ColorKey::EditorRightMarginColor);
    colors.errorMarker = c(ColorKey::EditorErrorMarkerColor);
    colors.warningMarker = c(ColorKey::EditorWarningMarkerColor);
    colors.changeAdded = c(ColorKey::EditorChangeAddedColor);
    colors.changeModified = c(ColorKey::EditorChangeModifiedColor);
    colors.changeDeleted = c(ColorKey::EditorChangeDeletedColor);
    for (std::size_t i = 0; i < syntax::kHighlightKindCount; ++i) {
        const auto kind = static_cast<syntax::HighlightKind>(i);
        colors.tokens[i] = TokenFormat{c(colorKeyFor(kind)), isItalic(kind)};
    }
    return colors;
}

EditorColors EditorColors::defaults() {
    EditorColors colors;
    colors.background = QColor(0x23, 0x25, 0x2A);
    colors.foreground = QColor(0xDF, 0xE1, 0xE5);
    colors.caret = QColor(0xE8, 0xEA, 0xED);
    colors.selection = QColor(0x2E, 0x43, 0x70);
    colors.currentLine = QColor(0x2A, 0x2D, 0x33);
    colors.gutterBackground = colors.background;
    colors.lineNumber = QColor(0x6B, 0x70, 0x79);
    colors.lineNumberCurrent = QColor(0xA9, 0xAE, 0xB6);
    colors.searchMatch = QColor(0x5A, 0x4A, 0x1C);
    colors.rightMargin = QColor(0x34, 0x37, 0x3D);
    colors.errorMarker = QColor(0xF2, 0x71, 0x6F);
    colors.warningMarker = QColor(0xE8, 0xB0, 0x4B);
    colors.changeAdded = QColor(0x6F, 0xBF, 0x73);
    colors.changeModified = QColor(0x6D, 0x9F, 0xE6);
    colors.changeDeleted = QColor(0x8E, 0x94, 0x9C);
    for (std::size_t i = 0; i < syntax::kHighlightKindCount; ++i) {
        colors.tokens[i] = TokenFormat{colors.foreground, isItalic(static_cast<syntax::HighlightKind>(i))};
    }
    return colors;
}

} // namespace frost::ui
