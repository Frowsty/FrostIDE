// SPDX-License-Identifier: Apache-2.0
//
// VtScreen: the terminal's screen model over the vendored libvterm 0.3.3 (ADR 0051). It parses the
// child's output into a grid of cells with scrollback, tracks the cursor, the alternate screen, the
// title, the bell and the modes the program set, and encodes keys, pastes and mouse events into the
// bytes the program expects back. This is PLAN.md's IVtEngine seam: no libvterm type appears here.
/*
 * Coordinates. Rows are 0-based from the top of the visible screen; scrollback rows are negative,
 * -1 being the line that scrolled off most recently, down to -scrollbackSize(). Columns are 0-based.
 *
 * Output. Everything the terminal sends to the child (query answers such as the cursor position
 * report, encoded keys, pastes and mouse reports) accumulates in an outgoing buffer that takeOutput()
 * empties; the session writes it to the PTY.
 *
 * Thread safety: none; one owner at a time (TerminalSession guards it with its mutex).
 * Complexity: feed() is linear in its input; cell() and line() are O(1) and O(columns).
 */
#pragma once

#include "frost/term/keys.hpp"
#include "frost/term/pty.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace frost::term {

// A cell colour: the terminal's default, one of the 256 palette entries, or 24-bit RGB.
struct CellColor {
    enum class Kind : std::uint8_t { Default, Indexed, Rgb };
    Kind kind = Kind::Default;
    std::uint8_t index = 0; // Kind::Indexed
    std::uint8_t red = 0;   // Kind::Rgb
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    [[nodiscard]] static constexpr CellColor indexed(std::uint8_t value) noexcept {
        return CellColor{Kind::Indexed, value, 0, 0, 0};
    }
    [[nodiscard]] static constexpr CellColor rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept {
        return CellColor{Kind::Rgb, 0, r, g, b};
    }
    friend constexpr bool operator==(const CellColor&, const CellColor&) = default;
};

enum class Underline : std::uint8_t { None, Single, Double, Curly };

// SGR attributes of a cell.
struct CellStyle {
    bool bold = false;
    bool italic = false;
    bool blink = false;
    bool inverse = false;
    bool strike = false;
    bool conceal = false;
    Underline underline = Underline::None;

    friend constexpr bool operator==(const CellStyle&, const CellStyle&) = default;
};

/*
 * One cell. `text` is the cell's character with its combining marks in UTF-8, empty for a blank cell
 * and for the right half of a wide character. `width` is 1, 2 for a wide character's left half, and 0
 * for its right half (whose text is empty).
 */
struct Cell {
    std::string text;
    std::uint8_t width = 1;
    CellColor foreground;
    CellColor background;
    CellStyle style;

    friend bool operator==(const Cell&, const Cell&) = default;
};

enum class CursorShape : std::uint8_t { Block, Underline, Bar };

struct CursorState {
    int row = 0;
    int column = 0;
    bool visible = true;
    bool blinking = true;
    CursorShape shape = CursorShape::Block;

    friend constexpr bool operator==(const CursorState&, const CursorState&) = default;
};

// Which mouse events the program asked for (DECSET 1000, 1002, 1003).
enum class MouseMode : std::uint8_t { None, Click, Drag, Move };

enum class MouseButton : std::uint8_t { Left = 1, Middle = 2, Right = 3, WheelUp = 4, WheelDown = 5 };

// A position for selections and text extraction.
struct ScreenPos {
    int row = 0;
    int column = 0;

    friend constexpr bool operator==(const ScreenPos&, const ScreenPos&) = default;
    friend constexpr auto operator<=>(const ScreenPos&, const ScreenPos&) = default;
};

// What happened since the last takeEvents().
struct ScreenEvents {
    bool changed = false;      // Cells, the cursor or a mode changed.
    bool titleChanged = false; // OSC 0 or 2.
    int bells = 0;             // BEL characters.
    // Lines that moved into the scrollback (for a view scrolled back, which keeps its place).
    std::size_t linesScrolledOff = 0;
};

class VtScreen {
public:
    // A screen of `size` keeping up to `scrollbackLines` lines of history (0: none).
    VtScreen(PtySize size, std::size_t scrollbackLines);
    ~VtScreen();
    VtScreen(const VtScreen&) = delete ("owns a libvterm instance");
    VtScreen& operator=(const VtScreen&) = delete ("owns a libvterm instance");

    // --- Output of the child ---

    // Parses `bytes` (UTF-8 and escape sequences; a sequence may be split across calls).
    void feed(std::string_view bytes);
    // Changes the size; lines reflow and move to or from the scrollback as libvterm decides.
    void resize(PtySize size);

    // --- The grid ---

    [[nodiscard]] PtySize size() const noexcept { return m_size; }
    [[nodiscard]] int rows() const noexcept { return m_size.rows; }
    [[nodiscard]] int columns() const noexcept { return m_size.columns; }
    [[nodiscard]] std::size_t scrollbackSize() const noexcept;
    [[nodiscard]] std::size_t scrollbackLimit() const noexcept { return m_scrollbackLimit; }
    // How many lines ever moved into the scrollback (monotonic; a view scrolled back adds the change
    // to its offset to keep its place).
    [[nodiscard]] std::uint64_t linesPushed() const noexcept { return m_linesPushed; }
    // The cell at (row, column); a blank cell outside the grid or the scrollback.
    [[nodiscard]] Cell cell(int row, int column) const;
    // The cells of `row`, columns() of them (scrollback lines narrower than the screen are padded).
    [[nodiscard]] std::vector<Cell> line(int row) const;
    // The text of `row` without trailing blanks.
    [[nodiscard]] std::string lineText(int row) const;
    /*
     * The text from `from` to `to` (inclusive of both cells, in reading order whichever comes first),
     * rows joined with '\n', each row's trailing blanks dropped. For copying a selection.
     */
    [[nodiscard]] std::string text(ScreenPos from, ScreenPos to) const;
    // The visible screen's text, rows joined with '\n' (tests and diagnostics).
    [[nodiscard]] std::string screenText() const;

    // --- State ---

    [[nodiscard]] CursorState cursor() const noexcept { return m_cursor; }
    [[nodiscard]] bool altScreen() const noexcept { return m_altScreen; }
    [[nodiscard]] const std::string& title() const noexcept { return m_title; }
    [[nodiscard]] MouseMode mouseMode() const noexcept { return m_mouseMode; }
    // DECSCNM: the whole screen in inverse video (already applied to the cells' inverse style).
    [[nodiscard]] bool reverseVideo() const noexcept { return m_reverse; }
    // The events since the last call, then cleared.
    [[nodiscard]] ScreenEvents takeEvents() noexcept;

    // --- Input to the child (into the outgoing buffer) ---

    // A native key record, encoded only when DECSET 9001 is active; returns whether consumed.
    bool sendWin32Key(const Win32KeyEvent& event);
    // A non-text key, encoded for the modes the program set (application cursor keys, ...).
    void sendKey(TermKey key, KeyMods mods);
    // A typed character: plain text as UTF-8, Ctrl+letter as its control code, Alt as an ESC prefix.
    void sendChar(char32_t character, KeyMods mods);
    // Typed or committed text (an IME commit), sent as it is.
    void sendText(std::string_view utf8);
    // Pasted text: line breaks (CR LF, LF) become CR, an embedded end marker ESC [201~ is removed, and
    // the paste is bracketed (ESC [200~ ... ESC [201~) when the program enabled bracketed paste
    // (DECSET 2004).
    void paste(std::string_view utf8);
    // A mouse event at a cell; nothing is sent unless the program asked for that kind of event.
    void mouseMove(int row, int column, KeyMods mods);
    void mouseButton(MouseButton button, bool pressed, int row, int column, KeyMods mods);
    // Focus changes, reported when the program enabled focus reporting (DECSET 1004).
    void focusChanged(bool focused);
    // The bytes to write to the child, then cleared.
    [[nodiscard]] std::string takeOutput();

private:
    struct Impl;
    friend struct Impl;

    PtySize m_size;
    std::size_t m_scrollbackLimit;
    CursorState m_cursor;
    bool m_altScreen = false;
    bool m_reverse = false;
    MouseMode m_mouseMode = MouseMode::None;
    std::string m_title;
    ScreenEvents m_events;
    std::uint64_t m_linesPushed = 0;
    std::unique_ptr<Impl> m_impl;
};

} // namespace frost::term
