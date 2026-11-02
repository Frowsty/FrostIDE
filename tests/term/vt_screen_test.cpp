// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for frost::term::VtScreen fed recorded VT sequences: text and cursor movement, SGR
// colours (16, 256 and truecolor) and attributes, wide characters, the alternate screen, scrollback
// with its cap, the title, the bell, cursor shapes, query answers, resizing and text extraction.

#include "frost/term/vt_screen.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using frost::term::CellColor;
using frost::term::CursorShape;
using frost::term::MouseButton;
using frost::term::MouseMode;
using frost::term::PtySize;
using frost::term::ScreenPos;
using frost::term::Underline;
using frost::term::VtScreen;

TEST(VtScreen, PlainTextAndCursor) {
    VtScreen screen(PtySize{20, 5}, 100);
    screen.feed("hello\r\nworld");
    EXPECT_EQ(screen.lineText(0), "hello");
    EXPECT_EQ(screen.lineText(1), "world");
    EXPECT_EQ(screen.cursor().row, 1);
    EXPECT_EQ(screen.cursor().column, 5);
    EXPECT_TRUE(screen.takeEvents().changed);
    EXPECT_FALSE(screen.takeEvents().changed);
}

TEST(VtScreen, CursorMovesAndErases) {
    VtScreen screen(PtySize{20, 6}, 100);
    screen.feed("\x1b[5;10HX");
    EXPECT_EQ(screen.cell(4, 9).text, "X");
    EXPECT_EQ(screen.cursor().row, 4);
    EXPECT_EQ(screen.cursor().column, 10);
    screen.feed("\x1b[1;1Habcdef\x1b[1;3H\x1b[K");
    EXPECT_EQ(screen.lineText(0), "ab");
    screen.feed("\x1b[2J");
    EXPECT_EQ(screen.screenText(), "\n\n\n\n\n");
}

TEST(VtScreen, SequencesMaySplitAcrossFeeds) {
    VtScreen screen(PtySize{20, 3}, 100);
    screen.feed("\x1b[3");
    screen.feed("1mX\xe4");
    screen.feed("\xb8\xad"); // U+4E2D split inside its UTF-8 encoding.
    EXPECT_EQ(screen.cell(0, 0).foreground, CellColor::indexed(1));
    EXPECT_EQ(screen.cell(0, 1).text, "\xe4\xb8\xad");
}

TEST(VtScreen, Colours) {
    VtScreen screen(PtySize{20, 3}, 100);
    screen.feed("\x1b[31mR\x1b[38;5;200mI\x1b[38;2;10;20;30mT\x1b[0mD\x1b[48;5;17mB\x1b[48;2;1;2;3mG\x1b[91mH");
    EXPECT_EQ(screen.cell(0, 0).foreground, CellColor::indexed(1));
    EXPECT_EQ(screen.cell(0, 1).foreground, CellColor::indexed(200));
    EXPECT_EQ(screen.cell(0, 2).foreground, CellColor::rgb(10, 20, 30));
    EXPECT_EQ(screen.cell(0, 3).foreground, CellColor{});
    EXPECT_EQ(screen.cell(0, 3).background, CellColor{});
    EXPECT_EQ(screen.cell(0, 4).background, CellColor::indexed(17));
    EXPECT_EQ(screen.cell(0, 5).background, CellColor::rgb(1, 2, 3));
    EXPECT_EQ(screen.cell(0, 6).foreground, CellColor::indexed(9));
}

TEST(VtScreen, Attributes) {
    VtScreen screen(PtySize{20, 3}, 100);
    screen.feed("\x1b[1;3;4;7;9mA\x1b[0mB\x1b[4:3mC");
    const auto a = screen.cell(0, 0).style;
    EXPECT_TRUE(a.bold);
    EXPECT_TRUE(a.italic);
    EXPECT_EQ(a.underline, Underline::Single);
    EXPECT_TRUE(a.inverse);
    EXPECT_TRUE(a.strike);
    EXPECT_EQ(screen.cell(0, 1).style, frost::term::CellStyle{});
    EXPECT_EQ(screen.cell(0, 2).style.underline, Underline::Curly);
}

TEST(VtScreen, WideCharacters) {
    VtScreen screen(PtySize{10, 3}, 100);
    screen.feed("\xe4\xb8\xad"
                "a"); // U+4E2D (CJK, two cells), then 'a'.
    EXPECT_EQ(screen.cell(0, 0).text, "\xe4\xb8\xad");
    EXPECT_EQ(screen.cell(0, 0).width, 2);
    EXPECT_EQ(screen.cell(0, 1).width, 0);
    EXPECT_TRUE(screen.cell(0, 1).text.empty());
    EXPECT_EQ(screen.cell(0, 2).text, "a");
    EXPECT_EQ(screen.cursor().column, 3);
    EXPECT_EQ(screen.lineText(0), "\xe4\xb8\xad"
                                  "a");
    // A combining mark joins the previous cell.
    screen.feed("\r\ne\xcc\x81");
    EXPECT_EQ(screen.cell(1, 0).text, "e\xcc\x81");
    EXPECT_EQ(screen.cursor().column, 1);
}

TEST(VtScreen, AlternateScreen) {
    VtScreen screen(PtySize{20, 4}, 100);
    screen.feed("main");
    EXPECT_FALSE(screen.altScreen());
    screen.feed("\x1b[?1049h");
    EXPECT_TRUE(screen.altScreen());
    EXPECT_EQ(screen.lineText(0), "");
    screen.feed("\x1b[Halt");
    EXPECT_EQ(screen.lineText(0), "alt");
    screen.feed("\x1b[?1049l");
    EXPECT_FALSE(screen.altScreen());
    EXPECT_EQ(screen.lineText(0), "main");
    EXPECT_EQ(screen.scrollbackSize(), 0U);
}

TEST(VtScreen, ScrollbackKeepsTheNewestLinesUpToItsCap) {
    VtScreen screen(PtySize{10, 3}, 5);
    static_cast<void>(screen.takeEvents());
    for (int i = 0; i < 10; ++i) {
        screen.feed("l" + std::to_string(i) + (i < 9 ? "\r\n" : ""));
    }
    // l7..l9 are on the screen; l0..l6 scrolled off and the cap keeps l2..l6.
    EXPECT_EQ(screen.lineText(0), "l7");
    EXPECT_EQ(screen.lineText(2), "l9");
    EXPECT_EQ(screen.scrollbackSize(), 5U);
    EXPECT_EQ(screen.lineText(-1), "l6");
    EXPECT_EQ(screen.lineText(-5), "l2");
    EXPECT_EQ(screen.lineText(-6), "");
    EXPECT_EQ(screen.takeEvents().linesScrolledOff, 7U);
    EXPECT_EQ(screen.linesPushed(), 7U);
    EXPECT_EQ(screen.text(ScreenPos{-2, 0}, ScreenPos{0, 9}), "l5\nl6\nl7");
}

TEST(VtScreen, ScrollbackOfZeroKeepsNothing) {
    VtScreen screen(PtySize{10, 2}, 0);
    screen.feed("a\r\nb\r\nc\r\nd");
    EXPECT_EQ(screen.scrollbackSize(), 0U);
    EXPECT_EQ(screen.screenText(), "c\nd");
}

TEST(VtScreen, ScrollbackKeepsColours) {
    VtScreen screen(PtySize{10, 2}, 10);
    screen.feed("\x1b[32mgreen\x1b[0m\r\nx\r\ny");
    EXPECT_EQ(screen.lineText(-1), "green");
    EXPECT_EQ(screen.cell(-1, 0).foreground, CellColor::indexed(2));
}

TEST(VtScreen, TitleAndBell) {
    VtScreen screen(PtySize{20, 3}, 100);
    static_cast<void>(screen.takeEvents());
    screen.feed("\x1b]0;my title\x07");
    auto events = screen.takeEvents();
    EXPECT_TRUE(events.titleChanged);
    EXPECT_EQ(events.bells, 0); // The BEL ended the OSC.
    EXPECT_EQ(screen.title(), "my title");
    screen.feed("\x1b]2;second\x1b\\\a\a");
    events = screen.takeEvents();
    EXPECT_EQ(screen.title(), "second");
    EXPECT_EQ(events.bells, 2);
}

TEST(VtScreen, CursorShapeAndVisibility) {
    VtScreen screen(PtySize{20, 3}, 100);
    EXPECT_EQ(screen.cursor().shape, CursorShape::Block);
    screen.feed("\x1b[6 q");
    EXPECT_EQ(screen.cursor().shape, CursorShape::Bar);
    EXPECT_FALSE(screen.cursor().blinking);
    screen.feed("\x1b[3 q");
    EXPECT_EQ(screen.cursor().shape, CursorShape::Underline);
    EXPECT_TRUE(screen.cursor().blinking);
    screen.feed("\x1b[?25l");
    EXPECT_FALSE(screen.cursor().visible);
    screen.feed("\x1b[?25h");
    EXPECT_TRUE(screen.cursor().visible);
}

TEST(VtScreen, AnswersQueriesThroughItsOutput) {
    VtScreen screen(PtySize{20, 5}, 100);
    screen.feed("\x1b[3;5H\x1b[6n");
    EXPECT_EQ(screen.takeOutput(), "\x1b[3;5R");
    EXPECT_EQ(screen.takeOutput(), "");
}

TEST(VtScreen, ResizeChangesTheGrid) {
    VtScreen screen(PtySize{20, 5}, 100);
    screen.feed("abc");
    screen.resize(PtySize{40, 10});
    EXPECT_EQ(screen.rows(), 10);
    EXPECT_EQ(screen.columns(), 40);
    EXPECT_EQ(screen.lineText(0), "abc");
    screen.feed("\x1b[10;40HZ");
    EXPECT_EQ(screen.cell(9, 39).text, "Z");
}

TEST(VtScreen, TextBetweenTwoPositions) {
    VtScreen screen(PtySize{10, 3}, 100);
    screen.feed("0123456789abc\r\nxyz");
    // The first row wrapped at 10 columns.
    EXPECT_EQ(screen.lineText(0), "0123456789");
    EXPECT_EQ(screen.lineText(1), "abc");
    EXPECT_EQ(screen.text(ScreenPos{0, 7}, ScreenPos{1, 1}), "789\nab");
    EXPECT_EQ(screen.text(ScreenPos{1, 1}, ScreenPos{0, 7}), "789\nab");
    EXPECT_EQ(screen.text(ScreenPos{2, 0}, ScreenPos{2, 9}), "xyz");
}

TEST(VtScreen, MouseReportingOnlyWhenTheProgramAsks) {
    VtScreen screen(PtySize{20, 5}, 100);
    screen.mouseButton(MouseButton::Left, true, 2, 3, {});
    EXPECT_EQ(screen.takeOutput(), "");
    screen.feed("\x1b[?1000h\x1b[?1006h");
    EXPECT_EQ(screen.mouseMode(), MouseMode::Click);
    screen.mouseButton(MouseButton::Left, true, 2, 3, {});
    screen.mouseButton(MouseButton::Left, false, 2, 3, {});
    EXPECT_EQ(screen.takeOutput(), "\x1b[<0;4;3M\x1b[<0;4;3m");
    screen.feed("\x1b[?1002h");
    EXPECT_EQ(screen.mouseMode(), MouseMode::Drag);
    screen.feed("\x1b[?1003h");
    EXPECT_EQ(screen.mouseMode(), MouseMode::Move);
    screen.feed("\x1b[?1003l");
    EXPECT_EQ(screen.mouseMode(), MouseMode::None);
}

TEST(VtScreen, FocusReportsWhenEnabled) {
    VtScreen screen(PtySize{20, 5}, 100);
    screen.focusChanged(true);
    EXPECT_EQ(screen.takeOutput(), "");
    screen.feed("\x1b[?1004h");
    screen.focusChanged(true);
    screen.focusChanged(false);
    EXPECT_EQ(screen.takeOutput(), "\x1b[I\x1b[O");
}

} // namespace
