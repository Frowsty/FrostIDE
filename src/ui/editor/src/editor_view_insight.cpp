// SPDX-License-Identifier: Apache-2.0
//
// EditorView (frost/ui/editor/editor_view.hpp), part 4: insight (ADR 0047). The seams the
// workbench feeds (semantic tokens, inactive regions, diagnostics, decorations, usages), how their
// ranges follow edits, F2 problem navigation and its balloon, the error stripe's geometry and
// marks, the inspection widget's layout, and the stripe and chrome parts of the display list.

#include "frost/text/anchor.hpp"
#include "frost/ui/editor/editor_view.hpp"

#include <QScrollBar>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <span>
#include <utility>

namespace frost::ui {

namespace {

// Pixel sizes at 100 % zoom: the stripe's width and the balloon's text cap.
constexpr double kStripeWidth = 11.0;
constexpr qsizetype kBalloonChars = 160;
constexpr int kSpinnerDots = 8;

// `range` through one raw batch: its start sticks to the text after it, its end to the text before
// it, so text typed at either edge stays outside; a range whose text was deleted collapses.
[[nodiscard]] text::ByteRange mapRange(text::ByteRange range, std::span<const text::RawEdit> edits) noexcept {
    const ByteOffset begin = text::mapOffset(range.begin, text::Bias::Right, edits);
    ByteOffset end = text::mapOffset(range.end, text::Bias::Left, edits);
    if (end < begin) {
        end = begin;
    }
    return text::ByteRange{begin, end};
}

// Maps every item's `range` through `edits`; drops the ones that became empty when `dropEmpty`.
template<class T>
void mapItems(std::vector<T>& items, std::span<const text::RawEdit> edits, bool dropEmpty) {
    for (T& item : items) {
        item.range = mapRange(item.range, edits);
    }
    if (dropEmpty) {
        std::erase_if(items, [](const T& item) { return item.range.empty(); });
    }
}

void mapRanges(std::vector<text::ByteRange>& ranges, std::span<const text::RawEdit> edits) {
    for (text::ByteRange& range : ranges) {
        range = mapRange(range, edits);
    }
    std::erase_if(ranges, [](const text::ByteRange& range) { return range.empty(); });
}

[[nodiscard]] bool identifierByte(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return u >= 0x80 || (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || c == '_';
}

// The first line of a message, capped for the balloon.
[[nodiscard]] QString balloonText(std::string_view message) {
    const std::size_t newline = message.find_first_of("\r\n");
    QString text = QString::fromUtf8(message.substr(0, newline == std::string_view::npos ? message.size() : newline));
    if (text.size() > kBalloonChars) {
        text = text.left(kBalloonChars - 3) + QStringLiteral("...");
    }
    return text;
}

[[nodiscard]] bool touches(text::ByteRange range, ByteOffset offset) noexcept {
    return range.begin <= offset && offset <= range.end;
}

[[nodiscard]] QColor withAlpha(QColor color, double alpha) {
    color.setAlphaF(static_cast<float>(std::clamp(alpha, 0.0, 1.0) * color.alphaF()));
    return color;
}

DisplayShape rectShape(QRectF rect, QColor fill, double radius = 0.0) {
    DisplayShape shape;
    shape.kind = DisplayShape::Kind::Rect;
    shape.rect = rect;
    shape.fill = fill;
    shape.radius = radius;
    return shape;
}

DisplayShape ellipseShape(QRectF rect, QColor fill) {
    DisplayShape shape;
    shape.kind = DisplayShape::Kind::Ellipse;
    shape.rect = rect;
    shape.fill = fill;
    return shape;
}

DisplayShape polyShape(DisplayShape::Kind kind, QPolygonF points, QColor fill, QColor stroke, double width) {
    DisplayShape shape;
    shape.kind = kind;
    shape.points = std::move(points);
    shape.fill = fill;
    shape.stroke = stroke;
    shape.strokeWidth = width;
    return shape;
}

// A point of the unit box `box` (fractions of its width and height).
[[nodiscard]] QPointF at(const QRectF& box, double fx, double fy) {
    return QPointF(box.left() + fx * box.width(), box.top() + fy * box.height());
}

// The exclamation mark of the error and warning icons, in `color`.
void exclamation(std::vector<DisplayShape>& out, const QRectF& box, QColor color, double top) {
    const double s = box.width();
    out.push_back(rectShape(QRectF(box.center().x() - 0.07 * s, box.top() + top * s, 0.14 * s, 0.34 * s), color));
    out.push_back(
        ellipseShape(QRectF(box.center().x() - 0.08 * s, box.top() + (top + 0.42) * s, 0.16 * s, 0.16 * s), color));
}

} // namespace

// --- edits ---

std::optional<std::vector<const EditorView::EditRecord*>> EditorView::editsSince(std::uint64_t version) const {
    const std::uint64_t current = m_editor->text().version();
    std::vector<const EditRecord*> out;
    if (version == current) {
        return out;
    }
    if (version > current) {
        return std::nullopt;
    }
    auto it = std::ranges::find(m_editHistory, version, &EditRecord::baseVersion);
    if (it == m_editHistory.end()) {
        return std::nullopt;
    }
    std::uint64_t running = version;
    for (; it != m_editHistory.end(); ++it) {
        if (it->baseVersion != running) {
            return std::nullopt; // A gap: a change the view did not see.
        }
        out.push_back(&*it);
        running = it->version;
    }
    if (running != current) {
        return std::nullopt;
    }
    return out;
}

void EditorView::followInsight(const text::ChangeSet& change) {
    if (change.version == change.baseVersion) {
        return;
    }
    m_editHistory.push_back(EditRecord{change.baseVersion, change.version, change.edits});
    while (m_editHistory.size() > kEditHistory) {
        m_editHistory.pop_front();
    }
    const std::span<const text::RawEdit> edits = change.edits;
    if (edits.empty()) {
        return;
    }
    mapItems(m_semantic, edits, true);
    mapRanges(m_inactive, edits);
    for (std::vector<ActiveDiagnostic>& set : m_diagnostics) {
        mapItems(set, edits, false);
    }
    mapItems(m_usages, edits, true);
    for (EditorInlayHint& hint : m_inlayHints) {
        hint.offset = text::mapOffset(hint.offset, text::Bias::Right, edits);
    }
    mapItems(m_documentLinks, edits, true);
    const text::Snapshot& snapshot = m_editor->text();
    for (EditorCodeVisionLens& lens : m_codeVisionLenses) {
        lens.symbolOffset = text::mapOffset(lens.symbolOffset, text::Bias::Right, edits);
        lens.line = snapshot.lineAt(lens.symbolOffset);
    }
    for (auto& [owner, set] : m_decorations) {
        mapItems(set, edits, true);
    }
    m_balloon.reset();
    emit documentEdited(static_cast<quint64>(change.version));
}

// --- the seams ---

bool EditorView::setSemanticTokens(std::uint64_t textVersion, std::vector<SemanticSpan> spans) {
    const std::optional<std::vector<const EditRecord*>> chain = editsSince(textVersion);
    if (!chain) {
        return false;
    }
    std::ranges::stable_sort(spans, {}, [](const SemanticSpan& span) { return span.range.begin; });
    for (const EditRecord* record : *chain) {
        mapItems(spans, record->edits, true);
    }
    std::erase_if(spans, [](const SemanticSpan& span) { return span.range.empty(); });
    m_semantic = std::move(spans);
    m_insightEnabled = true;
    insightUpdated();
    return true;
}

bool EditorView::setInactiveRanges(std::uint64_t textVersion, std::vector<text::ByteRange> ranges) {
    const std::optional<std::vector<const EditRecord*>> chain = editsSince(textVersion);
    if (!chain) {
        return false;
    }
    std::ranges::sort(ranges, {}, &text::ByteRange::begin);
    for (const EditRecord* record : *chain) {
        mapRanges(ranges, record->edits);
    }
    std::erase_if(ranges, [](const text::ByteRange& range) { return range.empty(); });
    m_inactive = std::move(ranges);
    m_insightEnabled = true;
    insightUpdated();
    return true;
}

text::ByteRange EditorView::resolvePosition(int line, int column) const {
    const text::Snapshot& snapshot = m_editor->text();
    const auto lines = static_cast<int>(snapshot.lineCount());
    const int index = std::clamp(line - 1, 0, std::max(0, lines - 1));
    const Result<text::LineInfo> info = snapshot.lineInfo(LineIndex{static_cast<std::uint32_t>(index)});
    if (!info) {
        return text::ByteRange{};
    }
    const text::ByteRange content = info->content;
    if (column <= 0) {
        return content;
    }
    const ByteOffset begin =
        snapshot.clip(std::min(content.begin + static_cast<std::uint64_t>(column - 1), content.end));
    const ByteOffset limit = std::min(content.end, begin + 256);
    const std::string bytes = snapshot.text(text::ByteRange{begin, limit});
    std::size_t length = 0;
    while (length < bytes.size() && identifierByte(bytes[length])) {
        ++length;
    }
    if (length == 0 && !bytes.empty()) {
        length = text::decodeUtf8(bytes).bytes; // One character.
    }
    return text::ByteRange{begin, snapshot.clip(begin + length)};
}

void EditorView::setDiagnostics(EditorDiagnosticSource source, std::vector<EditorDiagnostic> diagnostics) {
    const text::Snapshot& snapshot = m_editor->text();
    const std::uint64_t current = snapshot.version();
    std::vector<ActiveDiagnostic> resolved;
    resolved.reserve(diagnostics.size());
    for (EditorDiagnostic& diagnostic : diagnostics) {
        std::optional<text::ByteRange> range;
        if (diagnostic.range) {
            text::ByteRange mapped{snapshot.clip(std::min(diagnostic.range->begin, snapshot.size())),
                                   snapshot.clip(std::min(diagnostic.range->end, snapshot.size()))};
            if (!diagnostic.textVersion || *diagnostic.textVersion == current) {
                range = mapped;
            } else if (const auto chain = editsSince(*diagnostic.textVersion)) {
                mapped = *diagnostic.range;
                for (const EditRecord* record : *chain) {
                    mapped = mapRange(mapped, record->edits);
                }
                range = text::ByteRange{snapshot.clip(std::min(mapped.begin, snapshot.size())),
                                        snapshot.clip(std::min(mapped.end, snapshot.size()))};
            }
        }
        if (!range) {
            range = resolvePosition(diagnostic.line, diagnostic.column);
        }
        resolved.push_back(ActiveDiagnostic{*range, diagnostic.severity, source, std::move(diagnostic.message),
                                            std::move(diagnostic.code)});
    }
    std::ranges::stable_sort(resolved, [](const ActiveDiagnostic& a, const ActiveDiagnostic& b) {
        return std::pair(a.range.begin, a.severity) < std::pair(b.range.begin, b.severity);
    });
    m_diagnostics[static_cast<std::size_t>(source)] = std::move(resolved);
    m_insightEnabled = true;
    insightUpdated();
}

std::vector<ActiveDiagnostic> EditorView::diagnostics() const {
    const text::Snapshot& snapshot = m_editor->text();
    const std::vector<ActiveDiagnostic>& live = m_diagnostics[static_cast<std::size_t>(EditorDiagnosticSource::Live)];
    std::vector<ActiveDiagnostic> out = live;
    for (const ActiveDiagnostic& build : m_diagnostics[static_cast<std::size_t>(EditorDiagnosticSource::Build)]) {
        const LineIndex line = snapshot.lineAt(build.range.begin);
        const bool duplicate = std::ranges::any_of(live, [&](const ActiveDiagnostic& d) {
            return d.message == build.message && snapshot.lineAt(d.range.begin) == line;
        });
        if (!duplicate) {
            out.push_back(build);
        }
    }
    std::ranges::stable_sort(out, [](const ActiveDiagnostic& a, const ActiveDiagnostic& b) {
        return std::pair(a.range.begin, a.severity) < std::pair(b.range.begin, b.severity);
    });
    return out;
}

void EditorView::setDecorations(std::string owner, std::vector<EditorDecoration> decorations) {
    if (decorations.empty()) {
        m_decorations.erase(owner);
    } else {
        m_decorations.insert_or_assign(std::move(owner), std::move(decorations));
    }
    viewport()->update();
}

std::vector<EditorDecoration> EditorView::decorations() const {
    std::vector<EditorDecoration> out;
    for (const auto& [owner, set] : m_decorations) {
        out.insert(out.end(), set.begin(), set.end());
    }
    return out;
}

void EditorView::setUsageHighlights(std::vector<UsageHighlight> usages) {
    std::ranges::sort(usages, {}, [](const UsageHighlight& usage) { return usage.range.begin; });
    m_usages = std::move(usages);
    viewport()->update();
}

bool EditorView::setInlayHints(std::uint64_t textVersion, std::vector<EditorInlayHint> hints) {
    const std::optional<std::vector<const EditRecord*>> chain = editsSince(textVersion);
    if (!chain) {
        return false;
    }
    for (const EditRecord* record : *chain) {
        for (EditorInlayHint& hint : hints) {
            hint.offset = text::mapOffset(hint.offset, text::Bias::Right, record->edits);
        }
    }
    const ByteOffset size = m_editor->text().size();
    for (EditorInlayHint& hint : hints) {
        hint.offset = m_editor->text().clip(std::min(hint.offset, size));
    }
    std::erase_if(hints, [](const EditorInlayHint& hint) { return hint.label.empty(); });
    std::ranges::stable_sort(hints, {}, &EditorInlayHint::offset);
    m_inlayHints = std::move(hints);
    viewport()->update();
    return true;
}

bool EditorView::setDocumentLinks(std::uint64_t textVersion, std::vector<EditorDocumentLink> links) {
    const std::optional<std::vector<const EditRecord*>> chain = editsSince(textVersion);
    if (!chain) {
        return false;
    }
    std::erase_if(links, [](const EditorDocumentLink& link) { return link.target.empty(); });
    std::ranges::stable_sort(links, {}, [](const EditorDocumentLink& link) { return link.range.begin; });
    for (const EditRecord* record : *chain) {
        mapItems(links, record->edits, true);
    }
    std::erase_if(links, [](const EditorDocumentLink& link) { return link.range.empty(); });
    m_documentLinks = std::move(links);
    return true;
}

std::optional<EditorDocumentLink> EditorView::documentLinkAt(ByteOffset offset) const {
    // Links never overlap (one per #include line), so the last one starting at or before `offset`
    // is the only candidate.
    const auto after = std::ranges::upper_bound(m_documentLinks, offset, {},
                                                [](const EditorDocumentLink& link) { return link.range.begin; });
    if (after == m_documentLinks.begin()) {
        return std::nullopt;
    }
    const EditorDocumentLink& link = *std::prev(after);
    if (offset < link.range.end) {
        return link;
    }
    return std::nullopt;
}

void EditorView::setCodeVisionLenses(std::vector<EditorCodeVisionLens> lenses) {
    const text::Snapshot& snapshot = m_editor->text();
    const ByteOffset size = snapshot.size();
    const std::uint32_t maxLine = snapshot.lineCount() > 0 ? snapshot.lineCount() - 1 : 0;
    for (EditorCodeVisionLens& lens : lenses) {
        lens.symbolOffset = snapshot.clip(std::min(lens.symbolOffset, size));
        lens.line = LineIndex{std::min(lens.line.value(), maxLine)};
    }
    std::erase_if(lenses, [](const EditorCodeVisionLens& lens) { return lens.label.empty(); });
    std::ranges::stable_sort(lenses, {}, &EditorCodeVisionLens::line);
    m_codeVisionLenses = std::move(lenses);
    viewport()->update();
}

void EditorView::setInlineDebuggerValues(std::vector<EditorInlineValue> values) {
    const std::uint32_t maxLine = m_editor->text().lineCount() > 0 ? m_editor->text().lineCount() - 1 : 0;
    for (EditorInlineValue& val : values) {
        val.line = LineIndex{std::min(val.line.value(), maxLine)};
    }
    std::erase_if(values, [](const EditorInlineValue& val) { return val.text.empty(); });
    std::ranges::stable_sort(values, {}, &EditorInlineValue::line);
    if (m_inlineDebuggerValues == values) {
        return;
    }
    m_inlineDebuggerValues = std::move(values);
    viewport()->update();
}

void EditorView::setDebugHoverProvider(DebugHoverProvider provider) { m_debugHoverProvider = std::move(provider); }

void EditorView::setAnalyzing(bool analyzing) {
    m_analyzing = analyzing;
    m_insightEnabled = true;
    insightUpdated();
}

void EditorView::setInsightEnabled(bool enabled) {
    m_insightEnabled = enabled;
    insightUpdated();
}

InsightSummary EditorView::insightSummary() const {
    InsightSummary summary;
    for (const ActiveDiagnostic& diagnostic : diagnostics()) {
        ++summary.counts[static_cast<std::size_t>(diagnostic.severity)];
    }
    summary.analyzing = m_analyzing;
    return summary;
}

void EditorView::insightUpdated() {
    if (m_insightEnabled && m_analyzing) {
        if (!m_spinner.isActive()) {
            m_spinner.start();
        }
    } else {
        m_spinner.stop();
    }
    viewport()->update();
    const InsightSummary now = insightSummary();
    if (now != m_lastSummary || m_insightEnabled != m_lastInsightEnabled) {
        m_lastSummary = now;
        m_lastInsightEnabled = m_insightEnabled;
        emit insightChanged();
    }
}

// --- F2 ---

bool EditorView::gotoNextProblem(bool forward) {
    const std::vector<ActiveDiagnostic> all = diagnostics();
    if (all.empty()) {
        m_balloon.reset();
        viewport()->update();
        return false;
    }
    const InsightSeverity worst =
        std::ranges::min(all, {}, [](const ActiveDiagnostic& d) { return d.severity; }).severity;
    std::vector<const ActiveDiagnostic*> candidates;
    for (const ActiveDiagnostic& diagnostic : all) {
        if (diagnostic.severity == worst) {
            candidates.push_back(&diagnostic);
        }
    }
    const ByteOffset caret = m_editor->state().primary().head;
    const ActiveDiagnostic* target = nullptr;
    if (forward) {
        const auto it =
            std::ranges::find_if(candidates, [&](const ActiveDiagnostic* d) { return d->range.begin > caret; });
        target = it != candidates.end() ? *it : candidates.front();
    } else {
        const auto it = std::ranges::find_if(candidates.rbegin(), candidates.rend(),
                                             [&](const ActiveDiagnostic* d) { return d->range.begin < caret; });
        target = it != candidates.rend() ? *it : candidates.back();
    }
    const ActiveDiagnostic chosen = *target;
    setState(text::EditorState::caretAt(chosen.range.begin));
    m_balloon = Balloon{chosen.range, chosen.severity, balloonText(chosen.message), chosen.range.begin};
    viewport()->update();
    return true;
}

QString EditorView::problemBalloonText() const { return m_balloon ? m_balloon->text : QString(); }

double EditorView::stripeWidth() const noexcept { return std::ceil(kStripeWidth * zoomFactor()); }

// --- the stripe ---

EditorView::StripeGeometry EditorView::stripeGeometry() const {
    StripeGeometry geometry;
    const double width = stripeWidth();
    const QRectF area = viewport()->rect();
    if (width <= 0.0 || area.width() <= width + gutterWidth()) {
        return geometry;
    }
    const double zoom = zoomFactor();
    geometry.stripe = QRectF(area.width() - width, 0.0, width, area.height());
    const double side = std::max(4.0, width - 4.0 * zoom);
    geometry.summary = QRectF(geometry.stripe.left() + (width - side) / 2.0, 2.0 * zoom, side, side);
    geometry.top = geometry.summary.bottom() + 3.0 * zoom;
    const double bottom = std::max(geometry.top + 1.0, area.height() - 3.0 * zoom);
    const double rows = std::max(1.0, static_cast<double>(m_editor->display().rowCount()));
    // Short documents keep their rows' real spacing; long ones are scaled to the stripe's height.
    geometry.scale = std::min(m_lineHeight, (bottom - geometry.top) / rows);
    geometry.markHeight = std::max(2.0 * zoom, std::min(geometry.scale, 3.0 * zoom));
    return geometry;
}

std::vector<EditorView::StripeEntry> EditorView::stripeEntries() const {
    const text::DisplayMap& map = m_editor->display();
    const auto rowOf = [&](ByteOffset offset) { return map.toDisplayPoint(offset, text::Bias::Left).row.value(); };
    std::vector<StripeEntry> out;
    for (const UsageHighlight& usage : m_usages) {
        out.push_back(StripeEntry{usage.write ? StripeMarkKind::WriteUsage : StripeMarkKind::Usage, usage.range,
                                  rowOf(usage.range.begin),
                                  usage.write ? QStringLiteral("Write usage") : QStringLiteral("Usage")});
    }
    if (findBarVisible() && m_matchesVersion == m_editor->text().version()) {
        for (const text::ByteRange& match : m_matches) {
            out.push_back(
                StripeEntry{StripeMarkKind::SearchMatch, match, rowOf(match.begin), QStringLiteral("Search result")});
        }
    }
    for (const ActiveDiagnostic& diagnostic : diagnostics()) {
        out.push_back(StripeEntry{stripeKindFor(diagnostic.severity), diagnostic.range, rowOf(diagnostic.range.begin),
                                  QString::fromStdString(diagnostic.message)});
    }
    std::ranges::stable_sort(out, {}, &StripeEntry::kind); // Painted lowest kind first.
    return out;
}

std::vector<EditorView::StripeEntry> EditorView::stripeEntriesAt(double y) const {
    const StripeGeometry geometry = stripeGeometry();
    std::vector<StripeEntry> out;
    if (geometry.stripe.isEmpty()) {
        return out;
    }
    const double slack = 2.0 * zoomFactor();
    for (StripeEntry& entry : stripeEntries()) {
        const double top = geometry.top + static_cast<double>(entry.row) * geometry.scale;
        if (y >= top - slack && y <= top + geometry.markHeight + slack) {
            out.push_back(std::move(entry));
        }
    }
    std::ranges::stable_sort(out, std::greater<>{}, &StripeEntry::kind);
    return out;
}

// --- the inspection widget ---

EditorView::InspectionLayout EditorView::inspectionLayout() const {
    InspectionLayout layout;
    if (!m_insightEnabled) {
        return layout;
    }
    const InsightSummary summary = insightSummary();
    const double zoom = zoomFactor();
    const double icon = std::round(std::min(m_lineHeight * 0.8, 14.0 * zoom));
    const double cell = toPixels(m_shaper->cellWidth());
    const double pad = 5.0 * zoom;
    const double gap = 6.0 * zoom;
    const double inner = 3.0 * zoom;
    const double height = std::ceil(std::max(m_lineHeight, icon) + 4.0 * zoom);
    const bool problems = summary.total() > 0;
    const bool badge = summary.analyzing || !problems;

    double width = 2.0 * pad;
    int pieces = 0;
    if (badge) {
        width += icon;
        ++pieces;
    }
    for (std::size_t i = 0; i < kInsightSeverityCount; ++i) {
        if (summary.counts[i] > 0) {
            width += icon + inner + static_cast<double>(QString::number(summary.counts[i]).size()) * cell;
            ++pieces;
        }
    }
    if (problems) {
        width += 2.0 * icon + inner;
        ++pieces;
    }
    width += gap * static_cast<double>(std::max(0, pieces - 1));

    const double right = viewport()->width() - stripeWidth() - 6.0 * zoom;
    if (right - width < gutterWidth() + 20.0 * zoom) {
        return layout; // Too narrow to show it.
    }
    layout.rect = QRectF(right - width, 4.0 * zoom, width, height);
    layout.state = problems ? InspectionState::Problems
                            : (summary.analyzing ? InspectionState::Analyzing : InspectionState::Clean);
    const double cy = layout.rect.center().y();
    const double baseline = layout.rect.top() + (height - m_lineHeight) / 2.0 + m_ascent;
    double x = layout.rect.left() + pad;
    if (badge) {
        layout.badge = QRectF(x, cy - icon / 2.0, icon, icon);
        x += icon + gap;
    }
    for (std::size_t i = 0; i < kInsightSeverityCount; ++i) {
        if (summary.counts[i] == 0) {
            continue;
        }
        InspectionItem item;
        item.severity = static_cast<InsightSeverity>(i);
        item.icon = QRectF(x, cy - icon / 2.0, icon, icon);
        item.count = QString::number(summary.counts[i]);
        item.textOrigin = QPointF(x + icon + inner, baseline);
        x += icon + inner + static_cast<double>(item.count.size()) * cell + gap;
        layout.items.push_back(std::move(item));
    }
    if (problems) {
        layout.previous = QRectF(x, cy - icon / 2.0, icon, icon);
        layout.next = QRectF(x + icon + inner, cy - icon / 2.0, icon, icon);
    }
    return layout;
}

// --- tooltips and clicks ---

bool EditorView::insightContains(QPointF at) const {
    const StripeGeometry geometry = stripeGeometry();
    if (!geometry.stripe.isEmpty() && geometry.stripe.contains(at)) {
        return true;
    }
    const InspectionLayout layout = inspectionLayout();
    return layout.state != InspectionState::Hidden && layout.rect.contains(at);
}

QString EditorView::toolTipAt(QPointF at) const {
    if (at.x() < gutterWidth()) {
        return lineMarkerToolTip(at);
    }
    const StripeGeometry geometry = stripeGeometry();
    if (!geometry.stripe.isEmpty() && geometry.stripe.contains(at)) {
        if (m_insightEnabled && geometry.summary.adjusted(-2.0, -2.0, 2.0, 2.0).contains(at)) {
            return QString::fromStdString(describe(insightSummary()));
        }
        QStringList messages;
        for (const StripeEntry& entry : stripeEntriesAt(at.y())) {
            if (!entry.message.isEmpty() && !messages.contains(entry.message) && messages.size() < 10) {
                messages.push_back(entry.message);
            }
        }
        return messages.join(QLatin1Char('\n'));
    }
    const InspectionLayout layout = inspectionLayout();
    if (layout.state != InspectionState::Hidden && layout.rect.contains(at)) {
        if (layout.previous.contains(at)) {
            return QStringLiteral("Previous Highlighted Error (Shift+F2)");
        }
        if (layout.next.contains(at)) {
            return QStringLiteral("Next Highlighted Error (F2)");
        }
        return QString::fromStdString(describe(insightSummary()));
    }
    const auto hit = hitAt(at);
    const auto row = m_editor->display().rowInfo(hit.point.row);
    if (const std::optional<EditorCodeVisionLens> lens = codeVisionLensAt(at)) {
        if (!lens->tooltip.empty()) {
            return QString::fromStdString(lens->tooltip);
        }
        return QString::fromStdString(lens->label);
    }
    for (const auto& segment : row.segments) {
        const auto column = hit.point.column.value();
        if (segment.kind == text::SegmentKind::FoldPlaceholder && column >= segment.column.value() &&
            column <= segment.column.value() + segment.text.size()) {
            const auto end = std::min(segment.buffer.end, segment.buffer.begin + 2048);
            return QString::fromStdString(m_editor->text().text({segment.buffer.begin, m_editor->text().clip(end)}));
        }
    }
    const ByteOffset offset = hit.offset;
    QStringList messages;
    if (m_debugHoverProvider) {
        text::ByteRange exprRange = wordAt(offset);
        const text::Selection primary = m_editor->state().primary();
        if (!primary.empty() && primary.range().begin <= offset && offset <= primary.range().end &&
            primary.range().size() <= 512) {
            exprRange = primary.range();
        }
        if (!exprRange.empty() && exprRange.size() <= 512) {
            const std::string expr = m_editor->text().text(exprRange);
            if (!expr.empty()) {
                const QString debugTip = m_debugHoverProvider(expr, m_editor->text().lineAt(offset));
                if (!debugTip.isEmpty()) {
                    messages.push_back(debugTip);
                }
            }
        }
    }
    if (const std::optional<EditorDocumentLink> link = documentLinkAt(offset)) {
        messages.push_back(QString::fromStdString(link->tooltip.empty() ? link->target : link->tooltip) +
                           QStringLiteral(" (Ctrl+Click to open)"));
    }
    for (const EditorInlayHint& hint : m_inlayHints) {
        if (!hint.tooltip.empty() && hint.offset == offset) {
            messages.push_back(QString::fromStdString(hint.tooltip));
        }
    }
    for (const ActiveDiagnostic& diagnostic : diagnostics()) {
        if (touches(diagnostic.range, offset)) {
            messages.push_back(QString::fromStdString(diagnostic.message));
        }
    }
    for (const auto& [owner, set] : m_decorations) {
        for (const EditorDecoration& decoration : set) {
            if (!decoration.tooltip.empty() && touches(decoration.range, offset)) {
                messages.push_back(QString::fromStdString(decoration.tooltip));
            }
        }
    }
    messages.removeDuplicates();
    return messages.join(QLatin1Char('\n'));
}

bool EditorView::clickInsight(QPointF at) {
    const StripeGeometry geometry = stripeGeometry();
    const auto centreRow = [this](std::uint32_t row) {
        const int fullRows = std::max(1, static_cast<int>(std::floor(viewport()->height() / m_lineHeight)));
        verticalScrollBar()->setValue(std::max(0, static_cast<int>(row) - fullRows / 2));
    };
    if (!geometry.stripe.isEmpty() && geometry.stripe.contains(at)) {
        if (m_insightEnabled && geometry.summary.adjusted(-2.0, -2.0, 2.0, 2.0).contains(at)) {
            (void)gotoNextProblem(true);
            return true;
        }
        const std::vector<StripeEntry> hits = stripeEntriesAt(at.y());
        if (!hits.empty()) {
            const StripeEntry& entry = hits.front();
            setState(text::EditorState::caretAt(entry.range.begin));
            centreRow(entry.row);
            if (entry.kind >= StripeMarkKind::Info) {
                const auto severity = static_cast<InsightSeverity>(static_cast<int>(StripeMarkKind::Error) -
                                                                   static_cast<int>(entry.kind));
                m_balloon = Balloon{entry.range, severity, balloonText(entry.message.toStdString()), entry.range.begin};
                viewport()->update();
            }
            return true;
        }
        // No mark there: scroll the document to that place (IntelliJ).
        const double row = std::max(0.0, (at.y() - geometry.top) / geometry.scale);
        centreRow(static_cast<std::uint32_t>(std::min(row, static_cast<double>(m_editor->display().rowCount()))));
        return true;
    }
    const InspectionLayout layout = inspectionLayout();
    if (layout.state != InspectionState::Hidden && layout.rect.contains(at)) {
        if (layout.previous.contains(at)) {
            (void)gotoNextProblem(false);
        } else if (layout.next.contains(at)) {
            (void)gotoNextProblem(true);
        } else {
            emit problemsViewRequested();
        }
        return true;
    }
    return false;
}

// --- the display list ---

std::vector<EditorView::FrameDecoration> EditorView::frameDecorations(text::ByteRange visible) const {
    std::vector<FrameDecoration> out;
    const auto overlaps = [&](text::ByteRange range) {
        return range.end >= visible.begin && range.begin <= visible.end;
    };
    for (const UsageHighlight& usage : m_usages) {
        if (overlaps(usage.range)) {
            out.push_back(FrameDecoration{usage.range,
                                          DecorationStyle::Background,
                                          usage.write ? m_colors.writeUsage : m_colors.usage,
                                          std::nullopt,
                                          {}});
        }
    }
    for (const SemanticSpan& span : m_semantic) {
        if ((span.modifiers & semantic_modifiers::kDeprecated) != 0 && overlaps(span.range)) {
            out.push_back(FrameDecoration{span.range, DecorationStyle::Strikeout,
                                          m_colors.semanticFormat(span.kind, span.modifiers).color, std::nullopt,
                                          QStringLiteral("Deprecated")});
        }
    }
    for (const auto& [owner, set] : m_decorations) {
        for (const EditorDecoration& decoration : set) {
            if (overlaps(decoration.range)) {
                out.push_back(FrameDecoration{decoration.range, decoration.style,
                                              decoration.severity ? m_colors.severityColor(*decoration.severity)
                                                                  : decoration.color,
                                              decoration.severity, QString::fromStdString(decoration.tooltip)});
            }
        }
    }
    // Diagnostics last, so a squiggle paints over other underlines.
    std::vector<ActiveDiagnostic> problems = diagnostics();
    std::ranges::stable_sort(problems, std::greater<>{}, &ActiveDiagnostic::severity); // Errors on top.
    for (const ActiveDiagnostic& diagnostic : problems) {
        if (!overlaps(diagnostic.range)) {
            continue;
        }
        const bool wavy =
            diagnostic.severity == InsightSeverity::Error || diagnostic.severity == InsightSeverity::Warning;
        out.push_back(FrameDecoration{diagnostic.range,
                                      wavy ? DecorationStyle::WavyUnderline : DecorationStyle::DottedUnderline,
                                      m_colors.severityColor(diagnostic.severity), diagnostic.severity,
                                      QString::fromStdString(diagnostic.message)});
    }
    return out;
}

void EditorView::addInsightChrome(EditorDisplayList& list) const {
    const double zoom = zoomFactor();
    const InsightSummary summary = insightSummary();

    // The error stripe: one mark per kind and pixel row, the summary box on top.
    const StripeGeometry geometry = stripeGeometry();
    if (!geometry.stripe.isEmpty()) {
        list.stripe = geometry.stripe;
        list.stripeBackground = m_colors.stripeBackground;
        const text::Snapshot& snapshot = m_editor->text();
        std::set<std::pair<int, int>> seen;
        for (const StripeEntry& entry : stripeEntries()) {
            const double y = geometry.top + static_cast<double>(entry.row) * geometry.scale;
            if (!seen.emplace(static_cast<int>(entry.kind), static_cast<int>(std::floor(y))).second) {
                continue;
            }
            const bool problem = entry.kind >= StripeMarkKind::Info;
            const double x0 = geometry.stripe.left() + 2.0 * zoom;
            const double x1 = problem ? geometry.stripe.right() - 2.0 * zoom : geometry.stripe.center().x();
            list.stripeMarks.push_back(DisplayStripeMark{QRectF(x0, y, std::max(1.0, x1 - x0), geometry.markHeight),
                                                         m_colors.stripeColor(entry.kind), entry.kind, entry.range,
                                                         snapshot.lineAt(entry.range.begin)});
        }
        if (m_insightEnabled) {
            list.stripeSummary = geometry.summary;
            const std::optional<InsightSeverity> worst = summary.worst();
            list.stripeSummaryColor = worst
                                          ? m_colors.severityColor(*worst)
                                          : (summary.analyzing ? m_colors.inspectionForeground : m_colors.cleanMarker);
        }
    }

    // The inspection widget.
    const InspectionLayout layout = inspectionLayout();
    list.inspection = DisplayInspection{layout.state, layout.rect, summary, layout.previous, layout.next};
    if (layout.state != InspectionState::Hidden) {
        const QColor background = m_colors.inspectionBackground;
        const QColor foreground = m_colors.inspectionForeground;
        DisplayShape pill = rectShape(layout.rect, withAlpha(background, 0.94), layout.rect.height() * 0.5);
        pill.stroke = withAlpha(foreground, 0.18);
        pill.strokeWidth = std::max(1.0, zoom);
        list.chrome.push_back(pill);
        if (!layout.badge.isEmpty()) {
            const QRectF& box = layout.badge;
            if (summary.analyzing) {
                // A spinner of dots, the brightest at the current phase.
                const double radius = box.width() * 0.36;
                const double dot = box.width() * 0.11;
                for (int i = 0; i < kSpinnerDots; ++i) {
                    const double angle = 2.0 * std::numbers::pi * i / kSpinnerDots;
                    const QPointF centre(box.center().x() + radius * std::sin(angle),
                                         box.center().y() - radius * std::cos(angle));
                    const int age = (m_spinnerPhase - i + kSpinnerDots) % kSpinnerDots;
                    const double alpha = 1.0 - 0.8 * age / (kSpinnerDots - 1);
                    list.chrome.push_back(ellipseShape(QRectF(centre.x() - dot, centre.y() - dot, 2.0 * dot, 2.0 * dot),
                                                       withAlpha(foreground, alpha)));
                }
            } else {
                list.chrome.push_back(
                    polyShape(DisplayShape::Kind::Polyline,
                              QPolygonF({at(box, 0.15, 0.55), at(box, 0.40, 0.80), at(box, 0.85, 0.25)}), QColor(),
                              m_colors.cleanMarker, std::max(1.5, 2.0 * zoom)));
            }
        }
        for (const InspectionItem& item : layout.items) {
            const QRectF& box = item.icon;
            const QColor color = m_colors.severityColor(item.severity);
            switch (item.severity) {
            case InsightSeverity::Error:
                list.chrome.push_back(ellipseShape(box, color));
                exclamation(list.chrome, box, background, 0.18);
                break;
            case InsightSeverity::Warning:
                list.chrome.push_back(polyShape(DisplayShape::Kind::Polygon,
                                                QPolygonF({at(box, 0.5, 0.06), at(box, 0.97, 0.9), at(box, 0.03, 0.9)}),
                                                color, QColor(), 1.0));
                exclamation(list.chrome, box, background, 0.28);
                break;
            case InsightSeverity::WeakWarning:
                list.chrome.push_back(
                    polyShape(DisplayShape::Kind::Polygon,
                              QPolygonF({at(box, 0.5, 0.1), at(box, 0.93, 0.86), at(box, 0.07, 0.86)}), QColor(), color,
                              std::max(1.0, 1.5 * zoom)));
                break;
            case InsightSeverity::Info: list.chrome.push_back(ellipseShape(box, color)); break;
            }
            list.chromeText.push_back(DisplayText{item.textOrigin, item.count, foreground, false, std::nullopt,
                                                  text::ByteRange{}, std::nullopt, false});
        }
        if (!layout.previous.isEmpty()) {
            const double stroke = std::max(1.0, 1.5 * zoom);
            list.chrome.push_back(polyShape(DisplayShape::Kind::Polyline,
                                            QPolygonF({at(layout.previous, 0.2, 0.65), at(layout.previous, 0.5, 0.35),
                                                       at(layout.previous, 0.8, 0.65)}),
                                            QColor(), foreground, stroke));
            list.chrome.push_back(polyShape(
                DisplayShape::Kind::Polyline,
                QPolygonF({at(layout.next, 0.2, 0.35), at(layout.next, 0.5, 0.65), at(layout.next, 0.8, 0.35)}),
                QColor(), foreground, stroke));
        }
    }

    // The F2 balloon below the caret's line (above it near the bottom).
    if (m_balloon && !m_balloon->text.isEmpty()) {
        const QRectF caret = caretRect(m_balloon->caret, text::Bias::Left);
        const double cell = toPixels(m_shaper->cellWidth());
        const double pad = 6.0 * zoom;
        const double width = static_cast<double>(m_balloon->text.size()) * cell + 2.0 * pad;
        const double height = m_lineHeight + 6.0 * zoom;
        const double x = std::clamp(caret.left(), list.textArea.left() + 2.0,
                                    std::max(list.textArea.left() + 2.0, list.textArea.right() - width - 2.0));
        double y = caret.bottom() + 2.0 * zoom;
        if (y + height > list.viewport.height()) {
            y = caret.top() - height - 2.0 * zoom;
        }
        const QRectF box(x, y, width, height);
        DisplayShape frame = rectShape(box, m_colors.inspectionBackground, 3.0 * zoom);
        frame.stroke = m_colors.severityColor(m_balloon->severity);
        frame.strokeWidth = std::max(1.0, zoom);
        list.chrome.push_back(frame);
        list.chromeText.push_back(DisplayText{QPointF(x + pad, y + 3.0 * zoom + m_ascent), m_balloon->text,
                                              m_colors.foreground, false, std::nullopt, text::ByteRange{}, std::nullopt,
                                              false});
        list.balloon = DisplayBalloon{box, m_balloon->text, m_balloon->severity};
    }
}

} // namespace frost::ui
