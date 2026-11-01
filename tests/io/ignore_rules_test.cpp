// SPDX-License-Identifier: Apache-2.0
//
/*
 * Unit tests for the gitignore matcher (frost/io/ignore_rules.hpp): line handling (comments, escapes,
 * trailing spaces, CRLF, the byte-order mark), negation, directory-only and anchored patterns, files in
 * subdirectories, wildcards and bracket expressions, the globstar forms, patterns that never match, case
 * folding, the stack's precedence and the parent-directory rule, and a pathological pattern that must
 * stay linear. The vectors are written for this suite from the rules gitignore(5) describes; git itself
 * is the oracle in ignore_rules_git_test.cpp.
 */

#include "frost/io/ignore_rules.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>

namespace {

using frost::io::IgnoreOptions;
using frost::io::IgnoreRules;
using frost::io::IgnoreStack;

// Whether `rules` (a root file) ignore `path`.
bool ignores(const IgnoreRules& rules, std::string_view path, bool isDirectory = false) {
    const auto* rule = rules.lastMatch(path, isDirectory);
    return rule != nullptr && !rule->negated;
}

bool ignores(std::string_view text, std::string_view path, bool isDirectory = false) {
    return ignores(IgnoreRules::parse(text), path, isDirectory);
}

TEST(IgnoreRules, CommentsBlankLinesAndEscapedHashes) {
    const auto rules = IgnoreRules::parse("# a comment\n\n   \n\\#literal\n");
    ASSERT_EQ(rules.rules().size(), 1U);
    EXPECT_EQ(rules.rules()[0].line, 4U);
    EXPECT_TRUE(ignores(rules, "#literal"));
    EXPECT_FALSE(ignores(rules, "# a comment"));
    // Leading spaces are part of the pattern.
    EXPECT_TRUE(ignores("  #x", "  #x"));
}

TEST(IgnoreRules, TrailingSpacesUnlessEscaped) {
    EXPECT_TRUE(ignores("build   ", "build"));
    EXPECT_FALSE(ignores("build   ", "build "));
    EXPECT_TRUE(ignores("name\\ ", "name "));
    EXPECT_FALSE(ignores("name\\ ", "name"));
    // Only the unescaped spaces after the last escaped (or other) byte go.
    EXPECT_TRUE(ignores("two\\  \\  ", "two   "));
    EXPECT_EQ(IgnoreRules::parse("two\\  \\  ").rules()[0].text, "two\\  \\ ");
}

TEST(IgnoreRules, CrlfAndByteOrderMark) {
    const auto rules = IgnoreRules::parse("\xEF\xBB\xBF*.o\r\n!keep.o\r\nlast");
    ASSERT_EQ(rules.rules().size(), 3U);
    EXPECT_TRUE(ignores(rules, "a.o"));
    EXPECT_FALSE(ignores(rules, "keep.o"));
    EXPECT_TRUE(ignores(rules, "last"));
    EXPECT_EQ(rules.rules()[2].line, 3U);
}

TEST(IgnoreRules, NegationAndTheLastMatchingRuleDecides) {
    const auto rules = IgnoreRules::parse("*.log\n!important.log\nimportant.log.*\n\\!bang\n");
    EXPECT_TRUE(ignores(rules, "debug.log"));
    EXPECT_FALSE(ignores(rules, "important.log"));
    const auto* rule = rules.lastMatch("important.log", false);
    ASSERT_NE(rule, nullptr);
    EXPECT_TRUE(rule->negated);
    EXPECT_EQ(rule->line, 2U);
    EXPECT_TRUE(ignores(rules, "important.log.1"));
    EXPECT_TRUE(ignores(rules, "!bang"));
    EXPECT_EQ(rules.lastMatch("readme.md", false), nullptr);
    // Order matters: the later rule wins.
    EXPECT_TRUE(ignores("!a.log\n*.log", "a.log"));
}

TEST(IgnoreRules, DirectoryOnlyPatterns) {
    const auto rules = IgnoreRules::parse("out/\n");
    EXPECT_TRUE(ignores(rules, "out", true));
    EXPECT_FALSE(ignores(rules, "out", false));
    EXPECT_TRUE(ignores(rules, "src/out", true)); // No other slash: any depth.
    EXPECT_TRUE(rules.rules()[0].directoryOnly);
    EXPECT_FALSE(rules.rules()[0].anchored);
}

TEST(IgnoreRules, AnchoredAndFloatingPatterns) {
    EXPECT_TRUE(ignores("/todo", "todo"));
    EXPECT_FALSE(ignores("/todo", "src/todo"));
    EXPECT_TRUE(ignores("todo", "src/deep/todo"));
    EXPECT_TRUE(ignores("doc/frotz", "doc/frotz"));
    EXPECT_FALSE(ignores("doc/frotz", "a/doc/frotz")); // A middle slash anchors.
    EXPECT_TRUE(ignores("doc/frotz/", "doc/frotz", true));
    EXPECT_FALSE(ignores("doc/frotz/", "doc/frotz", false));
    // '*' never crosses a slash in an anchored pattern.
    EXPECT_TRUE(ignores("doc/*.txt", "doc/a.txt"));
    EXPECT_FALSE(ignores("doc/*.txt", "doc/sub/a.txt"));
}

TEST(IgnoreRules, RulesOfASubdirectoryFileApplyBelowIt) {
    const auto rules = IgnoreRules::parse("/gen\n*.tmp\nsub/x\n", "src/lib");
    EXPECT_TRUE(ignores(rules, "src/lib/gen"));
    EXPECT_FALSE(ignores(rules, "gen"));
    EXPECT_FALSE(ignores(rules, "src/gen"));
    EXPECT_TRUE(ignores(rules, "src/lib/a/b.tmp"));
    EXPECT_FALSE(ignores(rules, "src/b.tmp"));
    EXPECT_TRUE(ignores(rules, "src/lib/sub/x"));
    EXPECT_FALSE(ignores(rules, "src/libx/b.tmp")); // A sibling with the same prefix is not below it.
    EXPECT_FALSE(ignores(rules, "src/lib", true));  // Not the directory itself.
}

TEST(IgnoreRules, Wildcards) {
    EXPECT_TRUE(ignores("*", ".hidden"));
    EXPECT_TRUE(ignores("a?c", "abc"));
    EXPECT_FALSE(ignores("a?c", "ac"));
    EXPECT_FALSE(ignores("a/?/c", "a///c"));
    EXPECT_TRUE(ignores("*.tar.*", "x.tar.gz"));
    EXPECT_TRUE(ignores("a*b*c", "aXXbYYc"));
    EXPECT_FALSE(ignores("a*b*c", "aXXbYY"));
    EXPECT_TRUE(ignores("\\*", "*"));
    EXPECT_FALSE(ignores("\\*", "x"));
    EXPECT_TRUE(ignores("\\?x", "?x"));
    EXPECT_TRUE(ignores("a\\b", "ab")); // An escaped ordinary byte is itself.
}

TEST(IgnoreRules, BracketExpressions) {
    EXPECT_TRUE(ignores("file[0-9].txt", "file7.txt"));
    EXPECT_FALSE(ignores("file[0-9].txt", "fileA.txt"));
    EXPECT_TRUE(ignores("file[!0-9].txt", "fileA.txt"));
    EXPECT_TRUE(ignores("file[^0-9].txt", "fileA.txt"));
    EXPECT_FALSE(ignores("file[!0-9].txt", "file5.txt"));
    EXPECT_TRUE(ignores("[]x]", "]"));
    EXPECT_TRUE(ignores("[]x]", "x"));
    EXPECT_TRUE(ignores("[!]x]", "y"));
    EXPECT_FALSE(ignores("[!]x]", "]"));
    EXPECT_TRUE(ignores("[a-]", "-"));
    EXPECT_TRUE(ignores("[\\]]", "]"));
    EXPECT_TRUE(ignores("[[:digit:]]x", "4x"));
    EXPECT_TRUE(ignores("[[:upper:][:digit:]]", "Q"));
    EXPECT_FALSE(ignores("[[:upper:][:digit:]]", "q"));
    EXPECT_TRUE(ignores("[[:space:]]", " "));
    EXPECT_TRUE(ignores("[[:punct:]]", "!"));
    EXPECT_TRUE(ignores("[[:xdigit:]][[:xdigit:]]", "fF"));
    EXPECT_TRUE(ignores("[[x]", "["));          // "[x" without ":]": an ordinary '['.
    EXPECT_FALSE(ignores("a[/]b", "a/b"));      // A bracket never matches a slash...
    EXPECT_FALSE(ignores("d/a[!x]b", "d/a/b")); // ...negated ones included.
    EXPECT_TRUE(ignores("d/a[!x]b", "d/ayb"));
    EXPECT_TRUE(ignores("[\xC3\xA4-\xC3\xA4]", "\xC3")); // Byte-wise: the set holds bytes.
}

TEST(IgnoreRules, Globstars) {
    // Leading: at any depth.
    EXPECT_TRUE(ignores("**/foo", "foo"));
    EXPECT_TRUE(ignores("**/foo", "a/b/foo"));
    EXPECT_TRUE(ignores("**/foo/bar", "x/foo/bar"));
    EXPECT_FALSE(ignores("**/foo/bar", "x/foo/y/bar"));
    // Trailing: everything inside, not the directory itself.
    EXPECT_TRUE(ignores("abc/**", "abc/x"));
    EXPECT_TRUE(ignores("abc/**", "abc/x/y/z"));
    EXPECT_FALSE(ignores("abc/**", "abc", true));
    // Middle: zero or more directories.
    EXPECT_TRUE(ignores("a/**/b", "a/b"));
    EXPECT_TRUE(ignores("a/**/b", "a/x/b"));
    EXPECT_TRUE(ignores("a/**/b", "a/x/y/b"));
    EXPECT_FALSE(ignores("a/**/b", "a/xb"));
    EXPECT_TRUE(ignores("a/**/**/b", "a/b"));
    // Alone: everything.
    EXPECT_TRUE(ignores("**", "x/y"));
    // Anywhere else: an ordinary '*'.
    EXPECT_TRUE(ignores("a**b", "aXb"));
    EXPECT_FALSE(ignores("x/a**b", "x/a/b"));
    EXPECT_TRUE(ignores("**a", "ba"));
    EXPECT_FALSE(ignores("x/**a", "x/y/a"));
    EXPECT_TRUE(ignores("x/***/y", "x/m/n/y")); // Three stars in a globstar position are one globstar.
}

TEST(IgnoreRules, PatternsThatNeverMatchAreDiagnosed) {
    const auto rules = IgnoreRules::parse("[abc\n[[:nope:]]\ntrail\\\n!\n/\nok\n");
    ASSERT_EQ(rules.rules().size(), 6U);
    ASSERT_EQ(rules.diagnostics().size(), 5U);
    EXPECT_EQ(rules.diagnostics()[0].line, 1U);
    EXPECT_NE(rules.diagnostics()[0].message.find("bracket"), std::string::npos) << rules.diagnostics()[0].message;
    EXPECT_NE(rules.diagnostics()[1].message.find("nope"), std::string::npos);
    EXPECT_NE(rules.diagnostics()[2].message.find("backslash"), std::string::npos);
    EXPECT_FALSE(rules.rules()[0].valid);
    EXPECT_TRUE(rules.rules()[5].valid);
    EXPECT_FALSE(ignores(rules, "[abc"));
    EXPECT_FALSE(ignores(rules, "a"));
    EXPECT_FALSE(ignores(rules, "trail\\"));
    EXPECT_TRUE(ignores(rules, "ok"));
}

TEST(IgnoreRules, CaseFolding) {
    const auto exact = IgnoreRules::parse("Build/\n*.LOG\n[A-C]x\n");
    EXPECT_FALSE(ignores(exact, "build", true));
    EXPECT_FALSE(ignores(exact, "a.log"));
    const auto folded =
        IgnoreRules::parse("Build/\n*.LOG\n[A-C]x\n[!a]y\n", {}, {}, IgnoreOptions{.caseInsensitive = true});
    EXPECT_TRUE(ignores(folded, "BUILD", true));
    EXPECT_TRUE(ignores(folded, "a.log"));
    EXPECT_TRUE(ignores(folded, "bX"));
    EXPECT_FALSE(ignores(folded, "Ay")); // The folded set {a, A} is excluded.
    EXPECT_TRUE(ignores(folded, "by"));
    // Only ASCII folds.
    EXPECT_FALSE(ignores(IgnoreRules::parse("\xC3\x84", {}, {}, IgnoreOptions{.caseInsensitive = true}), "\xC3\xA4"));
}

TEST(IgnoreStack, DeeperFilesTakePrecedenceOverShallowerAndGlobalOnes) {
    IgnoreStack stack;
    stack = stack.pushed(IgnoreRules::parse("*.cache\n!keep.cache\n", {}, "info/exclude"));
    stack = stack.pushed(IgnoreRules::parse("*.o\n", {}, ".gitignore"));
    const IgnoreStack deep = stack.pushed(IgnoreRules::parse("!*.o\nkeep.cache\n", "vendor", "vendor/.gitignore"));
    EXPECT_TRUE(stack.match("a.o", false).ignored);
    EXPECT_TRUE(stack.match("vendor/a.o", false).ignored);
    EXPECT_FALSE(deep.match("vendor/a.o", false).ignored); // Re-included below vendor/.
    EXPECT_TRUE(deep.match("other/a.o", false).ignored);   // The vendor file does not apply here.
    EXPECT_FALSE(deep.match("keep.cache", false).ignored);
    EXPECT_TRUE(deep.match("vendor/keep.cache", false).ignored);
    const auto decided = deep.match("vendor/keep.cache", false);
    ASSERT_NE(decided.rules, nullptr);
    EXPECT_EQ(decided.rules->source(), "vendor/.gitignore");
    EXPECT_EQ(decided.rule->line, 2U);
    EXPECT_FALSE(deep.match("readme", false).ignored);
    EXPECT_EQ(deep.match("readme", false).rule, nullptr);
    // pushed() left the original alone; empty rule sets are not pushed.
    EXPECT_TRUE(stack.match("vendor/a.o", false).ignored);
    EXPECT_TRUE(IgnoreStack().pushed(IgnoreRules::parse("# nothing\n")).empty());
}

TEST(IgnoreStack, NothingBelowAnIgnoredDirectoryCanBeReIncluded) {
    const IgnoreStack stack = IgnoreStack().pushed(IgnoreRules::parse("build/\n!build/keep.txt\n"));
    // Its own rules would re-include the file...
    EXPECT_FALSE(stack.match("build/keep.txt", false).ignored);
    // ...but its parent directory is ignored, which decides.
    const auto decided = stack.matchWithParents("build/keep.txt", false);
    EXPECT_TRUE(decided.ignored);
    ASSERT_NE(decided.rule, nullptr);
    EXPECT_EQ(decided.rule->line, 1U);
    // "dir/*" ignores the entries, not the directory: re-including works.
    const IgnoreStack entries = IgnoreStack().pushed(IgnoreRules::parse("build/*\n!build/keep.txt\n"));
    EXPECT_FALSE(entries.matchWithParents("build/keep.txt", false).ignored);
    EXPECT_TRUE(entries.matchWithParents("build/other.txt", false).ignored);
}

TEST(IgnoreRules, PathologicalPatternsStayLinear) {
    // A backtracking matcher needs exponential time for these; the NFA is O(tokens x bytes).
    std::string pattern;
    for (int i = 0; i < 40; ++i) {
        pattern += "*a";
    }
    pattern += "*c*b";
    const auto rules = IgnoreRules::parse(pattern + "\n" + "**/" + pattern + "/**/x\n");
    const std::string name(20'000, 'a');
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(ignores(rules, name + "b")); // Ends with the tail, but has no 'c'.
    EXPECT_TRUE(ignores(rules, name + "cb"));
    EXPECT_TRUE(ignores(rules, name + "/" + name + "cb/q/x")); // The anchored rule.
    EXPECT_FALSE(ignores(rules, name + "/" + name + "b/q/x"));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(10));
}

} // namespace
