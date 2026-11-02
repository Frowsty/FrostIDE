// SPDX-License-Identifier: Apache-2.0
//
// The editor's display list (PLAN.md section 2 "Editor rendering"; ADR 0037): everything one frame
// of an EditorView shows, as backend-neutral primitives in viewport pixels. EditorView builds it from
// the DisplayMap, the selections, the highlights and the theme; an IEditorRenderer draws it. Tests
// read it to check what would be painted without comparing pixels. ADR 0047 adds semantic and
// inactive text runs, range decorations (squiggles, strike-outs, backgrounds), the error stripe and
// the chrome layer (the inspection widget and the problem balloon, as plain shapes and text).
#pragma once

#include "frost/syntax/highlight.hpp"
#include "frost/text/positions.hpp"
#include "frost/ui/editor/editor_insight.hpp"

#include <QColor>
#include <QFont>
#include <QLineF>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace frost::ui {

// How serious a gutter marker is (the error list fills them; W1).
enum class MarkerSeverity : std::uint8_t { Error, Warning };

// A filled rectangle (current line, selection, search match, gutter background).
struct DisplayFill {
    QRectF rect;
    QColor color;
};

/*
 * One run of text in one colour, drawn with its baseline's left end at `origin`. Runs never hold a
 * tab or a line break. `kind` is the highlight span the run came from (nullopt for plain text),
 * `semantic` the semantic token that coloured it (it wins over `kind`), `inactive` whether it lies
 * in an inactive preprocessor region (dimmed; wins over both), and `source` the buffer bytes it
 * shows; all but the text, colour and style are for tests and later backends.
 */
struct DisplayText {
    QPointF origin;
    QString text;
    QColor color;
    bool italic = false;
    std::optional<syntax::HighlightKind> kind;
    text::ByteRange source;
    std::optional<SemanticKind> semantic;
    bool inactive = false;
};

// A straight line (the right margin under the text, the IME preedit underline over it).
struct DisplayLine {
    QLineF line;
    QColor color;
    double width = 1.0;
};

// A gutter marker on one buffer line.
struct DisplayMarker {
    QRectF rect;
    QColor color;
    MarkerSeverity severity = MarkerSeverity::Error;
    LineIndex line;
};

// What a VCS change stripe marks (M5, ADR 0048): lines added or modified against the file's HEAD
// version, or the place where lines were deleted.
enum class ChangeStripeKind : std::uint8_t { Added, Modified, Deleted };

/*
 * A VCS change marker in the gutter: a narrow bar beside the lines of an added or modified block
 * (one rectangle per display row), or a short horizontal mark on the top edge of the line that
 * follows deleted lines. `change` is the index into EditorView::lineChanges(), for the click.
 */
struct DisplayChangeStripe {
    QRectF rect;
    QColor color;
    ChangeStripeKind kind = ChangeStripeKind::Modified;
    LineIndex line;
    std::size_t change = 0;
};

/*
 * A range decoration on one display row (ADR 0047): `rect` is the decorated text's box on the row
 * (its x range and the row's top and height), `baseline` the row's text baseline. Wavy and dotted
 * underlines and straight underlines sit just below the baseline, a strike-out through the middle
 * of the x-height, a background fills the box. `severity` is set for diagnostics' squiggles and
 * `tooltip` repeats the decoration's message (tests; the view answers tooltips itself).
 */
struct DisplayDecoration {
    QRectF rect;
    double baseline = 0.0;
    DecorationStyle style = DecorationStyle::WavyUnderline;
    QColor color;
    double width = 1.0; // The stroke width in pixels (zoomed).
    text::ByteRange source;
    std::optional<InsightSeverity> severity;
    QString tooltip;
};

// A mark in the error stripe beside the scroll bar: a problem, a search match or a usage.
struct DisplayStripeMark {
    QRectF rect;
    QColor color;
    StripeMarkKind kind = StripeMarkKind::Error;
    text::ByteRange source;
    LineIndex line;
};

/*
 * A primitive of the chrome layer, drawn last and unclipped: a rectangle (with a corner radius), an
 * ellipse, a filled polygon or a stroked polyline. An invalid fill or stroke colour means none.
 */
struct DisplayShape {
    enum class Kind : std::uint8_t { Rect, Ellipse, Polygon, Polyline };
    Kind kind = Kind::Rect;
    QRectF rect;      // Rect and Ellipse.
    QPolygonF points; // Polygon and Polyline.
    double radius = 0.0;
    QColor fill;
    QColor stroke;
    double strokeWidth = 1.0;
};

// What the inspection widget in the top-right corner shows.
enum class InspectionState : std::uint8_t { Hidden, Clean, Analyzing, Problems };

/*
 * The inspection widget's content and hit rectangles (Rider's "N errors, N warnings" with up/down
 * arrows; a green check when clean, a spinner while analysing). Drawn through `chrome`; the
 * rectangles are what clicks and tooltips test (EditorView answers them from the same layout).
 */
struct DisplayInspection {
    InspectionState state = InspectionState::Hidden;
    QRectF rect;
    InsightSummary summary;
    QRectF previous; // The up arrow (Shift+F2); empty without problems.
    QRectF next;     // The down arrow (F2).
};

// The balloon F2 shows at the caret with the problem's message.
struct DisplayBalloon {
    QRectF rect;
    QString text;
    InsightSeverity severity = InsightSeverity::Error;
};

/*
 * One frame. Paint order: background, `fills`, Background `decorations`, `underlines` (right
 * margin), `text`, the other `decorations`, `overlays` (preedit underline) and `carets`, all clipped
 * to textArea; then the gutter: gutterBackground over `gutter`, `gutterText`, `changeStripes` and
 * `markers`; then the error stripe: stripeBackground over `stripe`, `stripeMarks` and the summary
 * box; then `chrome` shapes and `chromeText`, unclipped. The raster backend draws all of them; the
 * QRhi backend (ADR 0043) draws the fills, lines, text, carets and the gutter with `changeStripes`
 * and `markers`, not yet the decorations, the error stripe or the chrome layer. Plain values; built
 * and read on the GUI thread.
 */
struct EditorDisplayList {
    QRectF viewport;
    QColor background;
    QRectF textArea;
    QRectF gutter;
    QColor gutterBackground;
    QFont font;
    std::vector<DisplayFill> fills;
    std::vector<DisplayLine> underlines;
    std::vector<DisplayText> text;
    std::vector<DisplayDecoration> decorations;
    std::vector<DisplayLine> overlays;
    std::vector<QRectF> carets;
    QColor caretColor;
    std::vector<DisplayText> gutterText;
    std::vector<DisplayMarker> markers;
    std::vector<DisplayChangeStripe> changeStripes;
    // The error stripe (empty rect when hidden) and its summary box (the worst severity's colour,
    // or the clean colour).
    QRectF stripe;
    QColor stripeBackground;
    std::vector<DisplayStripeMark> stripeMarks;
    QRectF stripeSummary;
    QColor stripeSummaryColor;
    // The chrome layer.
    DisplayInspection inspection;
    std::optional<DisplayBalloon> balloon;
    std::vector<DisplayShape> chrome;
    std::vector<DisplayText> chromeText;
    // The display rows shown, from firstRow.
    std::uint32_t firstRow = 0;
    std::uint32_t rowCount = 0;

    // The first text run whose text is exactly `text`, or nullptr. O(runs).
    [[nodiscard]] const DisplayText* findText(const QString& text) const noexcept;
    // The text run that shows buffer byte `offset`, or nullptr. O(runs).
    [[nodiscard]] const DisplayText* findTextAt(ByteOffset offset) const noexcept;
    // The gutter text of `number` (a 1-based line number), or nullptr. O(rows).
    [[nodiscard]] const DisplayText* findLineNumber(int number) const noexcept;
    // The decorations of one style, in paint order. O(decorations).
    [[nodiscard]] std::vector<const DisplayDecoration*> decorationsWithStyle(DecorationStyle style) const;
};

} // namespace frost::ui
