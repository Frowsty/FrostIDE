// SPDX-License-Identifier: Apache-2.0
//
// EditorView (PLAN.md section 2 "Editor rendering"; ADR 0037): FrostIDE's text editor widget, a
// QAbstractScrollArea over a frost::text::Editor (selections, commands, undo) and its DisplayMap,
// painted through the backend-neutral display list and IEditorRenderer, with tree-sitter
// highlighting, the gutter, find/replace, Go to Line, IME, mouse selection and the editor actions.
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
#include "frost/ui/editor/editor_colors.hpp"
#include "frost/ui/editor/editor_renderer.hpp"
#include "frost/ui/editor/editor_settings.hpp"
#include "frost/ui/editor/qt_text_shaper.hpp"
#include "frost/ui/editor/syntax_outline.hpp"
#include "frost/ui/model/theme.hpp"

#include <QAbstractScrollArea>
#include <QPointF>
#include <QString>
#include <QTimer>

#include <chrono>
#include <cstdint>
#include <functional>
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

// A gutter marker: an error or warning on buffer line `line` (W1's error list fills these).
struct LineMarker {
    LineIndex line;
    MarkerSeverity severity = MarkerSeverity::Error;
    std::string message; // The tooltip.

    friend bool operator==(const LineMarker&, const LineMarker&) = default;
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
 *   appearance: the theme and zoom, followed live, and the editor fonts (ensureEditorFonts()); when
 *     null the view follows FrostStyle::current(), or else paints with EditorColors::defaults();
 *   highlightBudget: the wall time one highlight query may take before it returns incomplete spans.
 */
struct EditorServices {
    const syntax::LanguageRegistry* languages = nullptr;
    sched::IScheduler* scheduler = nullptr;
    const IClock* clock = nullptr;
    EditorKeyRouting* keys = nullptr;
    AppearanceManager* appearance = nullptr;
    syntax::SyntaxOptions syntax{};
    IClock::Duration highlightBudget = std::chrono::milliseconds(4);
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
 * click in the gutter selects the line, the wheel scrolls. IME: QInputMethodEvent preedit goes into
 * the document as a composition (Document::updateComposition) and the commit becomes one Typing step.
 *
 * Painting: paintEvent builds an EditorDisplayList for the visible rows (buildDisplayList) and hands
 * it to the renderer. Highlight spans are queried for the visible rows only and cached until the tree
 * or the rows change; while the worker reparses, the session's stale tree (edited to the current
 * text) keeps colouring. Budget: B5.
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
    // Whether the blinking caret is in its visible phase.
    [[nodiscard]] bool caretVisible() const noexcept { return m_caretOn; }

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
