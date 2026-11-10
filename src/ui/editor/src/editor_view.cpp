// SPDX-License-Identifier: Apache-2.0
//
// EditorView (frost/ui/editor/editor_view.hpp): construction, settings and appearance, post-change
// model updates, status, and syntax/fold queries. Commands, folding, clipboard, find/replace, and
// Go to Line are in editor_view_commands.cpp; painting in editor_view_paint.cpp; input events in
// editor_view_input.cpp.

#include "frost/sched/task.hpp"
#include "frost/syntax/syntax_tree.hpp"
#include "frost/text/editor/unicode.hpp"
#include "frost/ui/appearance_manager.hpp"
#include "frost/ui/editor/find_bar.hpp"
#include "frost/ui/editor/go_to_line.hpp"
#include "frost/ui/frost_style.hpp"

#include "editor_view_internal.hpp"
#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <mutex>
#include <utility>

namespace frost::ui {

struct FoldQueryJob {
    std::mutex mutex;
    std::stop_source stop;
    syntax::SyntaxTree tree;
    std::optional<std::vector<text::ByteRange>> ranges;
    std::vector<std::uint64_t> maxEnd;
    bool done = false;
};

namespace {

namespace commands = text::commands;

sched::Task<void> queryFolds(std::shared_ptr<FoldQueryJob> job) {
    syntax::QueryLimits limits;
    limits.budget = std::chrono::seconds(2);
    limits.stop = job->stop.get_token();
    const auto result = job->tree.foldRanges(limits);
    std::vector<text::ByteRange> candidates;
    if (result.complete) {
        const auto snapshot = job->tree.text();
        for (const auto range : result.ranges) {
            if (limits.stop.stop_requested())
                break;
            const auto first = snapshot.lineInfo(snapshot.lineAt(range.begin));
            const auto last = snapshot.lineInfo(snapshot.lineAt(range.end));
            if (first && last && first->content.end < last->content.begin)
                candidates.push_back({first->content.end, last->content.begin});
        }
        (void)orderCandidates(candidates, limits.stop);
    }
    std::vector<std::uint64_t> maxEnd;
    if (!candidates.empty() && !limits.stop.stop_requested()) {
        maxEnd.resize(candidates.size() * 4);
        (void)buildCandidateMax(candidates, maxEnd, 1, 0, candidates.size(), limits.stop);
    }
    {
        const std::lock_guard lock(job->mutex);
        if (result.complete && !limits.stop.stop_requested()) {
            job->ranges = std::move(candidates);
            job->maxEnd = std::move(maxEnd);
        }
        job->done = true;
    }
    co_return;
}

// The syntax session's wake-up, called on a worker (or the SimExecutor's owner) after a parse or a
// query compilation: it only queues pollSyntax() on the view's thread. The session detaches the
// hook under its mutex before it is destroyed, and ~EditorView destroys the session first.
void wakeSyntax(void* context) noexcept {
    auto* view = static_cast<EditorView*>(context);
    QMetaObject::invokeMethod(view, [view] { (void)view->pollSyntax(); }, Qt::QueuedConnection);
}

} // namespace

EditorView::EditorView(text::Document& document, std::string path, EditorServices services, EditorViewSettings settings,
                       QWidget* parent)
    : QAbstractScrollArea(parent), m_document(&document), m_path(std::move(path)), m_services(std::move(services)),
      m_clock(m_services.clock != nullptr ? m_services.clock : &systemClock()), m_settings(std::move(settings)),
      m_colors(EditorColors::defaults()), m_language(&text::languageForPath(m_path)) {
    setObjectName(QStringLiteral("frostEditorView"));
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    configureViewport();

    if (m_services.appearance != nullptr) {
        // Registers JetBrains Mono on the first editor (ADR 0015 decision 9); a failure leaves the
        // font to Qt's fallback, which still draws a monospace face.
        (void)m_services.appearance->ensureEditorFonts();
        connect(m_services.appearance, &AppearanceManager::appearanceChanged, this, [this] { followAppearance(); });
    }
    followAppearance();

    rebuildFont(); // Creates the shaper; the editor and the renderer do not exist yet.
    text::EditorConfig config;
    config.settings = modelSettings(m_settings);
    config.language = m_language;
    m_editor = std::make_unique<text::Editor>(document, *m_shaper, config);
    m_foldTimer.setInterval(20);
    connect(&m_foldTimer, &QTimer::timeout, this, [this] { collectFoldQuery(); });
    m_renderer = std::make_unique<RasterEditorRenderer>(m_shaper->font());
    updateMetrics();

    if (m_services.languages != nullptr && m_services.scheduler != nullptr) {
        syntax::SyntaxOptions options = m_services.syntax;
        options.onParseReady = &wakeSyntax;
        options.onParseReadyContext = this;
        m_syntax =
            std::make_unique<syntax::SyntaxSession>(*m_services.languages, *m_services.scheduler, *m_clock, options);
        m_outline = std::make_unique<SyntaxOutlineProvider>(*m_syntax);
        m_syntaxLanguage = syntax::languageForFileName(m_path);
        m_syntax->open(m_syntaxLanguage, m_editor->text());
    }

    m_findBar = new EditorFindBar(this);
    m_findBar->hide();
    connect(m_findBar, &EditorFindBar::queryChanged, this, [this] { onQueryChanged(); });
    connect(m_findBar, &EditorFindBar::findNextRequested, this, [this] { (void)findNext(); });
    connect(m_findBar, &EditorFindBar::findPreviousRequested, this, [this] { (void)findPrevious(); });
    connect(m_findBar, &EditorFindBar::replaceRequested, this, [this] { (void)replaceNext(); });
    connect(m_findBar, &EditorFindBar::replaceAllRequested, this, [this] { (void)replaceAll(); });
    connect(m_findBar, &EditorFindBar::replaceModeRequested, this, [this] {
        m_findBar->setReplaceVisible(true);
        m_findBar->show();
        updateOverlayGeometry();
        if (m_findBar->findText().isEmpty()) {
            m_findBar->focusFind();
        } else {
            m_findBar->focusReplace();
        }
    });
    connect(m_findBar, &EditorFindBar::closeRequested, this, [this] { closeFind(); });

    m_goToLine = new GoToLinePopup(this);
    m_goToLine->hide();
    connect(m_goToLine, &GoToLinePopup::accepted, this, [this](const QString& input) {
        const Result<text::Point> target = text::parseGoToLine(utf8(input));
        if (!target) {
            m_goToLine->showError(QStringLiteral("Enter a line number, or line:column"));
            return;
        }
        m_goToLine->hide();
        setFocus(Qt::OtherFocusReason);
        (void)apply(commands::goToLine(commandInput(), *target));
    });
    connect(m_goToLine, &GoToLinePopup::cancelled, this, [this] {
        m_goToLine->hide();
        setFocus(Qt::OtherFocusReason);
    });

    m_blink.setInterval(500);
    connect(&m_blink, &QTimer::timeout, this, [this] {
        m_caretOn = !m_caretOn;
        viewport()->update();
    });
    // The inspection widget's spinner turns while the analysis runs (only its box repaints).
    m_spinner.setInterval(100);
    connect(&m_spinner, &QTimer::timeout, this, [this] {
        m_spinnerPhase = (m_spinnerPhase + 1) % 8;
        const InspectionLayout layout = inspectionLayout();
        if (!layout.rect.isEmpty()) {
            viewport()->update(layout.rect.toAlignedRect().adjusted(-1, -1, 1, 1));
        }
    });
    m_savedStateId = m_document->undoState().stateId;
    updateWrap();
    updateScrollBars();
    m_lastStatus = status();
    selectRenderer();
}

EditorView::~EditorView() {
    if (m_foldJob)
        m_foldJob->stop.request_stop();
    for (auto& task : m_foldTasks)
        task.join();
    // The session's hook points at this view: stop it before anything else goes.
    m_outline.reset();
    m_syntax.reset();
}

std::unique_ptr<EditorView> createEditorView(text::Document& document, std::string path, EditorServices services,
                                             EditorViewSettings settings, QWidget* parent) {
    return std::make_unique<EditorView>(document, std::move(path), std::move(services), std::move(settings), parent);
}

void EditorView::setPath(std::string path) {
    if (m_path == path) {
        return;
    }
    m_path = std::move(path);
    m_language = &text::languageForPath(m_path);
    m_editor->setLanguage(m_language);
    if (m_syntax != nullptr) {
        const syntax::LanguageId next = syntax::languageForFileName(m_path);
        if (next != m_syntaxLanguage) {
            m_syntaxLanguage = next;
            m_highlights.valid = false;
            m_syntax->open(m_syntaxLanguage, m_editor->text());
            viewport()->update();
        }
    }
}

// --- settings and appearance ---

double EditorView::zoomFactor() const noexcept {
    if (m_services.appearance != nullptr) {
        return m_services.appearance->scale().factor();
    }
    if (const FrostStyle* style = FrostStyle::current()) {
        return style->scale().factor();
    }
    return 1.0;
}

double EditorView::effectiveFontSize() const noexcept { return m_zoomedFontSize.value_or(m_settings.fontSize); }

void EditorView::resetFontZoom() {
    if (!m_zoomedFontSize.has_value()) {
        return;
    }
    m_zoomedFontSize.reset();
    rebuildFont();
    updateWrap();
    updateScrollBars();
    ensureCaretVisible();
    viewport()->update();
}

void EditorView::followAppearance() {
    std::shared_ptr<const Theme> theme;
    if (m_services.appearance != nullptr) {
        theme = m_services.appearance->themeSnapshot();
    } else if (const FrostStyle* style = FrostStyle::current()) {
        theme = style->themePtr();
    }
    if (theme != m_theme) {
        setTheme(std::move(theme));
    }
    if (m_shaper && std::abs(m_shaper->font().pointSizeF() - effectiveFontSize() * zoomFactor()) > 0.01) {
        rebuildFont();
    }
}

void EditorView::setTheme(std::shared_ptr<const Theme> theme) {
    m_theme = std::move(theme);
    m_colors = m_theme ? EditorColors::fromTheme(*m_theme) : EditorColors::defaults();
    viewport()->update();
}

void EditorView::rebuildFont() {
    QFont font(QString::fromStdString(m_settings.fontFamily));
    font.setStyleHint(QFont::TypeWriter, QFont::PreferDefault);
    font.setFixedPitch(true);
    font.setPointSizeF(std::max(1.0, effectiveFontSize() * zoomFactor()));
    font.setKerning(false);
    if (!m_settings.ligatures) {
        font.setFeature(QFont::Tag("calt"), 0);
        font.setFeature(QFont::Tag("liga"), 0);
    }
    auto next = std::make_unique<QtTextShaper>(font, m_settings.tabSize);
    if (m_editor) {
        // The editor switches before the old shaper goes (it borrows it).
        (void)m_editor->setShaper(*next);
    }
    m_shaper = std::move(next);
    if (m_renderer) {
        m_renderer->setFont(m_shaper->font());
    }
    updateMetrics();
}

void EditorView::updateMetrics() {
    m_lineHeight = std::max(1.0, std::ceil(m_shaper->height() * std::max(0.6, m_settings.lineSpacing)));
    m_ascent = std::floor((m_lineHeight - m_shaper->height()) / 2.0 + m_shaper->ascent());
    m_maxRowWidth = 0.0;
    if (m_editor) {
        updateWrap();
        updateScrollBars();
    }
    viewport()->update();
}

void EditorView::applySettings(const EditorViewSettings& settings) {
    const bool fontSizeSettingChanged = settings.fontSize != m_settings.fontSize;
    if (fontSizeSettingChanged) {
        m_zoomedFontSize.reset();
    }
    const bool fontChanged = settings.fontFamily != m_settings.fontFamily || fontSizeSettingChanged ||
                             settings.lineSpacing != m_settings.lineSpacing ||
                             settings.ligatures != m_settings.ligatures || settings.tabSize != m_settings.tabSize;
    const bool rendererChanged = settings.renderer != m_settings.renderer;
    m_settings = settings;
    m_editor->setSettings(modelSettings(m_settings));
    if (fontChanged) {
        rebuildFont();
    }
    if (rendererChanged) {
        // A new editor.renderer value is a fresh start: an earlier QRhi failure is tried again.
        m_rhiFailure.reset();
        selectRenderer();
    }
    updateWrap();
    updateScrollBars();
    restartBlink();
    viewport()->update();
    emitStatus();
}

void EditorView::updateWrap() {
    text::WrapSettings wrap;
    if (m_settings.softWrap && !m_fileInfo.largeFile) {
        const double width = viewport()->width() - gutterWidth() - stripeWidth() - 2.0 * 6.0 * zoomFactor();
        wrap.mode = text::WrapMode::Width;
        wrap.width = std::max(toShaperUnits(width), m_shaper->cellWidth());
    }
    if (!(wrap == m_editor->display().wrap())) {
        (void)m_editor->setWrap(wrap);
        m_maxRowWidth = 0.0;
        m_highlights.valid = false;
        updateScrollBars();
        viewport()->update();
    }
}

void EditorView::restartBlink() {
    m_caretOn = true;
    if (m_settings.caretBlinking && m_focused) {
        m_blink.start();
    } else {
        m_blink.stop();
    }
}

void EditorView::setFileInfo(const EditorFileInfo& info) {
    const bool largeChanged = info.largeFile != m_fileInfo.largeFile;
    m_fileInfo = info;
    if (largeChanged && m_syntax) {
        m_foldCandidates.clear();
        m_foldCandidateMaxEnd.clear();
        if (m_foldJob)
            m_foldJob->stop.request_stop();
        m_foldJob.reset();
        m_syntaxLanguage = m_fileInfo.largeFile ? syntax::LanguageId::None : syntax::languageForFileName(m_path);
        m_syntax->open(m_syntaxLanguage, m_editor->text());
        m_highlights.valid = false;
    }
    updateWrap();
    viewport()->update();
    emitStatus();
}

void EditorView::setInputToPaintSink(std::function<void(const InputToPaintSample&)> sink) {
    m_inputToPaint = std::move(sink);
}

void EditorView::afterChange(const text::EditorUpdate& update, bool notifyTextChange) {
    if (update.change) {
        // Grapheme boundaries can change anywhere on an edited line (for example regional
        // indicator parity), even when the edit did not touch the stored endpoint itself.
        const auto& snapshot = m_editor->text();
        std::vector<text::ByteRange> affectedLines;
        std::int64_t delta = 0;
        for (const auto& edit : update.change->edits) {
            const auto position =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(edit.range.begin.value()) + delta);
            const ByteOffset begin{position};
            const ByteOffset end{position + edit.newBytes};
            const auto first = snapshot.lineInfo(snapshot.lineAt(begin));
            const auto last = snapshot.lineInfo(snapshot.lineAt(end));
            if (first && last) {
                const text::ByteRange lines{first->content.begin, last->content.end};
                if (!affectedLines.empty() && lines.begin <= affectedLines.back().end)
                    affectedLines.back().end = std::max(affectedLines.back().end, lines.end);
                else
                    affectedLines.push_back(lines);
            }
            delta += static_cast<std::int64_t>(edit.newBytes) - static_cast<std::int64_t>(edit.range.size());
        }
        std::vector<text::FoldInfo> candidates;
        for (const auto lines : affectedLines) {
            auto nearby = m_editor->display().foldInfosWithBoundaryIn(lines);
            candidates.insert(candidates.end(), nearby.begin(), nearby.end());
        }
        std::ranges::sort(candidates, [](const auto& a, const auto& b) {
            return a.range.begin > b.range.begin || (a.range.begin == b.range.begin && a.range.end < b.range.end);
        });
        candidates.erase(std::unique(candidates.begin(), candidates.end(),
                                     [](const auto& a, const auto& b) { return a.id == b.id; }),
                         candidates.end());
        for (const auto& fold : candidates) {
            if (!text::isGraphemeBoundary(snapshot, fold.range.begin) ||
                !text::isGraphemeBoundary(snapshot, fold.range.end))
                (void)removeCollapsedFold(fold.id);
        }
        if (m_syntax) {
            m_syntax->apply(*update.change, m_editor->text());
        }
        m_highlights.valid = false;
        m_foldCandidates.clear();
        m_foldCandidateMaxEnd.clear();
        if (m_foldJob)
            m_foldJob->stop.request_stop();
        m_foldJob.reset();
        if (m_syntax) {
            const auto tree = m_syntax->tree();
            if (!tree.empty() && !tree.stale() && tree.version() == m_editor->text().version()) {
                m_foldJob = std::make_shared<FoldQueryJob>();
                m_foldJob->tree = tree;
                m_foldTasks.push_back(m_services.scheduler->spawn(sched::Lane::UserVisible, queryFolds(m_foldJob),
                                                                  m_foldJob->stop.get_token()));
                m_foldTimer.start();
            }
        }
        followInsight(*update.change);
    }
    if (!m_applyingExtendShrink) {
        if (update.change || m_editor->text().version() != m_selectionHistoryDocVersion ||
            !(m_editor->state().selections == m_lastExtendedSelection)) {
            m_selectionHistory.clear();
        }
    }
    const auto selections = m_editor->state().selections;
    for (const auto& selection : selections.all()) {
        revealFoldAt(selection.anchor);
        revealFoldAt(selection.head);
    }
    if (m_balloon && m_editor->state().primary().head != m_balloon->caret) {
        m_balloon.reset(); // The F2 balloon goes with the caret.
    }
    updateWrap(); // The gutter widens when the line count gains a digit.
    updateScrollBars();
    restartBlink();
    ensureCaretVisible();
    viewport()->update();
    emitStatus();
    armInputToPaint();
    if (notifyTextChange && update.change && update.change->version != update.change->baseVersion) {
        emit textChanged(*update.change);
    }
}

void EditorView::armInputToPaint() {
    if (m_inputToPaint && !m_pendingInput) {
        m_pendingInput = m_inputStart.value_or(m_clock->steadyNow());
    }
}

void EditorView::beginInput() {
    if (m_inputToPaint) {
        m_inputStart = m_clock->steadyNow();
    }
}

// --- status ---

bool EditorView::hasSelection() const noexcept {
    return std::ranges::any_of(m_editor->state().selections.all(),
                               [](const text::Selection& selection) { return !selection.empty(); });
}

input::ContextKeys EditorView::contextKeys() const noexcept {
    input::ContextKeys keys;
    keys.set(input::ContextKey::EditorReadOnly, isReadOnly());
    keys.set(input::ContextKey::EditorHasSelection, hasSelection());
    return keys;
}

void EditorView::setColumnMode(bool enabled) {
    if (m_columnMode == enabled) {
        return;
    }
    m_columnMode = enabled;
    emitStatus();
}

void EditorView::markSaved() {
    m_savedStateId = m_document->undoState().stateId;
    emitStatus();
}

void EditorView::setLineMarkers(std::vector<LineMarker> markers) {
    std::ranges::stable_sort(markers, {}, &LineMarker::line);
    m_markers = std::move(markers);
    viewport()->update();
}

void EditorView::setBookmarks(std::vector<LineIndex> lines) {
    std::ranges::sort(lines);
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    if (lines == m_bookmarks) {
        return;
    }
    m_bookmarks = std::move(lines);
    viewport()->update();
}

void EditorView::setLineChanges(std::vector<LineChangeMarker> changes) {
    const std::uint32_t lastLine = m_editor->text().lineCount() - 1;
    for (LineChangeMarker& change : changes) {
        if (change.kind == ChangeStripeKind::Deleted) {
            change.lineCount = 0;
            change.firstLine = LineIndex{std::min(change.firstLine.value(), lastLine)};
        }
    }
    std::ranges::stable_sort(changes, {}, &LineChangeMarker::firstLine);
    m_lineChanges = std::move(changes);
    viewport()->update();
}

void EditorView::reloadFromDocument() {
    m_foldCandidates.clear();
    m_foldCandidateMaxEnd.clear();
    if (m_foldJob)
        m_foldJob->stop.request_stop();
    m_foldJob.reset();
    const ByteOffset caret = m_editor->state().primary().head;
    text::EditorConfig config = m_editor->config();
    config.wrap = m_editor->display().wrap();
    m_editor = std::make_unique<text::Editor>(*m_document, *m_shaper, config);
    const text::Snapshot& snapshot = m_editor->text();
    m_editor->setState(text::EditorState::caretAt(snapshot.clip(std::min(caret, snapshot.size()))));
    if (m_syntax) {
        m_syntax->open(m_syntaxLanguage, snapshot);
    }
    m_highlights.valid = false;
    m_maxRowWidth = 0.0;
    // The reload is no ChangeSet the insight ranges could follow: they stay, clamped to the new
    // text, until their owners replace them.
    const ByteOffset size = snapshot.size();
    const auto clampRange = [size](text::ByteRange& range) {
        range.begin = std::min(range.begin, size);
        range.end = std::clamp(range.end, range.begin, size);
    };
    for (SemanticSpan& span : m_semantic) {
        clampRange(span.range);
    }
    for (text::ByteRange& range : m_inactive) {
        clampRange(range);
    }
    for (std::vector<ActiveDiagnostic>& set : m_diagnostics) {
        for (ActiveDiagnostic& diagnostic : set) {
            clampRange(diagnostic.range);
        }
    }
    for (auto& [owner, set] : m_decorations) {
        for (EditorDecoration& decoration : set) {
            clampRange(decoration.range);
        }
    }
    for (UsageHighlight& usage : m_usages) {
        clampRange(usage.range);
    }
    for (EditorInlayHint& hint : m_inlayHints) {
        hint.offset = std::min(hint.offset, size);
    }
    const std::uint32_t maxLine = snapshot.lineCount() > 0 ? snapshot.lineCount() - 1 : 0;
    for (EditorCodeVisionLens& lens : m_codeVisionLenses) {
        lens.symbolOffset = std::min(lens.symbolOffset, size);
        lens.line = LineIndex{std::min(lens.line.value(), maxLine)};
    }
    for (EditorInlineValue& val : m_inlineDebuggerValues) {
        val.line = LineIndex{std::min(val.line.value(), maxLine)};
    }
    m_balloon.reset();
    afterChange(text::EditorUpdate{});
}

void EditorView::setBreakpointMarkers(std::vector<BreakpointMarker> markers) {
    std::ranges::stable_sort(markers, {}, &BreakpointMarker::line);
    if (markers == m_breakpoints) {
        return;
    }
    m_breakpoints = std::move(markers);
    viewport()->update();
}

void EditorView::setExecutionPoint(std::optional<LineIndex> line) {
    if (line == m_executionPoint) {
        return;
    }
    m_executionPoint = line;
    viewport()->update();
}

EditorViewStatus EditorView::status() const {
    EditorViewStatus out;
    const text::Snapshot& snapshot = m_editor->text();
    const text::EditorState& state = m_editor->state();
    const text::Selection primary = state.primary();
    const text::Point point = snapshot.pointAt(primary.head);
    out.line = static_cast<int>(point.line.value()) + 1;
    // The column counts grapheme clusters, as the caret moves; long lines count code points.
    const ByteOffset lineStart = primary.head - point.column.value();
    if (point.column.value() <= 4096) {
        int column = 1;
        for (ByteOffset at = lineStart; at < primary.head; at = text::nextGraphemeBoundary(snapshot, at)) {
            ++column;
        }
        out.column = column;
    } else {
        out.column = static_cast<int>(snapshot.summaryOf(text::ByteRange{lineStart, primary.head}).chars) + 1;
    }
    out.carets = static_cast<int>(std::max<std::size_t>(state.selections.size(), 1));
    std::uint64_t selected = 0;
    for (const text::Selection& selection : state.selections.all()) {
        selected += snapshot.summaryOf(selection.range()).chars;
    }
    out.selectedCharacters = static_cast<int>(std::min<std::uint64_t>(selected, 0x7FFF'FFFF));
    if (!m_fileInfo.lineSeparator.empty()) {
        out.lineSeparator = m_fileInfo.lineSeparator;
    } else if (snapshot.lineCount() > 1) {
        const Result<text::LineInfo> first = snapshot.lineInfo(LineIndex{0});
        out.lineSeparator = breakToken(first ? first->lineBreak : text::LineBreak::Lf);
    } else {
        out.lineSeparator = breakToken(lineBreakFor(m_settings.lineSeparator));
    }
    out.encoding = m_fileInfo.encoding;
    out.useTabs = !m_settings.insertSpaces;
    out.indentSize = static_cast<int>(m_settings.insertSpaces ? m_settings.indentSize : m_settings.tabSize);
    out.readOnly = m_fileInfo.readOnly;
    out.dirty = m_document->undoState().stateId != m_savedStateId;
    out.columnMode = m_columnMode;
    out.overwrite = m_overwrite;
    return out;
}

void EditorView::emitStatus() {
    const EditorViewStatus now = status();
    if (now == m_lastStatus) {
        return;
    }
    const EditorViewStatus before = std::exchange(m_lastStatus, now);
    if (now.line != before.line || now.column != before.column) {
        emit caretPositionChanged(now.line, now.column);
    }
    if (now.carets != before.carets || now.selectedCharacters != before.selectedCharacters) {
        emit selectionChanged(now.carets, now.selectedCharacters);
    }
    if (now.encoding != before.encoding) {
        emit encodingChanged(QString::fromStdString(now.encoding));
    }
    if (now.lineSeparator != before.lineSeparator) {
        emit lineSeparatorChanged(QString::fromStdString(now.lineSeparator));
    }
    if (now.readOnly != before.readOnly) {
        emit readOnlyChanged(now.readOnly);
    }
    if (now.useTabs != before.useTabs || now.indentSize != before.indentSize) {
        emit indentStyleChanged(now.useTabs, now.indentSize);
    }
    if (now.dirty != before.dirty) {
        emit modifiedChanged(now.dirty);
    }
    if (now.columnMode != before.columnMode) {
        emit columnModeChanged(now.columnMode);
    }
    emit statusChanged();
}

// --- syntax ---

bool EditorView::pollSyntax() {
    collectFoldQuery();
    if (!m_syntax || !m_syntax->poll()) {
        return false;
    }
    m_highlights.valid = false;
    const syntax::SyntaxTree tree = m_syntax->tree();
    if (!tree.empty() && !tree.stale() && tree.version() == m_editor->text().version()) {
        if (m_foldJob)
            m_foldJob->stop.request_stop();
        m_foldJob = std::make_shared<FoldQueryJob>();
        m_foldJob->tree = tree;
        m_foldTasks.push_back(
            m_services.scheduler->spawn(sched::Lane::UserVisible, queryFolds(m_foldJob), m_foldJob->stop.get_token()));
        m_foldTimer.start();
    }
    viewport()->update();
    return true;
}

void EditorView::collectFoldQuery() {
    std::erase_if(m_foldTasks, [](const auto& task) { return task.ready(); });
    if (!m_foldJob) {
        m_foldTimer.stop();
        return;
    }
    const auto job = m_foldJob;
    std::optional<std::vector<text::ByteRange>> ranges;
    {
        const std::lock_guard lock(job->mutex);
        if (!job->done)
            return;
        ranges = std::move(job->ranges);
    }
    m_foldJob.reset();
    m_foldTimer.stop();
    if (ranges && job->tree.version() == m_editor->text().version() &&
        job->tree.text().sharesStorageWith(m_editor->text())) {
        m_foldCandidates = std::move(*ranges);
        m_foldCandidateMaxEnd = std::move(job->maxEnd);
        mergeLanguageFolds();
        viewport()->update();
    }
}

bool EditorView::setLanguageFoldRanges(std::uint64_t textVersion, std::vector<text::ByteRange> ranges) {
    if (textVersion != m_editor->text().version()) {
        return false;
    }
    const ByteOffset size = m_editor->text().size();
    std::erase_if(ranges, [size](const text::ByteRange& range) { return range.empty() || range.end > size; });
    std::ranges::sort(ranges, candidateLess);
    ranges.erase(std::unique(ranges.begin(), ranges.end()), ranges.end());
    m_languageFolds = std::move(ranges);
    m_languageFoldsVersion = textVersion;
    mergeLanguageFolds();
    viewport()->update();
    return true;
}

void EditorView::mergeLanguageFolds() {
    if (m_languageFolds.empty() || m_languageFoldsVersion != m_editor->text().version()) {
        return;
    }
    std::vector<text::ByteRange> merged;
    merged.reserve(m_foldCandidates.size() + m_languageFolds.size());
    std::ranges::merge(m_foldCandidates, m_languageFolds, std::back_inserter(merged), candidateLess);
    merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
    m_foldCandidates = std::move(merged);
    m_foldCandidateMaxEnd.assign(m_foldCandidates.size() * 4, 0);
    (void)buildCandidateMax(m_foldCandidates, m_foldCandidateMaxEnd, 1, 0, m_foldCandidates.size(), {});
}

std::vector<syntax::HighlightSpan> EditorView::highlightsFor(text::ByteRange range) {
    if (!m_syntax || m_fileInfo.largeFile) {
        return {};
    }
    const syntax::SyntaxTree tree = m_syntax->tree();
    if (tree.empty() || tree.version() != m_editor->text().version()) {
        return {};
    }
    const void* identity = tree.state().get();
    if (m_highlights.valid && m_highlights.tree == identity && m_highlights.version == tree.version() &&
        m_highlights.range.begin <= range.begin && range.end <= m_highlights.range.end) {
        return m_highlights.spans;
    }
    syntax::QueryLimits limits;
    limits.clock = m_clock;
    limits.budget = m_services.highlightBudget;
    syntax::HighlightResult result = tree.highlights(range, limits);
    m_highlights.tree = identity;
    m_highlights.version = tree.version();
    m_highlights.range = range;
    m_highlights.spans = std::move(result.spans);
    // An incomplete result (the budget ran out, queries not compiled yet) is queried again next time.
    m_highlights.valid = result.complete;
    return m_highlights.spans;
}

} // namespace frost::ui
