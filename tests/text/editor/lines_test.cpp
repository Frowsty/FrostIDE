// SPDX-License-Identifier: Apache-2.0
//
// Line operations of the editor model (ADR 0025): duplicate line or selection, delete lines, move
// lines up and down (with the last line's missing break and selections that end at a line start),
// join lines, line comments (C++, CMake, the block-token fallback for XML, plain text) and block
// comments, each as one undo step.

#include "frost/text/editor/commands.hpp"
#include "frost/text/editor/language.hpp"

#include "text/editor/editor_fixture.hpp"
#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using frost::text::languageById;
using frost::text::testing::applied;
using frost::text::testing::EditorFixture;
namespace commands = frost::text::commands;

// "«" and "»" (editor_fixture.hpp's selection markers).
#define M_A "\xC2\xAB"
#define M_H "\xC2\xBB"

std::string inLanguage(std::string_view id, std::string_view marked, auto command) {
    EditorFixture fixture(marked, languageById(id));
    fixture.run(command);
    return fixture.show();
}

TEST(LineOps, DuplicateLineOrSelection) {
    EXPECT_EQ(applied("ab|c\nd", commands::duplicate), "abc\nab|c\nd");
    EXPECT_EQ(applied("x\nab|", commands::duplicate), "x\nab\nab|");
    EXPECT_EQ(applied("a" M_A "bc" M_H "d", commands::duplicate), "abc" M_A "bc" M_H "d");
    EXPECT_EQ(applied("a" M_H "bc" M_A "d", commands::duplicate), "abc" M_H "bc" M_A "d");
    EXPECT_EQ(applied("a|b|c", commands::duplicate), "abc\na|b|c");
    EXPECT_EQ(applied("a\r\n|b\r\nc", commands::duplicate), "a\r\nb\r\n|b\r\nc");
}

TEST(LineOps, DeleteLinesKeepsTheColumn) {
    EXPECT_EQ(applied("a\nb|b\nc", commands::deleteLines), "a\nc|");
    EXPECT_EQ(applied("a\nb|", commands::deleteLines), "a|");
    EXPECT_EQ(applied("ab|c", commands::deleteLines), "|");
    EXPECT_EQ(applied("a\n" M_A "b\nc" M_H "\nd", commands::deleteLines), "a\nd|");
    // Carets that land on one line merge into one, keeping the primary's column (ADR 0037).
    EXPECT_EQ(applied("|a\nb|\nc", commands::deleteLines), "|c");
    EXPECT_EQ(applied("a|\n|b\ncd", commands::deleteLines), "c|d");
    EXPECT_EQ(applied("a|\nb\nc|d\ne", commands::deleteLines), "b|\ne|"); // Separate lines stay apart.
}

TEST(LineOps, DeleteLinesMergesCaretsOnTheLastLine) {
    EXPECT_EQ(applied("x\na|\nb|", commands::deleteLines), "x|");
    EXPECT_EQ(applied("a|b\nc|d", commands::deleteLines), "|");
}

TEST(LineOps, PasteLinesGoesAboveTheCaretLine) {
    EXPECT_EQ(applied("ab|c\nd", commands::pasteLines, "x\n"), "x\nab|c\nd");
    EXPECT_EQ(applied("a\n|b", commands::pasteLines, "x\n"), "a\nx\n|b");
    EXPECT_EQ(applied("a|b|\nc|", commands::pasteLines, "x\n"), "x\na|b|\nx\nc|"); // Once per line.
    EXPECT_EQ(applied("a" M_A "b" M_H "c", commands::pasteLines, "x\n"), "x\na" M_A "b" M_H "c");
    EXPECT_EQ(applied("a|", commands::pasteLines, ""), "a|");
}

TEST(LineOps, MoveLinesUpAndDown) {
    EXPECT_EQ(applied("a\nb|\nc", commands::moveLinesUp), "b|\na\nc");
    EXPECT_EQ(applied("a|\nb\nc", commands::moveLinesDown), "b\na|\nc");
    EXPECT_EQ(applied("a\nb|", commands::moveLinesUp), "b|\na");
    EXPECT_EQ(applied("a|\nb", commands::moveLinesDown), "b\na|");
    EXPECT_EQ(applied("a\nb|", commands::moveLinesDown), "a\nb|"); // Already the last line.
    EXPECT_EQ(applied("a|\nb", commands::moveLinesUp), "a|\nb");   // Already the first line.
    EXPECT_EQ(applied("x\n" M_A "a\nb\n" M_H "y", commands::moveLinesUp), M_A "a\nb\n" M_H "x\ny");
    EXPECT_EQ(applied(M_A "a\nb\n" M_H "x\ny", commands::moveLinesDown), "x\n" M_A "a\nb\n" M_H "y");
}

TEST(LineOps, MoveLinesIsOneUndoStep) {
    EditorFixture fixture("a\nb|\nc");
    fixture.run(commands::moveLinesUp);
    fixture.run(commands::moveLinesUp); // At the top already: nothing to do.
    EXPECT_EQ(fixture.show(), "b|\na\nc");
    fixture.undo();
    EXPECT_EQ(fixture.show(), "a\nb|\nc");
}

TEST(LineOps, JoinLines) {
    EXPECT_EQ(applied("a|\n  b", commands::joinLines), "a| b");
    EXPECT_EQ(applied("foo(|\n  x)", commands::joinLines), "foo(|x)");
    EXPECT_EQ(applied("x  |\n)", commands::joinLines), "x|)");
    EXPECT_EQ(applied(M_A "a\nb\nc" M_H, commands::joinLines), M_A "a b c" M_H);
    EXPECT_EQ(applied("a\nb|", commands::joinLines), "a\nb|"); // No next line.
    EXPECT_EQ(applied("a|\n\nb", commands::joinLines), "a|\nb");
}

TEST(LineOps, LineComments) {
    EXPECT_EQ(applied("|int x;", commands::toggleLineComment), "// |int x;");
    EXPECT_EQ(applied("  a|\n  b", commands::toggleLineComment), "  // a\n  b|"); // The caret moves down.
    EXPECT_EQ(applied(M_A "  // a\n  //b\n" M_H "c", commands::toggleLineComment), M_A "  a\n  b\n" M_H "c");
    EXPECT_EQ(applied(M_A "    a\n  b" M_H, commands::toggleLineComment), M_A "  //   a\n  // b" M_H);
    EXPECT_EQ(applied(M_A "a\n\nb" M_H, commands::toggleLineComment), M_A "// a\n\n// b" M_H);
    EXPECT_EQ(inLanguage("cmake", "|set(x)", commands::toggleLineComment), "# |set(x)");
    EXPECT_EQ(inLanguage("xml", "|<a/>", commands::toggleLineComment), "<!-- |<a/> -->");
    EXPECT_EQ(inLanguage("xml", "|<!-- <a/> -->", commands::toggleLineComment), "|<a/>");
    EXPECT_EQ(inLanguage("text", "|plain", commands::toggleLineComment), "|plain");
}

TEST(LineOps, CommentRoundTripIsTwoUndoSteps) {
    EditorFixture fixture(M_A "a\nb" M_H);
    fixture.run(commands::toggleLineComment);
    EXPECT_EQ(fixture.show(), M_A "// a\n// b" M_H);
    fixture.run(commands::toggleLineComment);
    EXPECT_EQ(fixture.show(), M_A "a\nb" M_H);
    fixture.undo();
    EXPECT_EQ(fixture.show(), M_A "// a\n// b" M_H);
}

TEST(LineOps, BlockComments) {
    EXPECT_EQ(applied("a " M_A "b" M_H " c", commands::toggleBlockComment), "a /*" M_A "b" M_H "*/ c");
    EXPECT_EQ(applied("a /*" M_A "b" M_H "*/ c", commands::toggleBlockComment), "a " M_A "b" M_H " c");
    EXPECT_EQ(applied(M_A "/*b*/" M_H, commands::toggleBlockComment), M_A "b" M_H);
    EXPECT_EQ(applied("x|", commands::toggleBlockComment), "x/*|*/");
    EXPECT_EQ(applied("/* a| */", commands::toggleBlockComment), " a| ");
    EXPECT_EQ(inLanguage("python", "|x = 1", commands::toggleBlockComment), "# |x = 1"); // No block tokens.
}

#undef M_A
#undef M_H

} // namespace
