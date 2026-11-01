// SPDX-License-Identifier: Apache-2.0
//
// Typing and smart keys of the editor model (ADR 0025): typing at every caret, undo grouping by word
// and by pause, auto-close and overtype of brackets and quotes, surround, Enter with auto-indent and
// bracket expansion, Tab and Shift+Tab, Backspace over indentation and empty pairs, deletion by
// grapheme cluster and by word, smart Home and End.

#include "frost/text/editor/commands.hpp"

#include "text/editor/editor_fixture.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace {

using frost::text::EditorConfig;
using frost::text::LineBreak;
using frost::text::Movement;
using frost::text::testing::applied;
using frost::text::testing::EditorFixture;
namespace commands = frost::text::commands;

constexpr std::string_view kFamily = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
constexpr std::string_view kFlagDe = "\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA";

std::string typed(std::string_view marked, std::string_view text) {
    EditorFixture fixture(marked);
    fixture.typeChars(text);
    return fixture.show();
}

TEST(Typing, InsertsAtEveryCaretAndReplacesSelections) {
    EXPECT_EQ(typed("a|b|c", "x"), "ax|bx|c");
    EXPECT_EQ(typed("\xC2\xAB"
                    "foo\xC2\xBB bar",
                    "x"),
              "x| bar");
    EXPECT_EQ(applied("a|b", commands::paste, "xy\nz"), "axy\nz|b");
}

TEST(Typing, AWordIsOneUndoStep) {
    EditorFixture fixture("|");
    fixture.typeChars("hello world");
    EXPECT_EQ(fixture.show(), "hello world|");
    fixture.undo();
    EXPECT_EQ(fixture.show(), "hello |");
    fixture.undo();
    EXPECT_EQ(fixture.show(), "|");
    EXPECT_FALSE(fixture.document().undoState().canUndo);
    fixture.redo();
    EXPECT_EQ(fixture.show(), "hello |");
    fixture.redo();
    EXPECT_EQ(fixture.show(), "hello world|");
}

TEST(Typing, APauseStartsANewStep) {
    EditorFixture fixture("|");
    fixture.typeChars("ab");
    fixture.clock().advance(std::chrono::seconds(2));
    fixture.typeChars("cd");
    fixture.undo();
    EXPECT_EQ(fixture.show(), "ab|");
}

TEST(SmartKeys, AutoCloseAndOvertypeBrackets) {
    EditorFixture fixture("|");
    fixture.typeChars("(");
    EXPECT_EQ(fixture.show(), "(|)");
    fixture.typeChars("x");
    EXPECT_EQ(fixture.show(), "(x|)");
    fixture.typeChars(")");
    EXPECT_EQ(fixture.show(), "(x)|");
    EXPECT_EQ(typed("|foo", "("), "(|foo");             // Not before a word.
    EXPECT_EQ(typed("foo(bar(|)", ")"), "foo(bar()|)"); // Unbalanced: the closer is typed.
    EXPECT_EQ(typed("f(|, y)", "["), "f([|], y)");      // Before a comma.
}

TEST(SmartKeys, AutoCloseAndOvertypeQuotes) {
    EditorFixture fixture("|");
    fixture.typeChars("\"");
    EXPECT_EQ(fixture.show(), "\"|\"");
    fixture.typeChars("\"");
    EXPECT_EQ(fixture.show(), "\"\"|");
    EXPECT_EQ(typed("don|", "'"), "don'|");       // After a word character.
    EXPECT_EQ(typed("\"abc|", "\""), "\"abc\"|"); // Closing an open string.
}

TEST(SmartKeys, SurroundSelections) {
    EXPECT_EQ(typed("\xC2\xAB"
                    "foo\xC2\xBB",
                    "("),
              "(\xC2\xAB"
              "foo\xC2\xBB)");
    EXPECT_EQ(typed("\xC2\xBB"
                    "foo\xC2\xAB",
                    "["),
              "[\xC2\xBB"
              "foo\xC2\xAB]");
    EXPECT_EQ(typed("\xC2\xAB"
                    "a\xC2\xBB \xC2\xAB"
                    "b\xC2\xBB",
                    "\""),
              "\"\xC2\xAB"
              "a\xC2\xBB\" \"\xC2\xAB"
              "b\xC2\xBB\"");
}

TEST(SmartKeys, EnterKeepsTheIndentation) {
    EXPECT_EQ(applied("    foo|", commands::newline), "    foo\n    |");
    EXPECT_EQ(applied("  |  foo", commands::newline), "  \n  |  foo");
    EXPECT_EQ(applied("if (x) {|", commands::newline), "if (x) {\n    |");
    EXPECT_EQ(applied("    f() {|}", commands::newline), "    f() {\n        |\n    }");
    EXPECT_EQ(applied("g(|)", commands::newline), "g(\n    |\n)");
    EXPECT_EQ(applied("a|\r\nb", commands::newline), "a\r\n|\r\nb");
    EXPECT_EQ(applied("a\r\nb|", commands::newline), "a\r\nb\r\n|"); // The text's line break.
    EXPECT_EQ(applied("x|y|", commands::newline), "x\n|y\n|");
}

TEST(SmartKeys, EnterIsItsOwnUndoStep) {
    EditorFixture fixture("|");
    fixture.typeChars("ab");
    fixture.run(commands::newline);
    fixture.typeChars("cd");
    fixture.undo();
    EXPECT_EQ(fixture.show(), "ab\n|");
    fixture.undo();
    EXPECT_EQ(fixture.show(), "ab|");
}

TEST(SmartKeys, TabAndShiftTab) {
    EXPECT_EQ(applied("ab|", commands::tab), "ab  |");
    EXPECT_EQ(applied("|x", commands::tab), "    |x");
    EXPECT_EQ(applied("\xC2\xAB"
                      "a\nb\n\xC2\xBB"
                      "c",
                      commands::tab),
              "\xC2\xAB    a\n    b\n\xC2\xBB"
              "c");
    EXPECT_EQ(applied("\xC2\xAB"
                      "a\n\nb\xC2\xBB",
                      commands::tab),
              "\xC2\xAB    a\n\n    b\xC2\xBB"); // Empty lines stay.
    EXPECT_EQ(applied("    |a\n  b", commands::outdent), "|a\n  b");
    EXPECT_EQ(applied("\xC2\xAB    a\n  b\xC2\xBB", commands::outdent), "\xC2\xAB"
                                                                        "a\nb\xC2\xBB");
    EXPECT_EQ(applied("\t\t|x", commands::outdent), "\t|x");

    EditorConfig tabs;
    tabs.settings.insertSpaces = false;
    EditorFixture fixture("a|", frost::text::languageById("cpp"), tabs);
    fixture.run(commands::tab);
    EXPECT_EQ(fixture.show(), "a\t|");
}

TEST(SmartKeys, BackspaceOverIndentationAndPairs) {
    EXPECT_EQ(applied("        |x", commands::backspace), "    |x");
    EXPECT_EQ(applied("      |x", commands::backspace), "    |x");
    EXPECT_EQ(applied("  |x", commands::backspace), "|x");
    EXPECT_EQ(applied("  a  |", commands::backspace), "  a |");
    EXPECT_EQ(applied("(|)", commands::backspace), "|");
    EXPECT_EQ(applied("\"|\"", commands::backspace), "|");
    EXPECT_EQ(applied("(|x)", commands::backspace), "|x)");
    EXPECT_EQ(applied("|abc", commands::backspace), "|abc");
    EXPECT_EQ(applied("a|b|c", commands::backspace), "|c"); // Touching deletions merge.
}

TEST(Deletion, ByGraphemeClusterAndByWord) {
    EXPECT_EQ(applied(std::string("a") + std::string(kFamily) + "|b", commands::backspace), "a|b");
    EXPECT_EQ(applied("e\xCC\x81|", commands::backspace), "|");
    EXPECT_EQ(applied("x\r\n|y", commands::backspace), "x|y");
    EXPECT_EQ(applied(std::string("|") + std::string(kFlagDe) + "x", commands::deleteForward), "|x");
    EXPECT_EQ(applied("\xC2\xAB"
                      "ab\xC2\xBB"
                      "c",
                      commands::deleteForward),
              "|c");
    EXPECT_EQ(applied("foo bar|", commands::deleteWordBackward), "foo |");
    EXPECT_EQ(applied("|foo bar", commands::deleteWordForward), "| bar");
}

TEST(Deletion, BackspaceRunsAreOneUndoStep) {
    EditorFixture fixture("abc|");
    fixture.run(commands::backspace);
    fixture.run(commands::backspace);
    fixture.run(commands::backspace);
    EXPECT_EQ(fixture.show(), "|");
    fixture.undo();
    EXPECT_EQ(fixture.show(), "abc|");
}

TEST(SmartKeys, HomeTogglesAndEndGoesToTheLineEnd) {
    EditorFixture fixture("    foo|");
    fixture.run(commands::move, Movement::LineStart, false);
    EXPECT_EQ(fixture.show(), "    |foo");
    fixture.run(commands::move, Movement::LineStart, false);
    EXPECT_EQ(fixture.show(), "|    foo");
    fixture.run(commands::move, Movement::LineStart, false);
    EXPECT_EQ(fixture.show(), "    |foo");
    fixture.run(commands::move, Movement::LineEnd, false);
    EXPECT_EQ(fixture.show(), "    foo|");
    fixture.run(commands::move, Movement::LineStart, true);
    EXPECT_EQ(fixture.show(), "    \xC2\xBB"
                              "foo\xC2\xAB");
    EXPECT_EQ(applied("a\n  b|c", commands::move, Movement::LineStart, false), "a\n  |bc");
}

} // namespace
