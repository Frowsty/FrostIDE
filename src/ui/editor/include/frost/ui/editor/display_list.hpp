// SPDX-License-Identifier: Apache-2.0
//
// The editor's display list (PLAN.md section 2 "Editor rendering"; ADR 0037): everything one frame
// of an EditorView shows, as backend-neutral primitives in viewport pixels. EditorView builds it from
// the DisplayMap, the selections, the highlights and the theme; an IEditorRenderer draws it. Tests
// read it to check what would be painted without comparing pixels.
#pragma once

#include "frost/syntax/highlight.hpp"
#include "frost/text/positions.hpp"

#include <QColor>
#include <QFont>
#include <QLineF>
#include <QPointF>
#include <QRectF>
#include <QString>

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
 * tab or a line break. `kind` is the highlight span the run came from (nullopt for plain text) and
 * `source` the buffer bytes it shows; both are for tests and later backends, the raster renderer
 * draws only text, colour and style.
 */
struct DisplayText {
    QPointF origin;
    QString text;
    QColor color;
    bool italic = false;
    std::optional<syntax::HighlightKind> kind;
    text::ByteRange source;
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

/*
 * One frame. Paint order: background, `fills`, `underlines` (right margin), `text` (clipped to
 * textArea), `overlays` (preedit underline), `carets`, then the gutter: gutterBackground over
 * `gutter`, `gutterText` and `markers`. Plain values; built and read on the GUI thread.
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
    std::vector<DisplayLine> overlays;
    std::vector<QRectF> carets;
    QColor caretColor;
    std::vector<DisplayText> gutterText;
    std::vector<DisplayMarker> markers;
    // The display rows shown, from firstRow.
    std::uint32_t firstRow = 0;
    std::uint32_t rowCount = 0;

    // The first text run whose text is exactly `text`, or nullptr. O(runs).
    [[nodiscard]] const DisplayText* findText(const QString& text) const noexcept;
    // The gutter text of `number` (a 1-based line number), or nullptr. O(rows).
    [[nodiscard]] const DisplayText* findLineNumber(int number) const noexcept;
};

} // namespace frost::ui
