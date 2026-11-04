// SPDX-License-Identifier: Apache-2.0
//
// EditorView (frost/ui/editor/editor_view.hpp), part 1: construction, settings and appearance, the
// model plumbing every command goes through, the editor actions, clipboard, find/replace, Go to
// Line, status and syntax. Painting is in editor_view_paint.cpp, input events in
// editor_view_input.cpp.

#include "frost/ui/editor/editor_view.hpp"

#include "frost/sched/task.hpp"
#include "frost/syntax/syntax_tree.hpp"
#include "frost/text/editor/unicode.hpp"
#include "frost/ui/appearance_manager.hpp"
#include "frost/ui/editor/find_bar.hpp"
#include "frost/ui/editor/go_to_line.hpp"
#include "frost/ui/frost_style.hpp"

#include <QClipboard>
#include <QGuiApplication>
#include <QLineEdit>
#include <QMetaObject>
#include <QMimeData>
#include <QScrollBar>

#include <algorithm>
#include <cmath>
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
using text::Movement;

// The MIME type that marks a clipboard line copy (copy or cut with no selection), so paste puts
// it above the caret's line as IntelliJ does.
constexpr const char* kLineCopyMime = "application/x-frostide-line-copy";

bool candidateLess(text::ByteRange a, text::ByteRange b) noexcept {
    return a.begin < b.begin || (a.begin == b.begin && a.end > b.end);
}

// Keep cancellation responsive even when a generated file contains many candidate regions.
bool orderCandidates(std::vector<text::ByteRange>& ranges, std::stop_token stop) {
    constexpr std::size_t chunk = 64;
    for (std::size_t first = 0; first < ranges.size(); first += chunk) {
        if (stop.stop_requested())
            return false;
        const auto end = std::min(first + chunk, ranges.size());
        std::sort(ranges.begin() + static_cast<std::ptrdiff_t>(first),
                  ranges.begin() + static_cast<std::ptrdiff_t>(end), candidateLess);
    }
    std::vector<text::ByteRange> scratch(ranges.size());
    for (std::size_t width = chunk; width < ranges.size(); width *= 2) {
        for (std::size_t first = 0; first < ranges.size(); first += width * 2) {
            const auto mid = std::min(first + width, ranges.size());
            const auto end = std::min(mid + width, ranges.size());
            auto left = first;
            auto right = mid;
            for (auto out = first; out < end; ++out) {
                if ((out % chunk) == 0 && stop.stop_requested())
                    return false;
                scratch[out] = right == end || (left < mid && !candidateLess(ranges[right], ranges[left]))
                                   ? ranges[left++]
                                   : ranges[right++];
            }
        }
        ranges.swap(scratch);
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if ((i % chunk) == 0 && stop.stop_requested())
            return false;
        if (kept == 0 || ranges[i] != ranges[kept - 1])
            ranges[kept++] = ranges[i];
    }
    ranges.resize(kept);
    return !stop.stop_requested();
}

std::uint64_t buildCandidateMax(const std::vector<text::ByteRange>& ranges, std::vector<std::uint64_t>& tree,
                                std::size_t node, std::size_t first, std::size_t end, std::stop_token stop) {
    if (stop.stop_requested())
        return 0;
    if (end - first == 1)
        return tree[node] = ranges[first].end.value();
    const auto mid = first + (end - first) / 2;
    return tree[node] = std::max(buildCandidateMax(ranges, tree, node * 2, first, mid, stop),
                                 buildCandidateMax(ranges, tree, node * 2 + 1, mid, end, stop));
}

std::optional<std::size_t> findCandidate(const std::vector<std::uint64_t>& tree, std::size_t node, std::size_t first,
                                         std::size_t end, std::size_t limit, std::uint64_t offset) noexcept {
    if (first >= limit || tree[node] < offset)
        return std::nullopt;
    if (end - first == 1)
        return first;
    const auto mid = first + (end - first) / 2;
    if (auto right = findCandidate(tree, node * 2 + 1, mid, end, limit, offset))
        return right;
    return findCandidate(tree, node * 2, first, mid, limit, offset);
}

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

[[nodiscard]] text::LineBreak lineBreakFor(const std::string& setting) noexcept {
    if (setting == "lf") {
        return text::LineBreak::Lf;
    }
    if (setting == "crlf") {
        return text::LineBreak::CrLf;
    }
    if (setting == "cr") {
        return text::LineBreak::Cr;
    }
#ifdef _WIN32
    return text::LineBreak::CrLf;
#else
    return text::LineBreak::Lf;
#endif
}

[[nodiscard]] std::string_view breakText(text::LineBreak lineBreak) noexcept {
    switch (lineBreak) {
    case text::LineBreak::Lf: return "\n";
    case text::LineBreak::Cr: return "\r";
    case text::LineBreak::CrLf: return "\r\n";
    case text::LineBreak::None: break;
    }
    return "\n";
}

[[nodiscard]] std::string breakToken(text::LineBreak lineBreak) {
    switch (lineBreak) {
    case text::LineBreak::Cr: return "CR";
    case text::LineBreak::CrLf: return "CRLF";
    case text::LineBreak::Lf:
    case text::LineBreak::None: break;
    }
    return "LF";
}

[[nodiscard]] text::EditorSettings modelSettings(const EditorViewSettings& settings) noexcept {
    text::EditorSettings out;
    out.tabSize = std::max<std::uint32_t>(settings.tabSize, 1);
    out.indentSize = std::max<std::uint32_t>(settings.indentSize, 1);
    out.insertSpaces = settings.insertSpaces;
    out.defaultLineBreak = lineBreakFor(settings.lineSeparator);
    return out;
}

[[nodiscard]] std::string utf8(const QString& text) { return text.toStdString(); }

// `text` with every line break (CRLF, CR, LF) replaced by `lineBreak`.
[[nodiscard]] std::string normaliseBreaks(std::string_view text, std::string_view lineBreak) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            out += lineBreak;
        } else if (text[i] == '\n') {
            out += lineBreak;
        } else {
            out += text[i];
        }
    }
    return out;
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
    selectRenderer(); // Raster now; the QRhi backend, when chosen, after the first frame.
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
    if (m_shaper && std::abs(m_shaper->font().pointSizeF() - m_settings.fontSize * zoomFactor()) > 0.01) {
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
    font.setPointSizeF(std::max(1.0, m_settings.fontSize * zoomFactor()));
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
    const bool fontChanged = settings.fontFamily != m_settings.fontFamily || settings.fontSize != m_settings.fontSize ||
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

// --- model plumbing ---

text::CommandInput EditorView::commandInput() const { return m_editor->input(); }

bool EditorView::applyUpdate(Result<text::EditorUpdate> update) {
    if (!update) {
        m_lastError = update.error();
        return false;
    }
    afterChange(*update);
    return true;
}

Result<void> EditorView::followExternalChange(const text::ChangeSet& change, bool notifyTextChange) {
    auto update = m_editor->follow(change);
    if (!update)
        return std::unexpected(std::move(update.error()));
    afterChange(*update, notifyTextChange);
    return {};
}

bool EditorView::apply(const text::CommandResult& result) {
    if (result.changesText() && isReadOnly()) {
        return false;
    }
    if (result.changesText() && std::ranges::any_of(result.edits, [&](const auto& edit) {
            return m_editor->display().intersectsFold(edit.range);
        })) {
        std::vector<text::FoldInfo> folds;
        for (const auto& edit : result.edits) {
            auto affected = m_editor->display().foldInfosIntersecting(edit.range);
            folds.insert(folds.end(), affected.begin(), affected.end());
        }
        std::ranges::sort(folds, [](const auto& a, const auto& b) {
            return a.range.begin > b.range.begin || (a.range.begin == b.range.begin && a.range.end < b.range.end);
        });
        folds.erase(std::unique(folds.begin(), folds.end(), [](const auto& a, const auto& b) { return a.id == b.id; }),
                    folds.end());
        for (const auto& fold : folds)
            (void)removeCollapsedFold(fold.id);
    }
    return applyUpdate(m_editor->apply(result));
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

void EditorView::setState(text::EditorState state) {
    m_editor->setState(std::move(state));
    afterChange(text::EditorUpdate{});
}

bool EditorView::setCollapsedFolds(std::vector<text::ByteRange> ranges) {
    const auto& snapshot = m_editor->text();
    for (const auto range : ranges) {
        if (range.begin > range.end || range.end > snapshot.size() || !snapshot.isCharBoundary(range.begin) ||
            !snapshot.isCharBoundary(range.end) || !text::isGraphemeBoundary(snapshot, range.begin) ||
            !text::isGraphemeBoundary(snapshot, range.end)) {
            m_lastError = Error(ErrorCode::InvalidArgument, "fold boundaries must be grapheme boundaries");
            return false;
        }
    }
    auto changed = m_editor->setFolds(std::move(ranges));
    if (!changed) {
        m_lastError = changed.error();
        return false;
    }
    const auto& state = m_editor->state();
    std::vector<text::Selection> selections(state.selections.all().begin(), state.selections.all().end());
    bool moved = false;
    for (auto& selection : selections) {
        for (const auto& fold : collapsedFolds()) {
            if (fold.begin < selection.anchor && selection.anchor < fold.end) {
                selection.anchor = fold.begin;
                moved = true;
            }
            if (fold.begin < selection.head && selection.head < fold.end) {
                selection.head = fold.begin;
                moved = true;
            }
        }
    }
    if (moved) {
        m_editor->setState(
            text::EditorState::from(text::SelectionSet::fromSelections(selections, state.selections.primaryIndex())));
    }
    updateScrollBars();
    ensureCaretVisible();
    viewport()->update();
    emitStatus();
    return true;
}

void EditorView::revealFoldAt(ByteOffset target) {
    const auto folds = m_editor->display().foldInfosContaining(target);
    for (const auto& fold : folds)
        (void)removeCollapsedFold(fold.id);
}

bool EditorView::addCollapsedFold(text::ByteRange range) {
    const auto& snapshot = m_editor->text();
    if (range.begin >= range.end || range.end > snapshot.size() || !snapshot.isCharBoundary(range.begin) ||
        !snapshot.isCharBoundary(range.end) || !text::isGraphemeBoundary(snapshot, range.begin) ||
        !text::isGraphemeBoundary(snapshot, range.end))
        return false;
    auto added = m_editor->addFold(range);
    if (!added) {
        m_lastError = added.error();
        return false;
    }
    const auto& state = m_editor->state();
    std::vector<text::Selection> selections(state.selections.all().begin(), state.selections.all().end());
    bool moved = false;
    for (auto& selection : selections) {
        if (range.begin < selection.anchor && selection.anchor < range.end) {
            selection.anchor = range.begin;
            moved = true;
        }
        if (range.begin < selection.head && selection.head < range.end) {
            selection.head = range.begin;
            moved = true;
        }
    }
    if (moved)
        m_editor->setState(
            text::EditorState::from(text::SelectionSet::fromSelections(selections, state.selections.primaryIndex())));
    updateScrollBars();
    ensureCaretVisible();
    viewport()->update();
    emitStatus();
    return true;
}

bool EditorView::removeCollapsedFold(text::FoldId id) {
    const auto removed = m_editor->removeFold(id);
    if (!removed) {
        m_lastError = removed.error();
        return false;
    }
    updateScrollBars();
    viewport()->update();
    return true;
}

bool EditorView::changeFold(bool collapse, bool all) {
    if (!finishCompositionBeforeFold())
        return false;
    if (all && !collapse)
        return setCollapsedFolds({});
    const auto selection = m_editor->state().primary().range();
    if (collapse && !all && !selection.empty()) {
        if (!addCollapsedFold(selection))
            return false;
        setState(text::EditorState::caretAt(selection.begin));
        return true;
    }
    if (collapse && all) {
        return setCollapsedFolds(m_foldCandidates);
    }
    const auto at = m_editor->state().primary().head;
    if (!collapse) {
        const auto containing = m_editor->display().foldInfosContaining(at);
        if (!containing.empty())
            return removeCollapsedFold(containing.front().id);
        auto found = m_editor->display().foldAtBoundary(at, true);
        if (!found)
            found = m_editor->display().foldAtBoundary(at, false);
        if (!found) {
            const auto line = m_editor->text().lineInfo(m_editor->text().lineAt(at));
            if (line)
                found = m_editor->display().firstFoldStartingIn(line->content);
        }
        return found && removeCollapsedFold(found->id);
    }
    std::optional<text::ByteRange> candidate;
    const auto line = m_editor->text().lineAt(at);
    const auto info = m_editor->text().lineInfo(line);
    if (info) {
        const auto first = std::ranges::lower_bound(m_foldCandidates, info->content.begin, {}, &text::ByteRange::begin);
        if (first != m_foldCandidates.end() && first->begin <= info->content.end)
            candidate = *first;
    }
    if (!candidate)
        candidate = foldCandidateAt(at);
    return candidate && addCollapsedFold(*candidate);
}

std::optional<text::ByteRange> EditorView::foldCandidateAt(ByteOffset offset) const noexcept {
    if (m_foldCandidates.empty())
        return std::nullopt;
    const auto limit = static_cast<std::size_t>(
        std::ranges::upper_bound(m_foldCandidates, offset, {}, &text::ByteRange::begin) - m_foldCandidates.begin());
    const auto found = findCandidate(m_foldCandidateMaxEnd, 1, 0, m_foldCandidates.size(), limit, offset.value());
    return found ? std::optional(m_foldCandidates[*found]) : std::nullopt;
}

bool EditorView::toggleFoldOnLine(LineIndex line) {
    const auto& snapshot = m_editor->text();
    const auto info = snapshot.lineInfo(line);
    if (!info)
        return false;
    if (const auto found = m_editor->display().firstFoldStartingIn(info->content))
        return removeCollapsedFold(found->id);
    const auto candidate = std::ranges::lower_bound(m_foldCandidates, info->content.begin, {}, &text::ByteRange::begin);
    if (candidate == m_foldCandidates.end() || candidate->begin > info->content.end)
        return false;
    return addCollapsedFold(*candidate);
}

bool EditorView::typeText(std::string_view text) {
    if (text.empty() || isReadOnly() || m_composing) {
        return false;
    }
    return apply(commands::type(commandInput(), text));
}

void EditorView::goToLine(int line, int column) {
    const text::Point target{LineIndex{static_cast<std::uint32_t>(std::max(line, 1) - 1)},
                             ByteColumn{static_cast<std::uint32_t>(std::max(column, 1) - 1)}};
    (void)apply(commands::goToLine(commandInput(), target));
}

void EditorView::goToStatusPosition(int line, int column) {
    const auto& snapshot = m_editor->text();
    const auto index =
        std::min<std::uint32_t>(static_cast<std::uint32_t>(std::max(line, 1) - 1), snapshot.lineCount() - 1);
    const auto info = snapshot.lineInfo(LineIndex{index});
    FROST_ASSERT(info);
    const int wanted = std::max(column, 1);
    ByteOffset at = info->content.begin;
    for (int current = 1; current < wanted && at < info->content.end; ++current)
        at = text::nextGraphemeBoundary(snapshot, at);
    // status() switches to Unicode scalar counts on lines past its 4096-byte work limit.
    if (at.value() - info->content.begin.value() > 4096) {
        at = info->content.begin;
        for (int current = 1; current < wanted && at < info->content.end; ++current) {
            auto next = at + 1;
            while (next < info->content.end && snapshot.clip(next) == at)
                next = next + 1;
            at = next;
        }
    }
    setState(text::EditorState::caretAt(at));
}

bool EditorView::move(Movement movement, bool extend) {
    return apply(commands::move(commandInput(), movement, extend));
}

void EditorView::scrollRows(int rows) { verticalScrollBar()->setValue(verticalScrollBar()->value() + rows); }

bool EditorView::perform(EditorAction action) {
    const EditorActionInfo& info = editorActionInfo(action);
    if (info.edits && isReadOnly()) {
        return false;
    }
    if ((action == EditorAction::Backspace || action == EditorAction::Delete) && m_editor->state().primary().empty()) {
        const auto caret = m_editor->state().primary().head;
        if (m_editor->display().hasFoldBoundary(caret, action == EditorAction::Delete)) {
            while (const auto fold = m_editor->display().foldAtBoundary(caret, action == EditorAction::Delete)) {
                if (!removeCollapsedFold(fold->id))
                    return false;
            }
            return true;
        }
    }
    const int page = static_cast<int>(m_editor->config().pageRows);
    class IndentContext final : public text::IIndentContext {
    public:
        explicit IndentContext(syntax::SyntaxTree tree) : m_tree(std::move(tree)) {}
        std::optional<text::ByteRange> opaqueAt(ByteOffset at) const noexcept override {
            return m_tree.luaOpaqueAt(at);
        }
        ByteOffset scanStart(ByteOffset at) const noexcept override { return m_tree.luaErrorStartAt(at); }
        std::optional<ByteOffset> closerOpener(ByteOffset at) const noexcept override {
            return m_tree.luaCloserOpener(at);
        }

    private:
        syntax::SyntaxTree m_tree;
    };
    const auto newline = [&] {
        const syntax::SyntaxTree indentTree = m_syntax ? m_syntax->tree() : syntax::SyntaxTree{};
        const IndentContext indentContext(indentTree);
        text::CommandInput input = commandInput();
        if (!indentTree.empty() && !indentTree.stale() && indentTree.language() == syntax::LanguageId::Lua &&
            indentTree.version() == input.text.version()) {
            input.indentContext = &indentContext;
        }
        return apply(commands::newline(input));
    };
    using A = EditorAction;
    switch (action) {
    case A::MoveLeft: return move(Movement::CharLeft, false);
    case A::MoveRight: return move(Movement::CharRight, false);
    case A::MoveUp: return move(Movement::LineUp, false);
    case A::MoveDown: return move(Movement::LineDown, false);
    case A::MoveLeftWithSelection: return move(Movement::CharLeft, true);
    case A::MoveRightWithSelection: return move(Movement::CharRight, true);
    case A::MoveUpWithSelection: return move(Movement::LineUp, true);
    case A::MoveDownWithSelection: return move(Movement::LineDown, true);
    case A::WordLeft: return move(Movement::WordLeft, false);
    case A::WordRight: return move(Movement::WordRight, false);
    case A::WordLeftWithSelection: return move(Movement::WordLeft, true);
    case A::WordRightWithSelection: return move(Movement::WordRight, true);
    case A::LineStart: return move(Movement::LineStart, false);
    case A::LineEnd: return move(Movement::LineEnd, false);
    case A::LineStartWithSelection: return move(Movement::LineStart, true);
    case A::LineEndWithSelection: return move(Movement::LineEnd, true);
    case A::TextStart: return move(Movement::TextStart, false);
    case A::TextEnd: return move(Movement::TextEnd, false);
    case A::TextStartWithSelection: return move(Movement::TextStart, true);
    case A::TextEndWithSelection: return move(Movement::TextEnd, true);
    case A::PageUp: scrollRows(-page); return move(Movement::PageUp, false);
    case A::PageDown: scrollRows(page); return move(Movement::PageDown, false);
    case A::PageUpWithSelection: scrollRows(-page); return move(Movement::PageUp, true);
    case A::PageDownWithSelection: scrollRows(page); return move(Movement::PageDown, true);
    case A::ScrollUp: scrollRows(-1); return true;
    case A::ScrollDown: scrollRows(1); return true;
    case A::Backspace: return apply(commands::backspace(commandInput()));
    case A::Delete: return apply(commands::deleteForward(commandInput()));
    case A::DeleteToWordStart: return apply(commands::deleteWordBackward(commandInput()));
    case A::DeleteToWordEnd: return apply(commands::deleteWordForward(commandInput()));
    case A::Enter: return newline();
    case A::StartNewLine: (void)move(Movement::LineEnd, false); return newline();
    case A::Tab: return apply(commands::tab(commandInput()));
    case A::Unindent: return apply(commands::outdent(commandInput()));
    case A::Escape: escape(); return true;
    case A::SelectAll: return apply(commands::selectAll(commandInput()));
    case A::Copy: copySelection(); return true;
    case A::Cut: cutSelection(); return true;
    case A::Paste: pasteClipboard(); return true;
    case A::Undo: return applyUpdate(m_editor->undo());
    case A::Redo: return applyUpdate(m_editor->redo());
    case A::Duplicate: return apply(commands::duplicate(commandInput()));
    case A::DeleteLine: return apply(commands::deleteLines(commandInput()));
    case A::JoinLines: return apply(commands::joinLines(commandInput()));
    case A::MoveLineUp: return apply(commands::moveLinesUp(commandInput()));
    case A::MoveLineDown: return apply(commands::moveLinesDown(commandInput()));
    case A::CommentLine: return apply(commands::toggleLineComment(commandInput()));
    case A::CommentBlock: return apply(commands::toggleBlockComment(commandInput()));
    case A::Find: openFind(false); return true;
    case A::Replace: openFind(true); return true;
    case A::FindNext: return findNext();
    case A::FindPrevious: return findPrevious();
    case A::GoToLine: openGoToLine(); return true;
    case A::NextMember: return m_outline != nullptr && apply(commands::nextMember(commandInput(), *m_outline, false));
    case A::PreviousMember:
        return m_outline != nullptr && apply(commands::previousMember(commandInput(), *m_outline, false));
    case A::NextError: return gotoNextProblem(true);
    case A::PreviousError: return gotoNextProblem(false);
    case A::Fold: return changeFold(true, false);
    case A::Unfold: return changeFold(false, false);
    case A::FoldAll: return changeFold(true, true);
    case A::UnfoldAll: return changeFold(false, true);
    case A::FoldSelection: return changeFold(true, false);
    }
    return false;
}

void EditorView::escape() {
    if (findBarVisible()) {
        closeFind();
        return;
    }
    if (m_goToLine->isVisible()) {
        m_goToLine->hide();
        return;
    }
    if (m_balloon) {
        m_balloon.reset();
        viewport()->update();
        return;
    }
    // IntelliJ's Escape: drop the secondary carets and the selection, keep the primary caret.
    setState(text::EditorState::caretAt(m_editor->state().primary().head));
}

// --- clipboard ---

std::string EditorView::lineBreakText() const {
    const text::Snapshot& snapshot = m_editor->text();
    if (snapshot.lineCount() > 1) {
        if (const Result<text::LineInfo> first = snapshot.lineInfo(LineIndex{0})) {
            return std::string(breakText(first->lineBreak));
        }
    }
    return std::string(breakText(lineBreakFor(m_settings.lineSeparator)));
}

void EditorView::copySelection() {
    const text::Snapshot& snapshot = m_editor->text();
    const std::span<const text::Selection> selections = m_editor->state().selections.all();
    const bool lineCopy =
        std::ranges::all_of(selections, [](const text::Selection& selection) { return selection.empty(); });
    std::string out;
    if (lineCopy) {
        // IntelliJ copies the caret's whole line, break included, when nothing is selected.
        std::vector<std::uint32_t> lines;
        for (const text::Selection& selection : selections) {
            lines.push_back(snapshot.lineAt(selection.head).value());
        }
        if (lines.empty()) {
            lines.push_back(0);
        }
        std::ranges::sort(lines);
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
        for (const std::uint32_t line : lines) {
            const Result<text::LineInfo> info = snapshot.lineInfo(LineIndex{line});
            if (!info) {
                continue;
            }
            out += snapshot.text(info->content);
            out += info->lineBreak == text::LineBreak::None ? lineBreakText() : std::string(breakText(info->lineBreak));
        }
    } else {
        const std::string separator = lineBreakText();
        bool firstPart = true;
        for (const text::Selection& selection : selections) {
            if (selection.empty()) {
                continue;
            }
            if (!firstPart) {
                out += separator;
            }
            out += snapshot.text(selection.range());
            firstPart = false;
        }
    }
    auto* mime = new QMimeData();
    mime->setText(QString::fromStdString(out));
    if (lineCopy) {
        mime->setData(QString::fromLatin1(kLineCopyMime), QByteArray("1"));
    }
    QGuiApplication::clipboard()->setMimeData(mime);
}

void EditorView::cutSelection() {
    const bool lineCut = !hasSelection();
    copySelection();
    if (lineCut) {
        (void)apply(commands::deleteLines(commandInput()));
        return;
    }
    // Deleting the selection, as its own undo step (a cut never coalesces with typing).
    text::CommandResult result = commands::backspace(commandInput());
    result.kind = text::EditKind::Other;
    result.breakCoalescing = true;
    (void)apply(result);
}

void EditorView::pasteClipboard() {
    const QMimeData* mime = QGuiApplication::clipboard()->mimeData();
    if (mime == nullptr || !mime->hasText()) {
        return;
    }
    const std::string text = normaliseBreaks(utf8(mime->text()), lineBreakText());
    if (mime->hasFormat(QString::fromLatin1(kLineCopyMime)) && !hasSelection()) {
        (void)apply(commands::pasteLines(commandInput(), text));
    } else {
        (void)apply(commands::paste(commandInput(), text));
    }
}

// --- find / replace ---

bool EditorView::findBarVisible() const noexcept { return m_findBar != nullptr && !m_findBar->isHidden(); }

void EditorView::openFind(bool replace) {
    const text::Selection primary = m_editor->state().primary();
    if (!findBarVisible()) {
        m_findOrigin = primary.range().begin;
    }
    if (!primary.empty() && primary.range().size() <= 1000) {
        const std::string selected = m_editor->text().text(primary.range());
        if (selected.find_first_of("\r\n") == std::string::npos) {
            m_findBar->setFindText(QString::fromStdString(selected));
            m_findOrigin = primary.range().begin;
        }
    }
    m_findBar->setReplaceVisible(replace);
    m_findBar->show();
    updateOverlayGeometry();
    m_findBar->focusFind();
    m_matchesVersion = ~std::uint64_t{0};
    refreshMatchLabel();
    viewport()->update();
}

void EditorView::closeFind() {
    m_findBar->hide();
    updateOverlayGeometry();
    m_matches.clear();
    m_matchesVersion = ~std::uint64_t{0};
    setFocus(Qt::OtherFocusReason);
    viewport()->update();
}

std::optional<text::SearchPattern> EditorView::currentPattern() const {
    const QString query = m_findBar->findText();
    if (query.isEmpty()) {
        return std::nullopt;
    }
    Result<text::SearchPattern> pattern = text::SearchPattern::compile(utf8(query), m_findBar->options());
    if (!pattern) {
        return std::nullopt;
    }
    return std::move(*pattern);
}

void EditorView::onQueryChanged() {
    m_matchesVersion = ~std::uint64_t{0};
    if (const std::optional<text::SearchPattern> pattern = currentPattern()) {
        // Incremental search: the first match at or after where the search started.
        text::CommandInput input = commandInput();
        input.state = text::EditorState::caretAt(m_findOrigin);
        const text::FindResult result = commands::findNext(input, *pattern);
        if (result.found) {
            (void)apply(result.command);
        }
    }
    refreshMatchLabel();
    viewport()->update();
}

const std::vector<text::ByteRange>& EditorView::searchMatches() {
    if (!findBarVisible()) {
        m_matches.clear();
        return m_matches;
    }
    const std::uint64_t version = m_editor->text().version();
    const QString query = m_findBar->findText();
    const text::SearchOptions options = m_findBar->options();
    if (version == m_matchesVersion && query == m_matchesQuery && options == m_matchesOptions) {
        return m_matches;
    }
    m_matches.clear();
    if (const std::optional<text::SearchPattern> pattern = currentPattern()) {
        for (const text::ByteRange& match : pattern->findAll(m_editor->text())) {
            if (!match.empty()) {
                m_matches.push_back(match);
            }
        }
    }
    m_matchesVersion = version;
    m_matchesQuery = query;
    m_matchesOptions = options;
    return m_matches;
}

void EditorView::refreshMatchLabel() {
    if (!findBarVisible()) {
        return;
    }
    const QString query = m_findBar->findText();
    if (!query.isEmpty() && !text::SearchPattern::compile(utf8(query), m_findBar->options())) {
        m_findBar->showError(QStringLiteral("Bad pattern"));
        return;
    }
    const std::vector<text::ByteRange>& matches = searchMatches();
    const text::ByteRange selected = m_editor->state().primary().range();
    const auto it = std::ranges::lower_bound(matches, selected.begin, {}, &text::ByteRange::begin);
    const std::size_t current =
        (it != matches.end() && *it == selected) ? static_cast<std::size_t>(it - matches.begin()) + 1 : 0;
    m_findBar->showMatches(current, matches.size());
}

bool EditorView::findNext() {
    const std::optional<text::SearchPattern> pattern = currentPattern();
    if (!pattern) {
        return false;
    }
    const text::FindResult result = commands::findNext(commandInput(), *pattern);
    if (result.found) {
        (void)apply(result.command);
    }
    refreshMatchLabel();
    return result.found;
}

bool EditorView::findPrevious() {
    const std::optional<text::SearchPattern> pattern = currentPattern();
    if (!pattern) {
        return false;
    }
    const text::FindResult result = commands::findPrevious(commandInput(), *pattern);
    if (result.found) {
        (void)apply(result.command);
    }
    refreshMatchLabel();
    return result.found;
}

bool EditorView::replaceNext() {
    const std::optional<text::SearchPattern> pattern = currentPattern();
    if (!pattern || isReadOnly()) {
        return false;
    }
    const text::FindResult result = commands::replaceNext(commandInput(), *pattern, utf8(m_findBar->replaceText()));
    const bool applied = apply(result.command);
    refreshMatchLabel();
    return applied && (result.found || result.replaced > 0);
}

std::size_t EditorView::replaceAll() {
    const std::optional<text::SearchPattern> pattern = currentPattern();
    if (!pattern || isReadOnly()) {
        return 0;
    }
    const text::FindResult result = commands::replaceAll(commandInput(), *pattern, utf8(m_findBar->replaceText()));
    if (!apply(result.command)) {
        return 0;
    }
    refreshMatchLabel();
    return result.replaced;
}

// --- Go to Line ---

void EditorView::openGoToLine() {
    const EditorViewStatus now = status();
    m_goToLine->open(QStringLiteral("%1:%2").arg(now.line).arg(now.column));
    updateOverlayGeometry();
}

void EditorView::updateOverlayGeometry() {
    if (findBarVisible()) {
        const int height = m_findBar->sizeHint().height();
        setViewportMargins(0, height, 0, 0);
        m_findBar->setGeometry(0, 0, width(), height);
    } else {
        setViewportMargins(0, 0, 0, 0);
    }
    if (m_goToLine != nullptr && !m_goToLine->isHidden()) {
        const QSize size = m_goToLine->sizeHint();
        const int x = std::max(0, (width() - size.width()) / 2);
        m_goToLine->setGeometry(x, viewport()->y() + 8, size.width(), size.height());
        m_goToLine->raise();
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

void EditorView::markSaved() {
    m_savedStateId = m_document->undoState().stateId;
    emitStatus();
}

void EditorView::setLineMarkers(std::vector<LineMarker> markers) {
    std::ranges::stable_sort(markers, {}, &LineMarker::line);
    m_markers = std::move(markers);
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
        viewport()->update();
    }
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
