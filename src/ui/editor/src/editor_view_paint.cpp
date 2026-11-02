// SPDX-License-Identifier: Apache-2.0
//
// EditorView (frost/ui/editor/editor_view.hpp), part 2: geometry, scrolling and painting. The frame
// is built as an EditorDisplayList (rows from the DisplayMap, colours from the theme, highlight spans
// from the syntax tree, semantic tokens and inactive regions over them, range decorations, and the
// stripe and chrome of editor_view_insight.cpp) and handed to the IEditorRenderer.

#include "frost/text/large_file.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include <QMetaObject>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <unordered_set>

namespace frost::ui {

namespace {

// Pixel sizes at 100 % zoom: the text's left inset, the line numbers' side padding, the marker
// column right of the numbers, and the caret's width.
constexpr double kTextPadding = 6.0;
constexpr double kNumberPadding = 8.0;
constexpr double kMarkerColumn = 14.0;
constexpr double kCaretWidth = 2.0;
// The VCS change stripe (ADR 0048): its width, its inset into the numbers' right padding, the
// deletion mark's height, and the click slack around it.
constexpr double kChangeStripeWidth = 3.0;
constexpr double kChangeStripeInset = 3.0;
constexpr double kDeletedMarkHeight = 3.0;
constexpr double kChangeStripeSlack = 3.0;

[[nodiscard]] std::uint32_t digitCount(std::uint32_t value) noexcept {
    std::uint32_t digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

// True for code points the raster runs draw nothing for: C0 controls, DEL and C1 controls.
[[nodiscard]] bool isControl(char32_t cp) noexcept { return cp < 0x20 || (cp >= 0x7F && cp < 0xA0); }

void appendCodePoint(QString& out, char32_t cp) {
    if (cp > 0xFFFF) {
        out.append(QChar(QChar::highSurrogate(cp)));
        out.append(QChar(QChar::lowSurrogate(cp)));
    } else {
        out.append(QChar(static_cast<char16_t>(cp)));
    }
}

} // namespace

// --- geometry ---

std::optional<LineIndex> EditorView::breakpointLineAt(QPointF at) const {
    const double gutter = gutterWidth();
    const double right = m_settings.lineNumbers ? gutter - kMarkerColumn * zoomFactor() : gutter;
    if (at.x() < 0.0 || at.x() >= right || at.y() < 0.0) {
        return std::nullopt;
    }
    // Below the last row is no line.
    const auto row = static_cast<std::uint64_t>(at.y() / m_lineHeight) + firstVisibleRow();
    if (row >= m_editor->display().rowCount()) {
        return std::nullopt;
    }
    return m_editor->text().lineAt(hitAt(at).offset);
}

double EditorView::gutterWidth() const noexcept {
    const double zoom = zoomFactor();
    double width = kMarkerColumn * zoom;
    if (m_settings.lineNumbers) {
        const std::uint32_t digits = std::max<std::uint32_t>(2, digitCount(m_editor->text().lineCount()));
        width += static_cast<double>(digits) * toPixels(m_shaper->cellWidth()) + 2.0 * kNumberPadding * zoom;
    }
    return std::ceil(width);
}

QRectF EditorView::changeStripeColumn() const noexcept {
    const double zoom = zoomFactor();
    // In the padding between the line numbers and the marker column, or at the gutter's left edge
    // without line numbers.
    const double x = m_settings.lineNumbers
                         ? gutterWidth() - kMarkerColumn * zoom - kNumberPadding * zoom + kChangeStripeInset * zoom
                         : zoom;
    return QRectF(x, 0.0, kChangeStripeWidth * zoom, static_cast<double>(viewport()->height()));
}

std::optional<std::size_t> EditorView::lineChangeOnLine(LineIndex line) const noexcept {
    const auto it = std::ranges::upper_bound(m_lineChanges, line, {}, &LineChangeMarker::firstLine);
    if (it == m_lineChanges.begin()) {
        return std::nullopt;
    }
    const LineChangeMarker& change = *(it - 1);
    const bool covers = change.kind == ChangeStripeKind::Deleted
                            ? change.firstLine == line
                            : line.value() < change.firstLine.value() + change.lineCount;
    if (!covers) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - 1 - m_lineChanges.begin());
}

std::optional<std::size_t> EditorView::lineChangeAt(QPointF at) const {
    if (m_lineChanges.empty() || at.x() >= gutterWidth() || at.y() < 0.0) {
        return std::nullopt;
    }
    const QRectF column = changeStripeColumn();
    const double slack = kChangeStripeSlack * zoomFactor();
    if (at.x() < column.left() - slack || at.x() > column.right() + 2.0 * slack) {
        return std::nullopt;
    }
    return lineChangeOnLine(m_editor->text().lineAt(hitAt(at).offset));
}

double EditorView::textOriginX() const noexcept {
    return gutterWidth() + kTextPadding * zoomFactor() - static_cast<double>(horizontalScrollBar()->value());
}

std::uint32_t EditorView::firstVisibleRow() const noexcept {
    return static_cast<std::uint32_t>(std::max(0, verticalScrollBar()->value()));
}

ByteOffset EditorView::offsetAt(QPointF at) const { return hitAt(at).offset; }

QRect EditorView::globalCaretRect(ByteOffset offset) const {
    const ByteOffset clamped = m_editor->text().clip(offset);
    const QRect local = caretRect(clamped, text::Bias::Right).toAlignedRect();
    return QRect(viewport()->mapToGlobal(local.topLeft()), local.size());
}

std::uint32_t EditorView::visibleRowCount() const noexcept {
    return static_cast<std::uint32_t>(std::ceil(viewport()->height() / m_lineHeight)) + 1;
}

QRectF EditorView::caretRect(ByteOffset offset, text::Bias bias) const {
    const text::DisplayMap& map = m_editor->display();
    const text::DisplayPoint point = map.toDisplayPoint(offset, bias);
    const double row = static_cast<double>(point.row.value()) - static_cast<double>(firstVisibleRow());
    const double x = textOriginX() + toPixels(map.xAt(point));
    return QRectF(x, row * m_lineHeight, std::max(1.0, kCaretWidth * zoomFactor()), m_lineHeight);
}

text::DisplayHit EditorView::hitAt(QPointF point) const {
    const text::DisplayMap& map = m_editor->display();
    const double rowOffset = std::floor(point.y() / m_lineHeight);
    const double row =
        std::clamp(static_cast<double>(firstVisibleRow()) + rowOffset, 0.0, static_cast<double>(map.rowCount() - 1));
    const text::TextWidth x = toShaperUnits(point.x() - textOriginX());
    return map.hitTest(text::DisplayRow{static_cast<std::uint32_t>(row)}, x);
}

// --- scrolling ---

void EditorView::updateScrollBars() {
    const text::DisplayMap& map = m_editor->display();
    const int fullRows = std::max(1, static_cast<int>(std::floor(viewport()->height() / m_lineHeight)));
    m_editor->setPageRows(static_cast<std::uint32_t>(std::max(1, fullRows - 1)));
    // IntelliJ's virtual space at the bottom: the last line may scroll up to the top.
    verticalScrollBar()->setRange(0, static_cast<int>(map.rowCount()) - 1);
    verticalScrollBar()->setPageStep(fullRows);
    verticalScrollBar()->setSingleStep(1);
    if (map.wrap().mode != text::WrapMode::Off) {
        horizontalScrollBar()->setRange(0, 0);
        return;
    }
    const double cell = toPixels(m_shaper->cellWidth());
    const double textWidth = viewport()->width() - gutterWidth() - stripeWidth() - kTextPadding * zoomFactor();
    const double content = m_maxRowWidth + 4.0 * cell;
    horizontalScrollBar()->setRange(0, std::max(0, static_cast<int>(std::ceil(content - textWidth))));
    horizontalScrollBar()->setPageStep(std::max(1, static_cast<int>(textWidth)));
    horizontalScrollBar()->setSingleStep(std::max(1, static_cast<int>(cell)));
}

void EditorView::ensureCaretVisible() {
    const text::DisplayMap& map = m_editor->display();
    const text::EditorState& state = m_editor->state();
    const text::Selection primary = state.primary();
    const text::CaretState caret = state.caret(state.selections.primaryIndex());
    const text::DisplayPoint point = map.toDisplayPoint(primary.head, caret.bias);
    const std::uint32_t row = point.row.value();
    const std::uint32_t first = firstVisibleRow();
    const auto fullRows =
        static_cast<std::uint32_t>(std::max(1, static_cast<int>(std::floor(viewport()->height() / m_lineHeight))));
    if (row < first) {
        verticalScrollBar()->setValue(static_cast<int>(row));
    } else if (row >= first + fullRows) {
        verticalScrollBar()->setValue(static_cast<int>(row - fullRows + 1));
    }
    if (map.wrap().mode != text::WrapMode::Off) {
        return;
    }
    const double x = toPixels(map.xAt(point));
    const double cell = toPixels(m_shaper->cellWidth());
    if (x > m_maxRowWidth) {
        m_maxRowWidth = x;
        updateScrollBars();
    }
    const double textWidth = viewport()->width() - gutterWidth() - stripeWidth() - 2.0 * kTextPadding * zoomFactor();
    const double scroll = horizontalScrollBar()->value();
    if (x < scroll) {
        horizontalScrollBar()->setValue(static_cast<int>(std::max(0.0, x - 4.0 * cell)));
    } else if (x + cell > scroll + textWidth) {
        horizontalScrollBar()->setValue(static_cast<int>(std::ceil(x + 4.0 * cell - textWidth)));
    }
}

void EditorView::scrollContentsBy(int, int) { viewport()->update(); }

void EditorView::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    updateOverlayGeometry();
    updateWrap();
    updateScrollBars();
}

// --- painting ---

EditorDisplayList EditorView::buildDisplayList() {
    EditorDisplayList list;
    const QRectF area = viewport()->rect();
    const double zoom = zoomFactor();
    const double gutter = gutterWidth();
    const double origin = textOriginX();
    const double cell = toPixels(m_shaper->cellWidth());
    list.viewport = area;
    list.background = m_colors.background;
    list.gutter = QRectF(0.0, 0.0, gutter, area.height());
    list.gutterBackground = m_colors.gutterBackground;
    list.textArea = QRectF(gutter, 0.0, std::max(0.0, area.width() - gutter - stripeWidth()), area.height());
    list.font = m_shaper->font();
    list.caretColor = m_colors.caret;

    const text::DisplayMap& map = m_editor->display();
    const text::Snapshot& snapshot = m_editor->text();
    const text::EditorState& state = m_editor->state();
    const std::uint32_t first = std::min(firstVisibleRow(), map.rowCount() - 1);
    const std::uint32_t wanted = std::min(visibleRowCount(), map.rowCount() - first);
    list.firstRow = first;

    std::vector<text::DisplayRowInfo> rows;
    rows.reserve(wanted);
    text::DisplayRowCursor cursor = map.rows(text::DisplayRow{first});
    text::DisplayRowInfo info;
    while (rows.size() < wanted && cursor.next(info)) {
        rows.push_back(info);
    }
    list.rowCount = static_cast<std::uint32_t>(rows.size());
    if (rows.empty()) {
        return list;
    }

    // The buffer bytes on screen, widened to whole lines so the highlight cache survives small
    // scrolls within them (the query is bounded by its budget either way).
    ByteOffset visibleBegin = rows.front().buffer.begin;
    ByteOffset visibleEnd = rows.back().buffer.end;
    if (const Result<text::LineInfo> line = snapshot.lineInfo(snapshot.lineAt(visibleBegin))) {
        visibleBegin = line->content.begin;
    }
    if (const Result<text::LineInfo> line = snapshot.lineInfo(snapshot.lineAt(visibleEnd))) {
        visibleEnd = line->content.end;
    }
    const std::vector<syntax::HighlightSpan> spans = highlightsFor(text::ByteRange{visibleBegin, visibleEnd});
    std::size_t spanIndex = 0;
    const auto kindAt = [&](ByteOffset at) -> std::optional<syntax::HighlightKind> {
        while (spanIndex < spans.size() && spans[spanIndex].range.end <= at) {
            ++spanIndex;
        }
        if (spanIndex < spans.size() && spans[spanIndex].range.begin <= at) {
            return spans[spanIndex].kind;
        }
        return std::nullopt;
    };
    // Semantic tokens and inactive regions, walked the same way (both sorted; rows ascend).
    constexpr std::size_t kNoSemantic = ~std::size_t{0};
    std::size_t semanticIndex = static_cast<std::size_t>(
        std::ranges::upper_bound(m_semantic, visibleBegin, {}, [](const SemanticSpan& s) { return s.range.end; }) -
        m_semantic.begin());
    const auto semanticAt = [&](ByteOffset at) -> std::size_t {
        while (semanticIndex < m_semantic.size() && m_semantic[semanticIndex].range.end <= at) {
            ++semanticIndex;
        }
        if (semanticIndex < m_semantic.size() && m_semantic[semanticIndex].range.begin <= at) {
            return semanticIndex;
        }
        return kNoSemantic;
    };
    std::size_t inactiveIndex = 0;
    const auto inactiveAt = [&](ByteOffset at) {
        while (inactiveIndex < m_inactive.size() && m_inactive[inactiveIndex].end <= at) {
            ++inactiveIndex;
        }
        return inactiveIndex < m_inactive.size() && m_inactive[inactiveIndex].begin <= at;
    };
    const std::vector<FrameDecoration> decorations = frameDecorations(text::ByteRange{visibleBegin, visibleEnd});

    // Lines holding an empty caret get the current-line background (IntelliJ shows none while the
    // caret's selection is non-empty); every caret line gets the brighter line number.
    std::unordered_set<std::uint32_t> caretLines;
    std::unordered_set<std::uint32_t> numberLines;
    for (const text::Selection& selection : state.selections.all()) {
        const std::uint32_t line = snapshot.lineAt(selection.head).value();
        numberLines.insert(line);
        if (selection.empty()) {
            caretLines.insert(line);
        }
    }

    const std::vector<text::ByteRange>& matches = searchMatches();
    const std::optional<text::ByteRange> composition = m_document->compositionRange();
    const double numbersRight = gutter - kMarkerColumn * zoom - kNumberPadding * zoom;
    const TokenFormat plain{m_colors.foreground, false};
    bool widened = false;

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const text::DisplayRowInfo& row = rows[i];
        const double top = static_cast<double>(i) * m_lineHeight;
        const double baseline = top + m_ascent;
        if (row.kind == text::RowKind::Block) {
            continue;
        }
        const std::uint32_t line = row.line.value();
        const auto xOf = [&](ByteOffset at) {
            const text::DisplayColumn column{static_cast<std::uint32_t>(at - row.buffer.begin)};
            return origin + toPixels(map.xAt(text::DisplayPoint{row.row, column}));
        };

        if (caretLines.contains(line)) {
            list.fills.push_back(
                DisplayFill{QRectF(gutter, top, list.textArea.width(), m_lineHeight), m_colors.currentLine});
        }
        if (m_executionPoint && *m_executionPoint == row.line) {
            list.fills.push_back(
                DisplayFill{QRectF(gutter, top, list.textArea.width(), m_lineHeight), m_colors.executionLine});
        }

        // Search matches on this row (sorted and non-overlapping, so also sorted by end).
        auto match = std::ranges::upper_bound(matches, row.buffer.begin, {}, &text::ByteRange::end);
        for (; match != matches.end() && match->begin < row.buffer.end; ++match) {
            const double x0 = xOf(std::max(match->begin, row.buffer.begin));
            const double x1 = xOf(std::min(match->end, row.buffer.end));
            if (x1 > x0) {
                list.fills.push_back(DisplayFill{QRectF(x0, top, x1 - x0, m_lineHeight), m_colors.searchMatch});
            }
        }

        // Selections; one that continues past a line end also covers the line break's cell.
        for (const text::Selection& selection : state.selections.all()) {
            const text::ByteRange range = selection.range();
            if (selection.empty() || range.end < row.buffer.begin || range.begin > row.buffer.end) {
                continue;
            }
            const double x0 = xOf(std::max(range.begin, row.buffer.begin));
            double x1 = xOf(std::min(range.end, row.buffer.end));
            if (range.end > row.buffer.end && row.endsLine) {
                x1 += cell;
            }
            if (x1 > x0) {
                list.fills.push_back(DisplayFill{QRectF(x0, top, x1 - x0, m_lineHeight), m_colors.selection});
            }
        }

        // The text, as runs of one highlight kind. Tabs and control characters split runs and draw
        // nothing; each non-ASCII cluster is a run of its own at the shaper's x, so wide and
        // fallback glyphs land where the display map puts carets. Past kLongLineCapBytes of a row
        // nothing is shaped or painted. Budget: B5.
        text::ByteRange shown = row.buffer;
        if (shown.size() > text::kLongLineCapBytes) {
            shown.end = snapshot.clip(shown.begin + text::kLongLineCapBytes);
        }
        const std::string bytes = snapshot.text(shown);
        const std::string_view view(bytes);
        text::TextWidth x = 0;
        std::size_t at = 0;
        QString run;
        double runX = 0.0;
        ByteOffset runBegin = shown.begin;
        std::optional<syntax::HighlightKind> runKind;
        std::size_t runSemantic = kNoSemantic;
        bool runInactive = false;
        bool runAscii = true;
        const auto flush = [&](ByteOffset end) {
            if (run.isEmpty()) {
                return;
            }
            // Inactive code is dimmed over everything; a semantic token wins over tree-sitter.
            TokenFormat format = runKind ? m_colors.format(*runKind) : plain;
            std::optional<SemanticKind> semantic;
            if (runSemantic != kNoSemantic) {
                const SemanticSpan& span = m_semantic[runSemantic];
                semantic = span.kind;
                format = m_colors.semanticFormat(span.kind, span.modifiers);
            }
            if (runInactive) {
                format = TokenFormat{m_colors.inactiveCode, false};
            }
            list.text.push_back(DisplayText{QPointF(origin + runX, baseline), run, format.color, format.italic, runKind,
                                            text::ByteRange{runBegin, end}, semantic, runInactive});
            run.clear();
        };
        while (at < view.size()) {
            const text::DecodedChar ch = text::decodeUtf8(view.substr(at));
            const ByteOffset offset = shown.begin + at;
            const bool ascii = ch.cp < 0x80;
            if (ch.cp == U'\t' || isControl(ch.cp)) {
                flush(offset);
            } else if (!ascii && !run.isEmpty() && text::charWidth(ch.cp) == text::CharWidth::Zero) {
                appendCodePoint(run, ch.cp); // A combining mark or joiner stays with its cluster.
            } else {
                const std::optional<syntax::HighlightKind> kind = kindAt(offset);
                const std::size_t semantic = semanticAt(offset);
                const bool inactive = inactiveAt(offset);
                if (!run.isEmpty() &&
                    (kind != runKind || semantic != runSemantic || inactive != runInactive || !ascii || !runAscii)) {
                    flush(offset);
                }
                if (run.isEmpty()) {
                    runX = toPixels(x);
                    runBegin = offset;
                    runKind = kind;
                    runSemantic = semantic;
                    runInactive = inactive;
                    runAscii = ascii;
                }
                appendCodePoint(run, ch.cp);
            }
            x += m_shaper->advance(ch.cp, x);
            at += ch.bytes;
        }
        flush(shown.begin + at);
        if (map.wrap().mode == text::WrapMode::Off && toPixels(x) > m_maxRowWidth) {
            m_maxRowWidth = toPixels(x);
            widened = true;
        }

        // Range decorations on this row: squiggles, strike-outs, usage boxes. An empty range (a
        // diagnostic at a point) or one ending at the line break marks one cell.
        for (const FrameDecoration& decoration : decorations) {
            const text::ByteRange range = decoration.range;
            double x0 = 0.0;
            double x1 = 0.0;
            if (range.empty()) {
                if (range.begin < row.buffer.begin || range.begin > row.buffer.end ||
                    (range.begin == row.buffer.end && !row.endsLine)) {
                    continue;
                }
                x0 = xOf(range.begin);
                x1 = x0 + cell;
            } else {
                if (range.end <= row.buffer.begin || range.begin >= row.buffer.end) {
                    continue;
                }
                x0 = xOf(std::max(range.begin, row.buffer.begin));
                x1 = xOf(std::min(range.end, row.buffer.end));
                if (x1 <= x0) {
                    x1 = x0 + cell;
                }
            }
            list.decorations.push_back(DisplayDecoration{QRectF(x0, top, x1 - x0, m_lineHeight), baseline,
                                                         decoration.style, decoration.color, std::max(1.0, zoom), range,
                                                         decoration.severity, decoration.tooltip});
        }

        // The IME preedit's underline.
        if (composition && composition->end >= row.buffer.begin && composition->begin <= row.buffer.end &&
            !composition->empty()) {
            const double x0 = xOf(std::max(composition->begin, row.buffer.begin));
            const double x1 = xOf(std::min(composition->end, row.buffer.end));
            const double y = baseline + std::max(1.0, 2.0 * zoom);
            if (x1 > x0) {
                list.overlays.push_back(DisplayLine{QLineF(x0, y, x1, y), m_colors.foreground, std::max(1.0, zoom)});
            }
        }

        // The VCS change stripe on every row of an added or modified line; a deletion mark on the
        // top edge of the first row of the line that follows deleted lines.
        if (const std::optional<std::size_t> index = lineChangeOnLine(row.line)) {
            const LineChangeMarker& change = m_lineChanges[*index];
            const QRectF column = changeStripeColumn();
            if (change.kind == ChangeStripeKind::Deleted) {
                if (row.startsLine) {
                    const double height = std::max(2.0, kDeletedMarkHeight * zoom);
                    list.changeStripes.push_back(
                        DisplayChangeStripe{QRectF(column.left(), top - height / 2.0, column.width() * 2.5, height),
                                            m_colors.changeDeleted, change.kind, row.line, *index});
                }
            } else {
                const QColor color =
                    change.kind == ChangeStripeKind::Added ? m_colors.changeAdded : m_colors.changeModified;
                list.changeStripes.push_back(DisplayChangeStripe{
                    QRectF(column.left(), top, column.width(), m_lineHeight), color, change.kind, row.line, *index});
            }
        }

        if (!row.startsLine) {
            continue;
        }
        const auto breakpoint = std::ranges::lower_bound(m_breakpoints, row.line, {}, &BreakpointMarker::line);
        const bool hasBreakpoint = breakpoint != m_breakpoints.end() && breakpoint->line == row.line;
        const bool isExecution = m_executionPoint && *m_executionPoint == row.line;
        if (hasBreakpoint || isExecution) {
            // In place of the number (IntelliJ's new UI), or in the marker column without numbers.
            const double areaRight = m_settings.lineNumbers ? gutter - kMarkerColumn * zoom : gutter;
            const double areaLeft = m_settings.lineNumbers ? 0.0 : gutter - kMarkerColumn * zoom;
            const double size = std::min(m_lineHeight, kMarkerColumn * zoom) * 0.75;
            const double cx = (areaLeft + areaRight) / 2.0;
            const double cy = top + m_lineHeight / 2.0;
            if (hasBreakpoint) {
                const QColor color =
                    breakpoint->kind == BreakpointKind::Verified ? m_colors.breakpoint : m_colors.breakpointUnverified;
                list.breakpoints.push_back(DisplayBreakpoint{QRectF(cx - size / 2.0, cy - size / 2.0, size, size),
                                                             color, breakpoint->kind, row.line});
            }
            if (isExecution) {
                const double height = size * 0.8;
                list.executionPoint = DisplayExecutionPoint{QRectF(cx - size / 2.0, cy - height / 2.0, size, height),
                                                            m_colors.executionPoint, row.line};
            }
        }
        if (m_settings.lineNumbers && !hasBreakpoint && !isExecution) {
            const QString number = QString::number(line + 1);
            const double width = static_cast<double>(number.size()) * cell;
            const QColor color = numberLines.contains(line) ? m_colors.lineNumberCurrent : m_colors.lineNumber;
            list.gutterText.push_back(DisplayText{QPointF(numbersRight - width, baseline), number, color, false,
                                                  std::nullopt, row.buffer, std::nullopt, false});
        }
        // The most severe marker of the line (Error sorts before Warning).
        const auto marker = std::ranges::lower_bound(m_markers, row.line, {}, &LineMarker::line);
        if (marker != m_markers.end() && marker->line == row.line) {
            MarkerSeverity severity = marker->severity;
            for (auto it = marker; it != m_markers.end() && it->line == row.line; ++it) {
                severity = std::min(severity, it->severity);
            }
            const double size = std::min(m_lineHeight, kMarkerColumn * zoom) * 0.6;
            const double cx = gutter - kMarkerColumn * zoom / 2.0;
            const double cy = top + m_lineHeight / 2.0;
            const QColor color = severity == MarkerSeverity::Error ? m_colors.errorMarker : m_colors.warningMarker;
            list.markers.push_back(
                DisplayMarker{QRectF(cx - size / 2.0, cy - size / 2.0, size, size), color, severity, row.line});
        }
    }

    if (m_settings.showRightMargin && map.wrap().mode == text::WrapMode::Off) {
        const double x = origin + static_cast<double>(m_settings.rightMargin) * cell;
        if (x > gutter && x < area.width()) {
            list.underlines.push_back(DisplayLine{QLineF(x, 0.0, x, area.height()), m_colors.rightMargin, 1.0});
        }
    }

    if (m_caretOn && m_focused) {
        const std::span<const text::Selection> all = state.selections.all();
        for (std::size_t i = 0; i < all.size(); ++i) {
            const QRectF rect = caretRect(all[i].head, state.caret(i).bias);
            if (rect.bottom() > 0.0 && rect.top() < area.height()) {
                list.carets.push_back(rect);
            }
        }
    }

    addInsightChrome(list);

    if (widened) {
        // The horizontal range follows the widest row seen; not changed while painting.
        QMetaObject::invokeMethod(this, [this] { updateScrollBars(); }, Qt::QueuedConnection);
    }
    return list;
}

void EditorView::paintEvent(QPaintEvent*) {
    const EditorDisplayList list = buildDisplayList();
    m_renderer->render(list, *viewport());
    ++m_paintCount;
    if (m_pendingInput && m_inputToPaint) {
        // The InputToPaint marker: input handled -> this paint ended. Budget: B5.
        m_inputToPaint(InputToPaintSample{*m_pendingInput, m_clock->steadyNow(), m_editor->text().version()});
    }
    m_pendingInput.reset();
    if (m_rendererState.pending && !m_rhiAttachQueued) {
        // The first frame was raster; the QRhi viewport replaces this one once the paint is over.
        m_rhiAttachQueued = true;
        QMetaObject::invokeMethod(
            this,
            [this] {
                m_rhiAttachQueued = false;
                if (m_rendererState.pending) {
                    attachRhi();
                }
            },
            Qt::QueuedConnection);
    }
}

} // namespace frost::ui
