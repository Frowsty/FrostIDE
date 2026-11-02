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

ColorKey colorKeyFor(SemanticKind kind) noexcept {
    using K = SemanticKind;
    switch (kind) {
    case K::LocalVariable: return ColorKey::SemanticLocalVariable;
    case K::Parameter: return ColorKey::SemanticParameter;
    case K::Field: return ColorKey::SemanticField;
    case K::StaticField: return ColorKey::SemanticStaticField;
    case K::GlobalVariable: return ColorKey::SemanticGlobalVariable;
    case K::Function: return ColorKey::SemanticFunction;
    case K::Method: return ColorKey::SemanticMethod;
    case K::StaticMethod: return ColorKey::SemanticStaticMethod;
    case K::Class: return ColorKey::SemanticClass;
    case K::Enum: return ColorKey::SemanticEnum;
    case K::Enumerator: return ColorKey::SemanticEnumerator;
    case K::Namespace: return ColorKey::SemanticNamespace;
    case K::Macro: return ColorKey::SemanticMacro;
    case K::TemplateParameter: return ColorKey::SemanticTemplateParameter;
    case K::Concept: return ColorKey::SemanticConcept;
    case K::TypeAlias: return ColorKey::SemanticTypeAlias;
    case K::kCount: break;
    }
    return ColorKey::EditorForeground;
}

TokenFormat EditorColors::semanticFormat(SemanticKind kind, std::uint16_t modifiers) const noexcept {
    TokenFormat out = semantic[static_cast<std::size_t>(kind)];
    const bool readonly = (modifiers & semantic_modifiers::kReadonly) != 0;
    if (readonly && (kind == SemanticKind::GlobalVariable || kind == SemanticKind::StaticField)) {
        out.color = semanticConstant;
    }
    out.italic = kind == SemanticKind::StaticField || kind == SemanticKind::StaticMethod;
    return out;
}

QColor EditorColors::severityColor(InsightSeverity severity) const noexcept {
    switch (severity) {
    case InsightSeverity::Error: return errorMarker;
    case InsightSeverity::Warning: return warningMarker;
    case InsightSeverity::WeakWarning: return weakWarningMarker;
    case InsightSeverity::Info: return infoMarker;
    }
    return errorMarker;
}

QColor EditorColors::stripeColor(StripeMarkKind kind) const noexcept {
    switch (kind) {
    case StripeMarkKind::Usage:
    case StripeMarkKind::WriteUsage: return usageStripe;
    case StripeMarkKind::SearchMatch: return searchMatchStripe;
    case StripeMarkKind::Info: return infoMarker;
    case StripeMarkKind::WeakWarning: return weakWarningMarker;
    case StripeMarkKind::Warning: return warningMarker;
    case StripeMarkKind::Error: return errorMarker;
    }
    return errorMarker;
}

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
    for (std::size_t i = 0; i < syntax::kHighlightKindCount; ++i) {
        const auto kind = static_cast<syntax::HighlightKind>(i);
        colors.tokens[i] = TokenFormat{c(colorKeyFor(kind)), isItalic(kind)};
    }
    colors.weakWarningMarker = c(ColorKey::EditorWeakWarningMarkerColor);
    colors.infoMarker = c(ColorKey::EditorInfoMarkerColor);
    colors.cleanMarker = c(ColorKey::EditorCleanMarkerColor);
    colors.inactiveCode = c(ColorKey::EditorInactiveCodeForeground);
    colors.stripeBackground = c(ColorKey::EditorErrorStripeBackground);
    colors.searchMatchStripe = c(ColorKey::EditorSearchMatchStripeColor);
    colors.usage = c(ColorKey::EditorUsageHighlightBackground);
    colors.writeUsage = c(ColorKey::EditorWriteUsageHighlightBackground);
    colors.usageStripe = c(ColorKey::EditorUsageStripeColor);
    colors.inspectionBackground = c(ColorKey::EditorInspectionWidgetBackground);
    colors.inspectionForeground = c(ColorKey::EditorInspectionWidgetForeground);
    colors.semanticConstant = c(ColorKey::SemanticConstant);
    for (std::size_t i = 0; i < kSemanticKindCount; ++i) {
        colors.semantic[i] = TokenFormat{c(colorKeyFor(static_cast<SemanticKind>(i))), false};
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
    for (std::size_t i = 0; i < syntax::kHighlightKindCount; ++i) {
        colors.tokens[i] = TokenFormat{colors.foreground, isItalic(static_cast<syntax::HighlightKind>(i))};
    }
    colors.weakWarningMarker = QColor(0xA4, 0x9F, 0x86);
    colors.infoMarker = QColor(0x6C, 0xA2, 0xFF);
    colors.cleanMarker = QColor(0x5F, 0xB8, 0x78);
    colors.inactiveCode = QColor(0x5C, 0x61, 0x68);
    colors.stripeBackground = colors.background;
    colors.searchMatchStripe = QColor(0xC9, 0xA3, 0x3E);
    colors.usage = QColor(0x36, 0x3B, 0x47);
    colors.writeUsage = QColor(0x47, 0x38, 0x3F);
    colors.usageStripe = QColor(0x8E, 0x9B, 0xB3);
    colors.inspectionBackground = QColor(0x2A, 0x2D, 0x33);
    colors.inspectionForeground = QColor(0x9D, 0xA3, 0xAD);
    colors.semanticConstant = colors.foreground;
    for (std::size_t i = 0; i < kSemanticKindCount; ++i) {
        colors.semantic[i] = TokenFormat{colors.foreground, false};
    }
    return colors;
}

} // namespace frost::ui
