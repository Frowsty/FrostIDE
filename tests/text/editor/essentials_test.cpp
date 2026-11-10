// SPDX-License-Identifier: Apache-2.0
//
// The UX-3 editor essentials of the editor model (frost/text/editor/essentials.hpp): Split Line,
// Start New Line Before, overwrite typing, line-block swaps, Move Statement over the real
// tree-sitter C++ and Lua grammars (frost::syntax is the oracle for what a statement is), Jump to
// Matching Brace and Code Block Start/End with and without a tree, and Auto-Indent Lines.

#include "frost/sched/sim_executor.hpp"
#include "frost/syntax/language.hpp"
#include "frost/syntax/syntax_session.hpp"
#include "frost/syntax/syntax_tree.hpp"
#include "frost/text/editor/essentials.hpp"
#include "frost/text/editor/language.hpp"
#include "frost/text/text_buffer.hpp"

#include "text/editor/editor_fixture.hpp"
#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using frost::ByteOffset;
using frost::text::ByteRange;
using frost::text::languageById;
using frost::text::testing::applied;
using frost::text::testing::EditorFixture;
using frost::text::testing::parse;
namespace commands = frost::text::commands;
namespace syntax = frost::syntax;

#define M_A "\xC2\xAB"
#define M_H "\xC2\xBB"

// A synchronous tree-sitter parse of `text` (tests/syntax's parseNow, without its private headers).
syntax::SyntaxTree parseNow(syntax::LanguageId language, std::string_view text) {
    static const syntax::LanguageRegistry registry;
    if (!registry.prepare(language)) {
        return {};
    }
    frost::Result<frost::text::TextBuffer> buffer = frost::text::TextBuffer::fromUtf8(std::string(text));
    if (!buffer) {
        return {};
    }
    frost::sched::SimExecutor sim;
    syntax::SyntaxSession session(registry, sim, sim);
    session.open(language, buffer->current());
    sim.runUntilIdle();
    (void)session.poll();
    return session.state() == syntax::SyntaxState::Current ? session.tree() : syntax::SyntaxTree{};
}

// The seam over a real tree.
class TreeRanges final : public frost::text::ISyntaxRanges {
public:
    explicit TreeRanges(syntax::SyntaxTree tree) : m_tree(std::move(tree)) {}
    std::vector<ByteRange> enclosing(ByteRange range) const override { return m_tree.enclosingRanges(range); }
    [[nodiscard]] bool valid() const noexcept { return !m_tree.empty(); }

private:
    syntax::SyntaxTree m_tree;
};

// Runs Move Statement on a marked text with the real grammar of `language`.
std::string movedStatement(std::string_view marked, bool up, syntax::LanguageId language = syntax::LanguageId::Cpp,
                           std::string_view languageId = "cpp") {
    EditorFixture fixture(marked, languageById(languageId));
    const TreeRanges tree(parseNow(language, parse(marked).text));
    EXPECT_TRUE(tree.valid());
    fixture.run(commands::moveStatement, up, &tree);
    return fixture.show();
}

std::string inLanguage(std::string_view id, std::string_view marked, auto command) {
    EditorFixture fixture(marked, languageById(id));
    fixture.run(command);
    return fixture.show();
}

TEST(EditorEssentials, SplitLineKeepsTheCaretBeforeTheBreak) {
    EXPECT_EQ(applied("    ab|cd", commands::splitLine), "    ab|\n    cd");
    EXPECT_EQ(applied("f(|)", commands::splitLine), "f(|\n    \n)");
    EXPECT_EQ(applied("a|b|c", commands::splitLine), "a|\nb|\nc");
}

TEST(EditorEssentials, StartNewLineBeforeIndentsLikeTheLine) {
    EXPECT_EQ(applied("x\n    ab|c", commands::startNewLineBefore), "x\n    |\n    abc");
    EXPECT_EQ(applied("{\n}|", commands::startNewLineBefore), "{\n    |\n}");
    // Two carets on one line make one new line.
    EXPECT_EQ(applied("a|b|", commands::startNewLineBefore), "|\nab");
    EXPECT_EQ(inLanguage("lua", "if x then\nend|", commands::startNewLineBefore), "if x then\n    |\nend");
    EXPECT_EQ(applied("a\r\nb|", commands::startNewLineBefore), "a\r\n|\r\nb");
}

TEST(EditorEssentials, OvertypeReplacesTheNextCharacterButNotTheLineBreak) {
    EXPECT_EQ(applied("a|bc", commands::overtype, "x"), "ax|c");
    EXPECT_EQ(applied("ab|\ncd", commands::overtype, "x"), "abx|\ncd");
    EXPECT_EQ(applied("a" M_A "bc" M_H "d", commands::overtype, "x"), "ax|d");
    // No auto-closed pair in overwrite mode.
    EXPECT_EQ(applied("|ab", commands::overtype, "("), "(|b");
    EditorFixture fixture("|abcd");
    fixture.run(commands::overtype, "x");
    fixture.run(commands::overtype, "y");
    EXPECT_EQ(fixture.show(), "xy|cd");
    fixture.undo();
    EXPECT_EQ(fixture.show(), "|abcd"); // One typing run is one undo step.
}

TEST(EditorEssentials, SwapLineBlocksKeepsBreaksAndSelections) {
    using frost::text::LineBlock;
    EXPECT_EQ(applied("a\nb\nc|\nd", commands::swapLineBlocks, LineBlock{2, 3}, LineBlock{0, 1}), "c|\nd\na\nb");
    EXPECT_EQ(applied("a|\nb\nc", commands::swapLineBlocks, LineBlock{0, 0}, LineBlock{1, 2}), "b\nc\na|");
    EXPECT_EQ(applied("x\n" M_A "a\n" M_H "b", commands::swapLineBlocks, LineBlock{1, 1}, LineBlock{2, 2}),
              "x\nb\n" M_A "a" M_H);
}

TEST(EditorEssentials, MoveStatementSwapsWholeStatements) {
    const std::string_view twoIfs = "void f() {\n"
                                    "    if (a) {\n"
                                    "        x();\n"
                                    "    }\n"
                                    "    y|();\n"
                                    "}\n";
    EXPECT_EQ(movedStatement(twoIfs, true), "void f() {\n"
                                            "    y|();\n"
                                            "    if (a) {\n"
                                            "        x();\n"
                                            "    }\n"
                                            "}\n");
    // A caret on the closing brace moves the whole if over the statement below.
    const std::string_view onBrace = "void f() {\n"
                                     "    if (a) {\n"
                                     "        x();\n"
                                     "    }|\n"
                                     "    y();\n"
                                     "}\n";
    EXPECT_EQ(movedStatement(onBrace, false), "void f() {\n"
                                              "    y();\n"
                                              "    if (a) {\n"
                                              "        x();\n"
                                              "    }|\n"
                                              "}\n");
    // A multi-line statement moves as one.
    EXPECT_EQ(movedStatement("void f() {\n    a();\n    g(1|,\n      2);\n}\n", true),
              "void f() {\n    g(1|,\n      2);\n    a();\n}\n");
}

TEST(EditorEssentials, MoveStatementStopsAtItsBlock) {
    const std::string_view first = "void f() {\n    a|();\n    b();\n}\n";
    EXPECT_EQ(movedStatement(first, true), first);
    const std::string_view last = "void f() {\n    a();\n    b|();\n}\n";
    EXPECT_EQ(movedStatement(last, false), last);
    // A blank line is a neighbour of its own.
    EXPECT_EQ(movedStatement("void f() {\n    a();\n\n    b|();\n}\n", true), "void f() {\n    a();\n    b|();\n\n}\n");
}

TEST(EditorEssentials, MoveStatementInLuaAndWithoutATree) {
    EXPECT_EQ(
        movedStatement("local a = 1\nif x then\n  y()\nend\nlocal b| = 2\n", true, syntax::LanguageId::Lua, "lua"),
        "local a = 1\nlocal b| = 2\nif x then\n  y()\nend\n");
    // Plain text has no statements: lines move.
    EditorFixture fixture("a\nb|\nc", languageById("text"));
    fixture.run(commands::moveStatement, true, nullptr);
    EXPECT_EQ(fixture.show(), "b|\na\nc");
}

TEST(EditorEssentials, MatchingBracketWithAndWithoutTheTree) {
    const std::string text = "void k() { f(a, \")\", g[1]); }";
    EditorFixture fixture(text);
    const frost::text::Snapshot snapshot = fixture.editor().text();
    const TreeRanges tree(parseNow(syntax::LanguageId::Cpp, text));
    ASSERT_TRUE(tree.valid());
    const ByteOffset open{text.find("f(") + 1};
    // The tree knows the ")" in the string is no bracket; the plain scan does not.
    const auto withTree = frost::text::matchingBracket(snapshot, open, &tree);
    ASSERT_TRUE(withTree.has_value());
    EXPECT_EQ(withTree->open, open);
    EXPECT_EQ(withTree->close.value(), text.find(");"));
    const auto plain = frost::text::matchingBracket(snapshot, open, nullptr);
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(plain->close.value(), text.find(")\""));
    // The bracket before the caret counts when none follows it.
    const auto closer = frost::text::matchingBracket(snapshot, ByteOffset{text.size()}, &tree);
    ASSERT_TRUE(closer.has_value());
    EXPECT_EQ(closer->open.value(), text.find('{'));
    EXPECT_FALSE(frost::text::matchingBracket(snapshot, ByteOffset{2}, &tree).has_value());
}

TEST(EditorEssentials, EnclosingBraceBlockWalksOutwards) {
    const std::string text = "void f() { if (a) { x(); } }";
    EditorFixture fixture(text);
    const frost::text::Snapshot snapshot = fixture.editor().text();
    const TreeRanges tree(parseNow(syntax::LanguageId::Cpp, text));
    const std::size_t x = text.find('x');
    for (const frost::text::ISyntaxRanges* syntax : {static_cast<const frost::text::ISyntaxRanges*>(&tree),
                                                     static_cast<const frost::text::ISyntaxRanges*>(nullptr)}) {
        const auto inner = frost::text::enclosingBraceBlock(snapshot, ByteOffset{x}, syntax);
        ASSERT_TRUE(inner.has_value());
        EXPECT_EQ(inner->open.value(), text.find('{', text.find("if")));
        EXPECT_EQ(inner->close.value(), text.find('}'));
        // From right before the inner '{' the outer block is next.
        const auto outer = frost::text::enclosingBraceBlock(snapshot, inner->open, syntax);
        ASSERT_TRUE(outer.has_value());
        EXPECT_EQ(outer->open.value(), text.find('{'));
        EXPECT_EQ(outer->close.value(), text.rfind('}'));
        EXPECT_FALSE(frost::text::enclosingBraceBlock(snapshot, ByteOffset{0}, syntax).has_value());
    }
}

TEST(EditorEssentials, AutoIndentLinesFollowsTheBrackets) {
    EXPECT_EQ(inLanguage("cpp", M_A "void f() {\nif (a) {\nx();\n}\ny(1,\n2);\nz();\n}" M_H, commands::autoIndentLines),
              M_A "void f() {\n    if (a) {\n        x();\n    }\n    y(1,\n        2);\n    z();\n}" M_H);
    // Strings, comments and preprocessor lines do not count.
    EXPECT_EQ(inLanguage("cpp", M_A "int a = '{'; // {\n#if X\n  b();" M_H, commands::autoIndentLines),
              M_A "int a = '{'; // {\n#if X\nb();" M_H);
    // A caret re-indents its own line from the one above.
    EXPECT_EQ(inLanguage("cpp", "if (a) {\nx|();\n}", commands::autoIndentLines), "if (a) {\n    x|();\n}");
    // Block comment continuation lines keep their place.
    EXPECT_EQ(inLanguage("cpp", M_A "/*\n * a\n */\nb();" M_H, commands::autoIndentLines),
              M_A "/*\n * a\n */\nb();" M_H);
}

TEST(EditorEssentials, AutoIndentLinesInLuaAndUnsupportedLanguages) {
    EXPECT_EQ(inLanguage("lua", M_A "if x then\ny()\nelse\nz()\nend\nw()" M_H, commands::autoIndentLines),
              M_A "if x then\n    y()\nelse\n    z()\nend\nw()" M_H);
    EXPECT_EQ(inLanguage("lua", M_A "f(function()\ng()\nend)\nh()" M_H, commands::autoIndentLines),
              M_A "f(function()\n    g()\nend)\nh()" M_H);
    // Indentation is syntax in Python and YAML; CMake blocks are not brackets: nothing changes.
    EXPECT_EQ(inLanguage("python", M_A "if x:\ny()" M_H, commands::autoIndentLines), M_A "if x:\ny()" M_H);
    EXPECT_EQ(inLanguage("cmake", M_A "if(X)\n  set(A 1)\nendif()" M_H, commands::autoIndentLines),
              M_A "if(X)\n  set(A 1)\nendif()" M_H);
    EXPECT_FALSE(commands::autoIndentSupports(languageById("text")));
    EXPECT_TRUE(commands::autoIndentSupports(languageById("lua")));
}

TEST(EditorEssentials, AutoIndentIsOneUndoStep) {
    EditorFixture fixture(M_A "{\na;\nb;\n}" M_H);
    fixture.run(commands::autoIndentLines);
    EXPECT_EQ(fixture.text(), "{\n    a;\n    b;\n}");
    fixture.undo();
    EXPECT_EQ(fixture.text(), "{\na;\nb;\n}");
}

} // namespace
