// SPDX-License-Identifier: Apache-2.0
//
// The editor's insight vocabulary (PLAN.md M3: semantic highlighting, inactive regions, the error
// stripe, the inspection widget, squiggles; ADR 0047): the values the workbench feeds an EditorView
// through its seams (setSemanticTokens, setInactiveRanges, setDiagnostics, setDecorations,
// setUsageHighlights) and the summary the inspection widget and the status bar show. Plain values;
// nothing here knows about LSP or the build (frost::ui_lang and the workbench translate into them).
#pragma once

#include "frost/text/positions.hpp"

#include <QColor>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace frost::ui {

/*
 * What a semantic token names (Rider's C++ highlighting categories). The language bridge maps the
 * server's token types and modifiers onto these; each kind has a Semantic.* theme key.
 */
enum class SemanticKind : std::uint8_t {
    LocalVariable,
    Parameter,
    Field,
    StaticField,
    GlobalVariable,
    Function,
    Method,
    StaticMethod,
    Class, // Classes, structs and unions (clangd does not tell them apart).
    Enum,
    Enumerator,
    Namespace,
    Macro,
    TemplateParameter,
    Concept,
    TypeAlias,
    kCount
};

inline constexpr std::size_t kSemanticKindCount = static_cast<std::size_t>(SemanticKind::kCount);

// "localVariable", "staticField", ... (the Semantic.* key's last segment). O(1).
[[nodiscard]] std::string_view toString(SemanticKind kind) noexcept;

// Modifier bits of a SemanticSpan.
namespace semantic_modifiers {
inline constexpr std::uint16_t kReadonly = 1U << 0;   // const / constexpr: readonly globals paint as constants.
inline constexpr std::uint16_t kStatic = 1U << 1;     // Static members paint italic (IntelliJ's convention).
inline constexpr std::uint16_t kDeprecated = 1U << 2; // Struck through.
inline constexpr std::uint16_t kDeclaration = 1U << 3;
inline constexpr std::uint16_t kVirtual = 1U << 4;
inline constexpr std::uint16_t kDefaultLibrary = 1U << 5; // From the standard library.
} // namespace semantic_modifiers

// One semantic token in the editor's text.
struct SemanticSpan {
    text::ByteRange range;
    SemanticKind kind = SemanticKind::LocalVariable;
    std::uint16_t modifiers = 0; // semantic_modifiers bits.
    friend bool operator==(const SemanticSpan&, const SemanticSpan&) = default;
};

// How serious a problem is, most serious first (Rider's error, warning, weak warning, info).
enum class InsightSeverity : std::uint8_t { Error, Warning, WeakWarning, Info };
inline constexpr std::size_t kInsightSeverityCount = 4;

// "error", "warning", "weak warning" or "info". O(1).
[[nodiscard]] std::string_view toString(InsightSeverity severity) noexcept;

// Where a diagnostic came from; each source replaces only its own set.
enum class EditorDiagnosticSource : std::uint8_t { Build, Live };

/*
 * A problem to show in the editor (squiggle, stripe mark, gutter marker, inspection count, F2).
 * Live diagnostics carry a byte range of the text of `textVersion` (the view maps it through the
 * edits made since, or uses the line and column when that version is unknown); build diagnostics
 * carry only a 1-based line and column, resolved against the current text when they are set (the
 * identifier at that column, else one character; column 0: the whole line).
 */
struct EditorDiagnostic {
    InsightSeverity severity = InsightSeverity::Error;
    std::string message;
    std::string code; // "C2065", "-Wunused-variable", or "".
    std::optional<text::ByteRange> range;
    std::optional<std::uint64_t> textVersion; // The version `range` belongs to; nullopt: the current text.
    int line = 0;                             // 1-based; used when `range` is empty.
    int column = 0;                           // 1-based; 0: the whole line.
    friend bool operator==(const EditorDiagnostic&, const EditorDiagnostic&) = default;
};

// A diagnostic as the view holds it: its range in the current text and where it came from.
struct ActiveDiagnostic {
    text::ByteRange range;
    InsightSeverity severity = InsightSeverity::Error;
    EditorDiagnosticSource source = EditorDiagnosticSource::Live;
    std::string message;
    std::string code;
    friend bool operator==(const ActiveDiagnostic&, const ActiveDiagnostic&) = default;
};

/*
 * How a range decoration marks its text. The display list carries one DisplayDecoration per
 * decorated row segment, so every renderer draws the same marks.
 *   WavyUnderline:   errors and warnings (Rider's squiggle);
 *   DottedUnderline: weak warnings and infos;
 *   Underline:       a straight line (links, the IME's preedit uses an overlay instead);
 *   Strikeout:       deprecated symbols;
 *   Background:      a filled box behind the text (usages, custom highlights).
 */
enum class DecorationStyle : std::uint8_t { WavyUnderline, DottedUnderline, Underline, Strikeout, Background };

/*
 * A generic range decoration (ADR 0047): a range of the current text, a style, a colour and a
 * tooltip. With `severity` set the colour is the theme's marker colour of that severity (it follows
 * theme switches); otherwise `color` is used. Producers own named sets (EditorView::setDecorations
 * with an owner key), so the diagnostics, the workbench and later slices never overwrite each
 * other's marks. The integrator of the W1 workbench's own decoration API should route it here.
 */
struct EditorDecoration {
    text::ByteRange range;
    DecorationStyle style = DecorationStyle::WavyUnderline;
    std::optional<InsightSeverity> severity;
    QColor color;
    std::string tooltip;
    friend bool operator==(const EditorDecoration&, const EditorDecoration&) = default;
};

// A highlighted usage of the symbol under the caret (Rider: read and write usages differ).
struct UsageHighlight {
    text::ByteRange range;
    bool write = false;
    friend bool operator==(const UsageHighlight&, const UsageHighlight&) = default;
};

/*
 * What the inspection widget and the status-bar indicator show: problem counts per severity and
 * whether the analysis of the current text is still running. clean(): nothing to report and not
 * analysing (the green check).
 */
struct InsightSummary {
    std::array<int, kInsightSeverityCount> counts{};
    bool analyzing = false;

    [[nodiscard]] int count(InsightSeverity severity) const noexcept {
        return counts[static_cast<std::size_t>(severity)];
    }
    [[nodiscard]] int total() const noexcept { return counts[0] + counts[1] + counts[2] + counts[3]; }
    [[nodiscard]] bool clean() const noexcept { return !analyzing && total() == 0; }
    // The most serious severity with a problem, if any.
    [[nodiscard]] std::optional<InsightSeverity> worst() const noexcept;
    friend bool operator==(const InsightSummary&, const InsightSummary&) = default;
};

// "2 errors, 1 warning", "No problems", "Analyzing..." (English; TODO(M3): through tr()).
[[nodiscard]] std::string describe(const InsightSummary& summary);

// What a stripe mark stands for (painted lowest first: usages, search matches, info .. error).
enum class StripeMarkKind : std::uint8_t { Usage, WriteUsage, SearchMatch, Info, WeakWarning, Warning, Error };

[[nodiscard]] StripeMarkKind stripeKindFor(InsightSeverity severity) noexcept;

} // namespace frost::ui
