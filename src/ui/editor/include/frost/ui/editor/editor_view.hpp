// SPDX-License-Identifier: Apache-2.0
//
// EditorView (PLAN.md section 2 "Editor rendering"; ADR 0037): FrostIDE's text editor widget, a
// QAbstractScrollArea over a frost::text::Editor (selections, commands, undo) and its DisplayMap,
// painted through the backend-neutral display list and IEditorRenderer, with tree-sitter
// highlighting, the gutter, find/replace, Go to Line, IME, mouse selection and the editor actions;
// and (ADR 0047) semantic highlighting, inactive regions, diagnostics with squiggles, the error
// stripe, the inspection widget and F2 / Shift+F2 problem navigation.
#pragma once

#include "frost/core/clock.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/syntax/highlight.hpp"
#include "frost/syntax/language.hpp"
#include "frost/syntax/syntax_session.hpp"
#include "frost/text/display_map.hpp"
#include "frost/text/document.hpp"
#include "frost/text/editor/commands.hpp"
#include "frost/text/editor/editor.hpp"
#include "frost/text/editor/language.hpp"
#include "frost/text/editor/search.hpp"
#include "frost/ui/editor/display_list.hpp"
#include "frost/ui/editor/editor_actions.hpp"
#include "frost/ui/editor/editor_backend.hpp"
#include "frost/ui/editor/editor_colors.hpp"
#include "frost/ui/editor/editor_insight.hpp"
#include "frost/ui/editor/editor_renderer.hpp"
#include "frost/ui/editor/editor_settings.hpp"
#include "frost/ui/editor/qt_text_shaper.hpp"
#include "frost/ui/editor/syntax_outline.hpp"
#include "frost/ui/model/theme.hpp"

#include <QAbstractScrollArea>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QTimer>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QInputMethodEvent;

namespace frost::ui {

class AppearanceManager;
class EditorFindBar;
class EditorKeyRouting;
class GoToLinePopup;
class RhiEditorViewport;

// A gutter marker: an error or warning on buffer line `line` (W1's error list fills these).
struct LineMarker {
    LineIndex line;
    MarkerSeverity severity = MarkerSeverity::Error;
    std::string message; // The tooltip.

    friend bool operator==(const LineMarker&, const LineMarker&) = default;
};

/*
 * A VCS change marker (M5, ADR 0048): `lineCount` buffer lines from `firstLine` were added or
 * modified against the file's HEAD version; a Deleted marker has lineCount 0 and marks the line the
 * deleted lines stood before (the line count itself at the end of the text is clamped to the last
 * line). The VCS layer computes them (frost::vcs::computeLineChanges) and sets them again after edits.
 */
struct LineChangeMarker {
    LineIndex firstLine;
    std::uint32_t lineCount = 0;
    ChangeStripeKind kind = ChangeStripeKind::Modified;

    friend bool operator==(const LineChangeMarker&, const LineChangeMarker&) = default;
};

// A line breakpoint on buffer line `line` (frost::ui::DebugController fills these from the
// BreakpointManager; ADR 0050).
struct BreakpointMarker {
    LineIndex line;
    BreakpointKind kind = BreakpointKind::Verified;
    std::string message; // The tooltip: why it is unverified; "" when verified.

    friend bool operator==(const BreakpointMarker&, const BreakpointMarker&) = default;
};

/*
 * Facts about the file that the document itself does not hold, from whoever opened it (the save
 * manager's load service): the status bar's encoding token (text::formatStatus, "UTF-8 BOM"), the
 * line separator token ("LF", "CRLF", "CR"; empty: read from the text's first line break), whether
 * the file is read-only, and large-file mode (no soft wrap, no highlighting; frost/text/large_file.hpp).
 */
struct EditorFileInfo {
    std::string encoding = "UTF-8";
    std::string lineSeparator;
    bool readOnly = false;
    bool largeFile = false;

    friend bool operator==(const EditorFileInfo&, const EditorFileInfo&) = default;
};

/*
 * What the status bar shows for the editor (m1/shell-frame's EditorStatus has the same fields): the
 * primary caret's 1-based line and column (the column counts grapheme clusters), the caret count,
 * the selected characters (code points), the line separator and encoding tokens, the indentation,
 * read-only, and dirty (the undo state differs from the one markSaved() recorded).
 */
struct EditorViewStatus {
    int line = 1;
    int column = 1;
    int carets = 1;
    int selectedCharacters = 0;
    std::string lineSeparator = "LF";
    std::string encoding = "UTF-8";
    bool useTabs = false;
    int indentSize = 4;
    bool readOnly = false;
    bool dirty = false;

    friend bool operator==(const EditorViewStatus&, const EditorViewStatus&) = default;
};

// One InputToPaint sample (PLAN.md "Self-diagnostics"; B5 is keystroke to paint end): the steady
// time an input event was handled and the time the paint that showed it ended.
struct InputToPaintSample {
    IClock::SteadyTime input{};
    IClock::SteadyTime paintEnd{};
    std::uint64_t textVersion = 0;
};

/*
 * What a view uses besides its document; every pointer may be null and must outlive the view.
 *   languages + scheduler: tree-sitter highlighting and the outline (none without both);
 *   clock: the syntax session's and the InputToPaint marker's (null: systemClock());
 *   keys: key routing (null: the view types text from QKeyEvent::text() and runs no shortcuts);
 *   actionHost: without `keys`, the host the view makes itself active in on focus-in, so the
 *     shell's ShortcutDispatcher (the only key path in the workbench) runs editor actions on it;
 *   appearance: the theme and zoom, followed live, and the editor fonts (ensureEditorFonts()); when
 *     null the view follows FrostStyle::current(), or else paints with EditorColors::defaults();
 *   highlightBudget: the wall time one highlight query may take before it returns incomplete spans;
 *   renderer: the renderer rule's environment and fault hooks (tests; editor_backend.hpp).
 */
struct EditorServices {
    const syntax::LanguageRegistry* languages = nullptr;
    sched::IScheduler* scheduler = nullptr;
    const IClock* clock = nullptr;
    EditorKeyRouting* keys = nullptr;
    EditorActionHost* actionHost = nullptr;
    AppearanceManager* appearance = nullptr;
    syntax::SyntaxOptions syntax{};
    IClock::Duration highlightBudget = std::chrono::milliseconds(4);
    EditorRendererHooks renderer{};
};

/*
 * Which backend draws the view (ADR 0043): `active` draws now; `mode` is the editor.renderer setting;
 * `reason` is why (the rule's answer, or the failure the view fell back from); `pending` means the
 * QRhi backend was chosen and waits for the first raster frame (ADR 0007 decision 3: QRhi's start-up
 * stays off the first frame); `fallbacks` counts switches to raster after a QRhi failure.
 */
struct EditorRendererState {
    EditorBackend active = EditorBackend::Raster;
    EditorRendererMode mode = EditorRendererMode::Auto;
    std::string reason;
    bool pending = false;
    std::uint32_t fallbacks = 0;
};

/*
 * The editor widget for one Document. It borrows the document (which must outlive it) and owns its
 * Editor (selections, display map), shaper, renderer, syntax session and overlays. Several views
 * may show one document only once m1's split views follow each other's changes (Editor::follow);
 * TODO(M1): the W1 integration slice shares one SyntaxSession per document.
 *
 * Keys: with services.keys, KeyPress and ShortcutOverride go through EditorKeyRouting (the IntelliJ
 * keymap); Tab and Shift+Tab reach the editor rather than moving focus. Mouse: click places the
 * caret, drag selects, double click selects a word, triple click a line, Shift+click extends, a
 * click in the gutter selects the line (with setBreakpointGutterEnabled, a click left of the line
 * numbers asks for a breakpoint instead: breakpointToggleRequested), the wheel scrolls. IME: QInputMethodEvent preedit
 * goes into the document as a composition (Document::updateComposition) and the commit becomes one Typing step.
 *
 * Painting: paintEvent builds an EditorDisplayList for the visible rows (buildDisplayList) and hands
 * it to the renderer. Highlight spans are queried for the visible rows only and cached until the tree
 * or the rows change; while the worker reparses, the session's stale tree (edited to the current
 * text) keeps colouring. Budget: B5.
 *
 * Renderers (ADR 0043): the first frame is always raster. When editor.renderer's rule picks QRhi
 * (chooseEditorBackend), the view swaps its viewport for a RhiEditorViewport after that frame, and
 * back to a plain raster viewport when the setting changes, the QRhi cannot be created, the device
 * is lost or the adapter turns out to be WARP under "auto". A swap keeps the editor model (document,
 * selections, undo), the scroll position and the overlays; only the viewport widget is replaced, so
 * callers must not keep viewport() pointers across rendererChanged().
 *
 * Insight (ADR 0047). The workbench feeds the language server's and the build's results through
 * small seams: setSemanticTokens and setInactiveRanges (ranges of a given text version, mapped
 * through the edits made since), setDiagnostics per source (build, live), setDecorations per
 * owner, setUsageHighlights and setAnalyzing. Between an edit and fresh results every range
 * follows the text (Bias::Right at a range's start, Left at its end), so colours and squiggles stay
 * on the stale ranges until new ones arrive. Semantic colours win over tree-sitter's; inactive
 * regions are dimmed over both. Diagnostics draw squiggles (wavy for errors and warnings, dotted
 * for weak warnings and infos) with message tooltips, marks in the error stripe at the right edge
 * of the viewport (scaled to the document's rows; hover for the messages, click to jump; the
 * summary box at the top takes the worst severity's colour), and the counts of the inspection
 * widget in the top-right corner (a check when clean, a spinner while analysing, up/down arrows).
 * Everything is part of the display list, so every renderer draws it.
 *
 * GUI thread only. Not copyable.
 */
class EditorView final : public QAbstractScrollArea {
    Q_OBJECT

public:
    EditorView(text::Document& document, std::string path, EditorServices services = {},
               EditorViewSettings settings = {}, QWidget* parent = nullptr);
    ~EditorView() override;

    // The model.
    [[nodiscard]] text::Editor& editor() noexcept { return *m_editor; }
    [[nodiscard]] const text::Editor& editor() const noexcept { return *m_editor; }
    [[nodiscard]] text::Document& document() noexcept { return *m_document; }
    [[nodiscard]] const std::string& path() const noexcept { return m_path; }
    [[nodiscard]] const text::LanguageInfo& language() const noexcept { return *m_language; }
    [[nodiscard]] syntax::LanguageId syntaxLanguage() const noexcept { return m_syntaxLanguage; }
    // The syntax session, or nullptr without languages and scheduler.
    [[nodiscard]] syntax::SyntaxSession* syntaxSession() noexcept { return m_syntax.get(); }

    // Runs `action` (the keymap handlers call this). Returns false when it did not apply: an
    // editing action in a read-only editor, or a model error (logged in lastError()).
    bool perform(EditorAction action);
    // Types `utf8` at every caret with the smart keys (keyboard text). False when read-only.
    bool typeText(std::string_view utf8);
    // Applies a command result like perform does (repaint, highlight, scroll to the caret, status).
    bool apply(const text::CommandResult& result);
    // Follows an externally applied change to this document, updating display, caret and listeners.
    // The change must start at this editor's current version. GUI thread; errors are from Editor::follow.
    [[nodiscard]] Result<void> followExternalChange(const text::ChangeSet& change);
    // Moves the primary caret to 1-based `line` and `column` (clamped) as one caret.
    void goToLine(int line, int column);
    // Replaces the selections (a click elsewhere, a navigation target) and scrolls to the caret.
    void setState(text::EditorState state);
    // The last model error a command hit (Busy while composing, ...), for diagnostics.
    [[nodiscard]] const std::optional<Error>& lastError() const noexcept { return m_lastError; }

    // Settings and appearance.
    void applySettings(const EditorViewSettings& settings);
    [[nodiscard]] const EditorViewSettings& settings() const noexcept { return m_settings; }
    // Paints with `theme`'s Editor.* and Syntax.* colours from now on (null: the defaults).
    void setTheme(std::shared_ptr<const Theme> theme);
    [[nodiscard]] const EditorColors& colors() const noexcept { return m_colors; }
    [[nodiscard]] const QFont& editorFont() const noexcept { return m_shaper->font(); }
    [[nodiscard]] const QtTextShaper& shaper() const noexcept { return *m_shaper; }
    [[nodiscard]] double lineHeight() const noexcept { return m_lineHeight; }

    // File facts and status.
    void setFileInfo(const EditorFileInfo& info);
    [[nodiscard]] const EditorFileInfo& fileInfo() const noexcept { return m_fileInfo; }
    [[nodiscard]] bool isReadOnly() const noexcept { return m_fileInfo.readOnly; }
    [[nodiscard]] bool hasSelection() const noexcept;
    // Records the current undo state as the saved one (the dirty flag clears).
    void markSaved();
    [[nodiscard]] EditorViewStatus status() const;
    // ContextKeys for the shell while the editor has focus (EditorReadOnly, EditorHasSelection).
    [[nodiscard]] input::ContextKeys contextKeys() const noexcept;

    // Gutter markers (sorted by line on the way in; replaces the previous set).
    void setLineMarkers(std::vector<LineMarker> markers);
    [[nodiscard]] const std::vector<LineMarker>& lineMarkers() const noexcept { return m_markers; }
    // The messages of the markers on the line at viewport point `at` when it is in the gutter, one per
    // line (the gutter tooltip); empty elsewhere.
    [[nodiscard]] QString lineMarkerToolTip(QPointF at) const;

    // Line breakpoints (sorted by line on the way in; replaces the previous set). A line with a
    // breakpoint or the execution point shows it in place of its number (IntelliJ's new UI).
    void setBreakpointMarkers(std::vector<BreakpointMarker> markers);
    [[nodiscard]] const std::vector<BreakpointMarker>& breakpointMarkers() const noexcept { return m_breakpoints; }
    // The debugger's execution point (the stopped frame's line), or nullopt.
    void setExecutionPoint(std::optional<LineIndex> line);
    [[nodiscard]] std::optional<LineIndex> executionPoint() const noexcept { return m_executionPoint; }
    // Whether a click in the gutter's breakpoint area emits breakpointToggleRequested (off: it
    // selects the line, as before). The breakpoint area is the gutter left of the marker column.
    void setBreakpointGutterEnabled(bool enabled) noexcept { m_breakpointGutter = enabled; }
    [[nodiscard]] bool breakpointGutterEnabled() const noexcept { return m_breakpointGutter; }
    // The buffer line under viewport point `at` when it lies in the breakpoint area, else nullopt.
    [[nodiscard]] std::optional<LineIndex> breakpointLineAt(QPointF at) const;

    // --- Insight (M3, ADR 0047; see the class comment) ---
    /*
     * Replaces the semantic tokens with `spans` (sorted on the way in), computed for the text of
     * `textVersion`. Spans of an older version are mapped through the edits made since; returns
     * false and keeps the current tokens when that version is unknown (older than the last
     * kEditHistory edits or newer than the text). O(spans × edits since).
     */
    bool setSemanticTokens(std::uint64_t textVersion, std::vector<SemanticSpan> spans);
    [[nodiscard]] const std::vector<SemanticSpan>& semanticTokens() const noexcept { return m_semantic; }
    // Replaces the inactive preprocessor regions (dimmed text), with setSemanticTokens' rules.
    bool setInactiveRanges(std::uint64_t textVersion, std::vector<text::ByteRange> ranges);
    [[nodiscard]] const std::vector<text::ByteRange>& inactiveRanges() const noexcept { return m_inactive; }
    // Replaces the diagnostics of `source` (EditorDiagnostic says how their ranges are resolved).
    void setDiagnostics(EditorDiagnosticSource source, std::vector<EditorDiagnostic> diagnostics);
    // Both sources' diagnostics in the current text, sorted by start, then severity. A build
    // diagnostic on the same line with the same message as a live one is listed once (the live one).
    [[nodiscard]] std::vector<ActiveDiagnostic> diagnostics() const;
    // Replaces the decorations of `owner` (any key), given in current-text offsets.
    void setDecorations(std::string owner, std::vector<EditorDecoration> decorations);
    // Every owner's decorations, by owner then in the order given.
    [[nodiscard]] std::vector<EditorDecoration> decorations() const;
    // Replaces the highlighted usages (background boxes and stripe marks), in current-text offsets.
    void setUsageHighlights(std::vector<UsageHighlight> usages);
    [[nodiscard]] const std::vector<UsageHighlight>& usageHighlights() const noexcept { return m_usages; }
    // Whether the analysis of the current text still runs (the inspection widget's spinner).
    void setAnalyzing(bool analyzing);
    // Shows the inspection widget without any diagnostics yet (any insight setter also does).
    void setInsightEnabled(bool enabled);
    [[nodiscard]] bool insightEnabled() const noexcept { return m_insightEnabled; }
    [[nodiscard]] InsightSummary insightSummary() const;
    /*
     * F2 / Shift+F2 (navigate.nextError, navigate.previousError): moves the caret to the start of
     * the next (previous) problem after (before) it, wrapping at the ends, among the most serious
     * severity present (IntelliJ visits errors before warnings), scrolls it into view and shows its
     * message in a balloon at the caret. False (and nothing moves) when there is no problem.
     */
    bool gotoNextProblem(bool forward = true);
    // The balloon's text, empty when none shows (it hides on an edit, a caret move or Esc).
    [[nodiscard]] QString problemBalloonText() const;
    // The tooltip at viewport point `at`: the gutter markers' messages, the stripe marks' under it,
    // the inspection widget's summary, or the messages of the diagnostics and decorations there.
    [[nodiscard]] QString toolTipAt(QPointF at) const;
    // A left click at viewport point `at` on the stripe (jump to the mark under it, else scroll
    // there; the summary box goes to the next problem) or the inspection widget (the arrows go to
    // the previous / next problem, the counts emit problemsViewRequested). False elsewhere.
    bool clickInsight(QPointF at);
    // The error stripe's width in pixels at the current zoom.
    [[nodiscard]] double stripeWidth() const noexcept;
    // The edit batches kept for mapping older versions' ranges.
    static constexpr std::size_t kEditHistory = 256;

    // VCS change markers (sorted by firstLine on the way in; replaces the previous set). A click on
    // a stripe emits lineChangeClicked instead of selecting the line.
    void setLineChanges(std::vector<LineChangeMarker> changes);
    [[nodiscard]] const std::vector<LineChangeMarker>& lineChanges() const noexcept { return m_lineChanges; }
    // The index into lineChanges() of the marker whose stripe is under viewport point `at` (the
    // stripe column, a few pixels of slack, on one of the marker's lines), or nullopt. O(log n).
    [[nodiscard]] std::optional<std::size_t> lineChangeAt(QPointF at) const;
    // The stripe column: x and width in viewport pixels (y spans the viewport).
    [[nodiscard]] QRectF changeStripeColumn() const noexcept;

    /*
     * Follows a change made to the document outside any editor (a reload from disk, an
     * external-change decision, a restored journal entry), which reports no ChangeSet: rebuilds the
     * model at the document's version with the primary caret kept (clamped), reparses and repaints.
     * The insight ranges (semantic tokens, inactive regions, diagnostics, decorations, usages) stay,
     * clamped to the new text, until their owners replace them; an older version's ranges are no
     * longer mapped (editsSince sees the gap). Undo history is the document's and is unaffected.
     * Emits no textChanged.
     */
    void reloadFromDocument();

    // Find / replace and Go to Line.
    void openFind(bool replace);
    void closeFind();
    [[nodiscard]] EditorFindBar* findBar() const noexcept { return m_findBar; }
    [[nodiscard]] bool findBarVisible() const noexcept;
    bool findNext();
    bool findPrevious();
    bool replaceNext();
    std::size_t replaceAll();
    // The matches of the bar's query in the current text (empty without a valid query).
    [[nodiscard]] const std::vector<text::ByteRange>& searchMatches();
    void openGoToLine();
    [[nodiscard]] GoToLinePopup* goToLinePopup() const noexcept { return m_goToLine; }

    // Syntax: collects a finished worker parse (the session's wake-up calls this through a queued
    // call; tests call it directly). Returns true when the highlights changed.
    bool pollSyntax();

    // The display list of the current frame (what paintEvent hands the renderer).
    [[nodiscard]] EditorDisplayList buildDisplayList();
    // The highlight spans for `range` of the current text (the cached query paintEvent uses).
    [[nodiscard]] std::vector<syntax::HighlightSpan> highlightsFor(text::ByteRange range);
    [[nodiscard]] std::uint64_t paintCount() const noexcept { return m_paintCount; }
    [[nodiscard]] std::uint32_t firstVisibleRow() const noexcept;
    [[nodiscard]] std::uint32_t visibleRowCount() const noexcept;
    [[nodiscard]] IEditorRenderer& renderer() noexcept { return *m_renderer; }
    // The renderer choice and why it was made.
    [[nodiscard]] const EditorRendererState& rendererState() const noexcept { return m_rendererState; }
    // Switches to the raster backend now and remembers `reason`, so the QRhi backend is not tried
    // again until editor.renderer changes. The QRhi viewport's failures come here (queued).
    void fallBackToRaster(std::string reason);
    // Whether the blinking caret is in its visible phase.
    [[nodiscard]] bool caretVisible() const noexcept { return m_caretOn; }
    // Geometry for overlays (the language features' popups, Ctrl+click): the text offset under the
    // viewport point `at`, and the caret box at `offset` (clamped to the text) in global coordinates.
    [[nodiscard]] ByteOffset offsetAt(QPointF at) const;
    [[nodiscard]] QRect globalCaretRect(ByteOffset offset) const;

    // The InputToPaint marker's sink (none by default): called after every paint that follows a
    // handled input event. TODO(M1): the hardening phase feeds the ETW provider and B5 from it.
    void setInputToPaintSink(std::function<void(const InputToPaintSample&)> sink);

    // The IME's queries: the caret rectangle (viewport-offset widget coordinates), the font, the
    // caret's line as surrounding text with UTF-16 cursor and anchor positions, the selection.
    [[nodiscard]] QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

signals:
    void caretPositionChanged(int line, int column);
    void selectionChanged(int carets, int selectedCharacters);
    void encodingChanged(const QString& encoding);
    void lineSeparatorChanged(const QString& separator);
    void readOnlyChanged(bool readOnly);
    void indentStyleChanged(bool useTabs, int size);
    void modifiedChanged(bool modified);
    // Any status() field changed.
    void statusChanged();
    // The renderer backend changed (rendererState().active); viewport() is a new widget.
    void rendererChanged();
    void breakpointToggleRequested(int line);
    // insightSummary() or insightEnabled() changed (the status-bar indicator follows it).
    void insightChanged();
    // The text changed to `version` (the language bridge schedules its requests from this).
    void documentEdited(quint64 version);
    // The inspection widget's counts were clicked (the workbench shows the Problems tool window).
    void problemsViewRequested();
    /*
     * The document's text changed through this view (typing, commands, undo and redo, the IME):
     * the document's ChangeSet, for language servers (W1 feeds ClangdService::changeDocument).
     * Receivers that need no ChangeSet connect a slot without arguments (the VCS markers).
     */
    void textChanged(const frost::text::ChangeSet& change);
    // A click on a VCS change stripe: the index into lineChanges() and the click's global position.
    void lineChangeClicked(int index, QPoint globalPos);
    // A Ctrl+click (Cmd+click on macOS) put the caret at `offset`: Rider's go to declaration.
    void ctrlClicked(frost::ByteOffset offset);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void changeEvent(QEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;
    bool focusNextPrevChild(bool next) override;
    bool viewportEvent(QEvent* event) override;

private:
    // How a mouse drag extends: by character, word or line (single, double, triple click).
    enum class Granularity : std::uint8_t { Character, Word, Line };

    // Construction and settings.
    void rebuildFont();
    void updateWrap();
    void updateMetrics();
    void updateScrollBars();
    void updateOverlayGeometry();
    void restartBlink();
    // Renderer choice (editor_view_renderer.cpp).
    void configureViewport();
    void selectRenderer();
    void attachRhi();
    void useRaster(std::string reason);
    void installViewport(QWidget* viewport);
    [[nodiscard]] double zoomFactor() const noexcept;
    void followAppearance();

    // Model plumbing.
    bool applyUpdate(Result<text::EditorUpdate> update);
    void afterChange(const text::EditorUpdate& update);
    void ensureCaretVisible();
    void emitStatus();
    [[nodiscard]] text::CommandInput commandInput() const;
    bool move(text::Movement movement, bool extend);
    void copySelection();
    void cutSelection();
    void pasteClipboard();
    void escape();
    void scrollRows(int rows);
    [[nodiscard]] std::string lineBreakText() const;
    void armInputToPaint();

    // Search.
    void onQueryChanged();
    [[nodiscard]] std::optional<text::SearchPattern> currentPattern() const;
    void refreshMatchLabel();

    // Geometry.
    // The index of the change marker on buffer line `line` (a Deleted one only on its own line).
    [[nodiscard]] std::optional<std::size_t> lineChangeOnLine(LineIndex line) const noexcept;
    [[nodiscard]] double textOriginX() const noexcept;
    [[nodiscard]] double gutterWidth() const noexcept;
    [[nodiscard]] text::DisplayHit hitAt(QPointF point) const;
    [[nodiscard]] QRectF caretRect(ByteOffset offset, text::Bias bias) const;
    [[nodiscard]] text::ByteRange wordAt(ByteOffset offset) const;
    [[nodiscard]] text::ByteRange lineAt(ByteOffset offset) const;
    // One selection, its caret at `selection.head` with `bias`.
    void select(text::Selection selection, text::Bias bias);
    // The selection a drag from m_dragOrigin to the unit under `hit` covers.
    void dragTo(const text::DisplayHit& hit);

    // IME.
    void finishComposition(const std::string& commit);
    // Follows a Document change made outside the Editor (the IME's), then places a caret.
    void followDocument(Result<text::ChangeSet> change, std::optional<ByteOffset> caret);
    // Marks the start of an input event for the InputToPaint marker.
    void beginInput();

    // Insight (editor_view_insight.cpp).
    struct EditRecord {
        std::uint64_t baseVersion = 0;
        std::uint64_t version = 0;
        std::vector<text::RawEdit> edits;
    };
    // The batches that take the text of `version` to the current one, oldest first; nullopt when
    // `version` is unknown.
    [[nodiscard]] std::optional<std::vector<const EditRecord*>> editsSince(std::uint64_t version) const;
    // Records `change` and moves every insight range through it.
    void followInsight(const text::ChangeSet& change);
    // A build diagnostic's 1-based line and column resolved in the current text.
    [[nodiscard]] text::ByteRange resolvePosition(int line, int column) const;
    void insightUpdated();
    struct StripeEntry {
        StripeMarkKind kind = StripeMarkKind::Error;
        text::ByteRange range;
        std::uint32_t row = 0;
        QString message;
    };
    struct StripeGeometry {
        QRectF stripe;
        QRectF summary;
        double top = 0.0;
        double scale = 1.0;
        double markHeight = 2.0;
    };
    [[nodiscard]] StripeGeometry stripeGeometry() const;
    [[nodiscard]] std::vector<StripeEntry> stripeEntries() const;
    // The stripe entries whose marks lie within a few pixels of y, most serious first.
    [[nodiscard]] std::vector<StripeEntry> stripeEntriesAt(double y) const;
    struct InspectionItem {
        InsightSeverity severity = InsightSeverity::Error;
        QRectF icon;
        QPointF textOrigin;
        QString count;
    };
    struct InspectionLayout {
        InspectionState state = InspectionState::Hidden;
        QRectF rect;
        QRectF badge; // The check or the spinner.
        std::vector<InspectionItem> items;
        QRectF previous;
        QRectF next;
    };
    [[nodiscard]] InspectionLayout inspectionLayout() const;
    // Whether viewport point `at` is on the error stripe or the inspection widget.
    [[nodiscard]] bool insightContains(QPointF at) const;
    // Adds the stripe, the inspection widget and the balloon to `list`.
    void addInsightChrome(EditorDisplayList& list) const;
    // The text-area decorations of one frame (diagnostics, deprecated tokens, usages, owners').
    struct FrameDecoration {
        text::ByteRange range;
        DecorationStyle style = DecorationStyle::WavyUnderline;
        QColor color;
        std::optional<InsightSeverity> severity;
        QString tooltip;
    };
    [[nodiscard]] std::vector<FrameDecoration> frameDecorations(text::ByteRange visible) const;

    text::Document* m_document;
    std::string m_path;
    EditorServices m_services;
    const IClock* m_clock;
    EditorViewSettings m_settings;
    EditorFileInfo m_fileInfo;
    EditorColors m_colors;
    std::shared_ptr<const Theme> m_theme;
    const text::LanguageInfo* m_language;
    syntax::LanguageId m_syntaxLanguage = syntax::LanguageId::None;

    // Declaration order matters: the editor borrows the shaper, the outline the session.
    std::unique_ptr<QtTextShaper> m_shaper;
    std::unique_ptr<text::Editor> m_editor;
    std::unique_ptr<IEditorRenderer> m_renderer;
    // The QRhi viewport while the QRhi backend draws (the scroll area owns it); null on raster.
    QPointer<RhiEditorViewport> m_rhiViewport;
    EditorRendererState m_rendererState;
    // Why the QRhi backend failed in this view; it is not tried again until editor.renderer changes.
    std::optional<std::string> m_rhiFailure;
    bool m_rhiAttachQueued = false;
    std::unique_ptr<syntax::SyntaxSession> m_syntax;
    std::unique_ptr<SyntaxOutlineProvider> m_outline;

    double m_lineHeight = 16.0;
    double m_ascent = 12.0;
    double m_maxRowWidth = 0.0;
    std::optional<Error> m_lastError;

    // Highlights of the cached range for one tree.
    struct HighlightCache {
        const void* tree = nullptr;
        std::uint64_t version = 0;
        text::ByteRange range;
        std::vector<syntax::HighlightSpan> spans;
        bool valid = false;
    };
    HighlightCache m_highlights;

    // Search state.
    EditorFindBar* m_findBar = nullptr;
    ByteOffset m_findOrigin;
    std::vector<text::ByteRange> m_matches;
    std::uint64_t m_matchesVersion = ~std::uint64_t{0};
    QString m_matchesQuery;
    text::SearchOptions m_matchesOptions;
    GoToLinePopup* m_goToLine = nullptr;

    std::vector<LineMarker> m_markers;
    std::vector<BreakpointMarker> m_breakpoints;
    std::optional<LineIndex> m_executionPoint;
    bool m_breakpointGutter = false;
    std::vector<LineChangeMarker> m_lineChanges;

    // Insight (ADR 0047): every range is in the current text.
    std::vector<SemanticSpan> m_semantic;
    std::vector<text::ByteRange> m_inactive;
    std::array<std::vector<ActiveDiagnostic>, 2> m_diagnostics; // By EditorDiagnosticSource.
    std::map<std::string, std::vector<EditorDecoration>, std::less<>> m_decorations;
    std::vector<UsageHighlight> m_usages;
    std::deque<EditRecord> m_editHistory;
    bool m_analyzing = false;
    bool m_insightEnabled = false;
    InsightSummary m_lastSummary;
    bool m_lastInsightEnabled = false;
    struct Balloon {
        text::ByteRange range;
        InsightSeverity severity = InsightSeverity::Error;
        QString text;
        ByteOffset caret;
    };
    std::optional<Balloon> m_balloon;
    QTimer m_spinner;
    int m_spinnerPhase = 0;

    // Caret blink (Rider: 500 ms phases, solid while typing).
    QTimer m_blink;
    bool m_caretOn = true;
    bool m_focused = false;

    // Mouse.
    Granularity m_granularity = Granularity::Character;
    text::ByteRange m_dragOrigin;
    bool m_dragging = false;
    std::chrono::steady_clock::time_point m_lastDoubleClick{};
    QPointF m_lastDoubleClickPos;

    // IME composition: the base range it replaces on commit (the primary selection when it
    // started; it starts at the range's begin) and the selections before it.
    bool m_composing = false;
    text::ByteRange m_compositionBase;
    text::SelectionSet m_compositionSelections;

    // Status.
    EditorViewStatus m_lastStatus;
    std::uint64_t m_savedStateId = 0;

    std::uint64_t m_paintCount = 0;
    std::optional<IClock::SteadyTime> m_inputStart;
    std::optional<IClock::SteadyTime> m_pendingInput;
    std::function<void(const InputToPaintSample&)> m_inputToPaint;
};

/*
 * The editor factory the shell's IEditorFactory wraps (m1/shell-frame, ADR 0038): a view on
 * `document` for the file at `path` (absolute UTF-8, or empty for an untitled document) with the
 * language picked from the file name and highlighting started. The document must outlive the view.
 */
[[nodiscard]] std::unique_ptr<EditorView> createEditorView(text::Document& document, std::string path,
                                                           EditorServices services = {},
                                                           EditorViewSettings settings = {}, QWidget* parent = nullptr);

} // namespace frost::ui
