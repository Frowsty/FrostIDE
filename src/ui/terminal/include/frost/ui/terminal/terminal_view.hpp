// SPDX-License-Identifier: Apache-2.0
//
// TerminalView: the widget of one terminal tab (ADR 0051). It runs a frost::term::TerminalSession,
// paints its cell grid with the editor font and the theme's colours, and turns keys, IME commits,
// pastes, the mouse and the wheel into terminal input.
/*
 * Keys. While it has focus the view keeps the keys of frost::term's "terminal keeps these keys" table
 * (KeyKeeper, so the shell's shortcut dispatcher lets them through): Ctrl+C sends ^C, unless text is
 * selected, when it copies; Ctrl+Shift+C always copies; Ctrl+V, Ctrl+Shift+V and Shift+Insert paste
 * (bracketed when the program asked). Tab stays in the terminal (no focus chain). Typing scrolls
 * back to the bottom.
 *
 * Mouse. Dragging selects (a double click selects a word); when the program asked for mouse events
 * (DECSET 1000/1002/1003) presses, releases, drags and the wheel go to it instead, and Shift+drag
 * still selects. Without mouse reporting the wheel scrolls the scrollback (on the alternate screen
 * it sends Up/Down, as xterm's alternateScroll does).
 *
 * Threads. The session's notifications arrive on its threads and are posted to this widget; the
 * view only reads the screen through TerminalSession::read. GUI thread only. Destruction closes the
 * session, which ends the shell's process tree.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/process.hpp"
#include "frost/term/pty.hpp"
#include "frost/term/session.hpp"
#include "frost/ui/shell/key_keeper.hpp"
#include "frost/ui/terminal/terminal_colors.hpp"

#include <QFont>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>

class QScrollBar;

namespace frost::ui {

class TerminalView final : public QWidget, public KeyKeeper {
    Q_OBJECT

public:
    explicit TerminalView(QWidget* parent = nullptr);
    ~TerminalView() override;
    TerminalView(const TerminalView&) = delete ("QObject instances have identity and cannot be copied");
    TerminalView& operator=(const TerminalView&) = delete ("QObject instances have identity and cannot be copied");

    /*
     * Starts `spec` on a terminal of the view's current grid size from `launcher`, keeping
     * `scrollbackLines` lines of history. Once only. Errors: those of TerminalSession::start;
     * AlreadyExists when a session runs.
     */
    [[nodiscard]] Result<void> start(term::IPtyLauncher& launcher, const io::ProcessSpec& spec,
                                     std::size_t scrollbackLines);
    // The session, or nullptr before start().
    [[nodiscard]] term::TerminalSession* session() const noexcept { return m_session.get(); }

    void setColors(const TerminalColors& colors);
    [[nodiscard]] const TerminalColors& colors() const noexcept { return m_colors; }
    // The editor font: `family` at `pointSize` (fixed pitch).
    void setFontSettings(const QString& family, double pointSize);
    [[nodiscard]] const QFont& terminalFont() const noexcept { return m_font; }

    // The grid the widget's size holds (at least 1 x 1).
    [[nodiscard]] term::PtySize gridSize() const noexcept { return m_grid; }
    // One cell in logical pixels.
    [[nodiscard]] QSizeF cellSize() const noexcept { return {m_cellWidth, m_cellHeight}; }

    // --- Selection and the clipboard ---

    [[nodiscard]] bool hasSelection() const noexcept { return m_selection.has_value(); }
    // The selected text (rows joined with '\n'), "" without a selection.
    [[nodiscard]] QString selectedText() const;
    // Selects from `from` to `to` (screen coordinates, scrollback rows negative).
    void select(term::ScreenPos from, term::ScreenPos to);
    void selectAll();
    void clearSelection();
    // Copies the selection to the clipboard; false without one.
    bool copySelection();
    // Pastes the clipboard's text into the terminal.
    void pasteClipboard();

    // --- Scrollback ---

    // How many lines the view is scrolled back (0: the live screen).
    [[nodiscard]] int scrollOffset() const noexcept { return m_scrollOffset; }
    void setScrollOffset(int lines);
    void scrollToBottom() { setScrollOffset(0); }

    // The title the program set (OSC 0/2), "" until then.
    [[nodiscard]] const QString& title() const noexcept { return m_title; }

    // The terminal keeps `event` (see the header comment and frost/term/keys.hpp).
    [[nodiscard]] bool keepsKey(const QKeyEvent& event) const override;

signals:
    void titleChanged(const QString& title);
    void bellRang();
    // The shell exited with `exitCode`.
    void exited(int exitCode);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    [[nodiscard]] QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    // Tab and Shift+Tab are terminal keys, never focus moves.
    bool focusNextPrevChild(bool next) override;

private:
    void onScreenChanged();
    void updateMetrics();
    void updateGrid();
    void updateScrollBar();
    // The screen cell under `point` (scrollback rows negative), clamped to the grid.
    [[nodiscard]] term::ScreenPos cellAt(QPointF point) const;
    // Whether mouse events go to the program (it asked, and Shift is not held).
    [[nodiscard]] bool reportsMouse(Qt::KeyboardModifiers modifiers) const;
    [[nodiscard]] QRectF cursorRect() const;

    std::unique_ptr<term::TerminalSession> m_session;
    std::map<std::uint32_t, term::Win32KeyEvent> m_nativeKeysDown;
    TerminalColors m_colors;
    QFont m_font;
    double m_cellWidth = 8;
    double m_cellHeight = 16;
    double m_ascent = 12;
    term::PtySize m_grid;
    QScrollBar* m_scrollBar = nullptr;
    int m_scrollOffset = 0;
    std::uint64_t m_linesPushed = 0;
    // The selection's anchor and moving end, in screen coordinates.
    struct Selection {
        term::ScreenPos anchor;
        term::ScreenPos end;
    };
    std::optional<Selection> m_selection;
    bool m_selecting = false;
    // A mouse button the program was told about (pressed), for its release.
    std::optional<term::MouseButton> m_reportedButton;
    QString m_title;
    QString m_preedit;
};

} // namespace frost::ui
