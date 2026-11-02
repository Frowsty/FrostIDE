// SPDX-License-Identifier: Apache-2.0
//
// frost::syntax highlights and outlines (ADR 0023): the kind of representative tokens in a small
// sample of every grammar, span invariants, the outline (which powers next/previous member) of
// every sample, and enclosingRanges.

#include "frost/core/clock.hpp"
#include "frost/syntax/highlight.hpp"
#include "frost/syntax/outline.hpp"
#include "frost/syntax/syntax_tree.hpp"

#include "syntax/syntax_test_support.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using frost::ByteOffset;
using frost::syntax::HighlightResult;
using frost::syntax::HighlightSpan;
using frost::syntax::LanguageId;
using frost::syntax::SyntaxTree;
using frost::syntax::testing::kindAt;
using frost::syntax::testing::outlineText;
using frost::syntax::testing::parseNow;
using frost::syntax::testing::sample;
using frost::text::ByteRange;

// One expectation: the first `needle` at or after the first `after` (empty: the start) has `kind`.
struct Expect {
    std::string_view needle;
    std::string_view kind;
    std::string_view after = {};
};

void expectKinds(LanguageId language, const std::vector<Expect>& expectations) {
    const SyntaxTree tree = parseNow(language, sample(language));
    ASSERT_FALSE(tree.empty());
    EXPECT_FALSE(tree.hasError()) << tree.dumpForTesting(true);
    const std::string text = tree.text().text();
    for (const Expect& e : expectations) {
        const std::size_t from = e.after.empty() ? 0 : text.find(e.after);
        ASSERT_NE(from, std::string::npos) << e.after;
        EXPECT_EQ(kindAt(tree, e.needle, from), e.kind) << "needle '" << e.needle << "' after '" << e.after << "'";
    }
}

TEST(SyntaxHighlight, C) {
    expectKinds(LanguageId::C, {
                                   {"/** Doc comment. */", "comment.doc"},
                                   {"#include", "keyword.directive"},
                                   {"<stdio.h>", "string"},
                                   {"#define", "keyword.directive"},
                                   {"MAX_SIZE", "macro"},
                                   {"1", "number", "add("},
                                   {"struct", "keyword"},
                                   {"point", "type"},
                                   {"int", "type.builtin", "{ "},
                                   {"x", "property", "int x"},
                                   {"unsigned long", "type.builtin"},
                                   {"size_type", "type"},
                                   {"RED", "constant"},
                                   {"static", "keyword"},
                                   {"add", "function"},
                                   {"a", "parameter", "(int a"},
                                   {"+", "operator"},
                                   {"// sum", "comment"},
                                   {"'x'", "character"},
                                   {"printf", "function.call"},
                                   {"%d", "string", "printf"},
                                   {"\\n", "string.escape", "printf"},
                                   {"MAX_SIZE", "constant", "return"},
                                   {"{", "punctuation.bracket"},
                                   {";", "punctuation"},
                               });
}

TEST(SyntaxHighlight, LuaLongStringsCommentsAndNestedBlocks) {
    constexpr std::string_view source = "--[=[ long comment\nend ]=]\n"
                                        "local function outer(x)\n  local message = [=[if then\n\xC3\xA5 end]=]\n"
                                        "  if x then\n    return message\n  end\nend\n";
    const SyntaxTree tree = parseNow(LanguageId::Lua, source);
    ASSERT_FALSE(tree.empty());
    EXPECT_FALSE(tree.hasError()) << tree.dumpForTesting(true);
    EXPECT_EQ(kindAt(tree, "long comment"), "comment");
    EXPECT_EQ(kindAt(tree, "if then"), "string");
    EXPECT_EQ(kindAt(tree, "outer"), "function");
    EXPECT_EQ(kindAt(tree, "return"), "keyword");
    const auto folds = tree.foldRanges(frost::syntax::testing::unlimited());
    ASSERT_TRUE(folds.complete);
    EXPECT_GE(folds.ranges.size(), 4U);
    const auto containsFold = [&](std::string_view needle) {
        for (const auto& range : folds.ranges) {
            if (source
                    .substr(static_cast<std::size_t>(range.begin.value()),
                            static_cast<std::size_t>(range.end.value() - range.begin.value()))
                    .starts_with(needle)) {
                return true;
            }
        }
        return false;
    };
    EXPECT_TRUE(containsFold("--[=[ long comment"));
    EXPECT_TRUE(containsFold("[=[if then"));
    EXPECT_TRUE(containsFold("local function outer"));
    EXPECT_TRUE(containsFold("if x then"));
    EXPECT_EQ(outlineText(tree), "function outer\n");
}

TEST(SyntaxHighlight, Cpp) {
    expectKinds(LanguageId::Cpp, {
                                     {"// Line comment", "comment"},
                                     {"\"frost/core/error.hpp\"", "string"},
                                     {"namespace", "keyword"},
                                     {"frost", "namespace", "namespace "},
                                     {"demo", "namespace", "namespace "},
                                     {"/// Doc comment.", "comment.doc"},
                                     {"template", "keyword"},
                                     {"Box", "type"},
                                     {"public", "keyword"},
                                     {"explicit", "keyword"},
                                     {"Box", "function", "explicit"},
                                     {"value", "parameter", "explicit"},
                                     {"Box", "function", "~"},
                                     {"~", "operator"},
                                     {"get", "function"},
                                     {"noexcept", "keyword"},
                                     {"m_value", "property", "private"},
                                     {"Fast", "constant"},
                                     {"constexpr", "keyword"},
                                     {"0x10", "number"},
                                     {"auto", "type.builtin"},
                                     {"twice", "function"},
                                     {"->", "operator", "twice"},
                                     {"R\"x(raw)x\"", "string"},
                                     {"true", "boolean"},
                                     {"&&", "operator"},
                                     {"get", "function.call", "box."},
                                     {"twice", "function.call", "return ok"},
                                     {"::", "operator", "frost::demo::Box<int>"},
                                 });
}

TEST(SyntaxHighlight, CSharp) {
    expectKinds(LanguageId::CSharp, {
                                        {"using", "keyword"},
                                        {"System", "namespace"},
                                        {"Generic", "namespace"},
                                        {"Demo", "namespace"},
                                        {"/// <summary>Doc comment.</summary>", "comment.doc"},
                                        {"Serializable", "attribute"},
                                        {"sealed", "keyword"},
                                        {"Widget", "type"},
                                        {"IDisposable", "type"},
                                        {"int", "type.builtin"},
                                        {"42", "number"},
                                        {"Name", "property"},
                                        {"\"widget\"", "string"},
                                        {"Changed", "variable"},
                                        {"Widget", "function", "public Widget"},
                                        {"count", "parameter", "int count"},
                                        {"Dispose", "function"},
                                        {"WriteLine", "function.call"},
                                        {"\\n", "string.escape", "WriteLine"},
                                        {"bye", "string"},
                                        {"#region Helpers", "keyword.directive"},
                                        {"'x'", "character"},
                                        {"true", "boolean", "'x'"},
                                        {"=>", "operator"},
                                        {"Red", "constant"},
                                        {"interface", "keyword"},
                                    });
}

TEST(SyntaxHighlight, CMake) {
    expectKinds(LanguageId::CMake, {
                                       {"# Comment", "comment"},
                                       {"cmake_minimum_required", "function.call"},
                                       {"VERSION", "<none>"},
                                       {"\"util.cpp\"", "string"},
                                       {"function", "keyword"},
                                       {"${name}", "variable"},
                                       {"\"helper ", "string"},
                                       {"endfunction", "keyword"},
                                       {"STATIC", "keyword"},
                                       {"${SOURCES}", "variable"},
                                       {"if", "keyword", "${SOURCES}"},
                                       {"ON", "boolean", "WIN32 AND"},
                                       {"AND", "keyword"},
                                       {"PRIVATE", "keyword"},
                                       {"endif", "keyword"},
                                       {"(", "punctuation.bracket"},
                                   });
}

TEST(SyntaxHighlight, Xml) {
    expectKinds(LanguageId::Xml, {
                                     {"version", "keyword.directive"},
                                     {"<?", "keyword.directive"},
                                     {"<!-- Comment -->", "comment"},
                                     {"Project", "tag"},
                                     {"Sdk", "tag.attribute"},
                                     {"\"Microsoft.NET.Sdk\"", "string"},
                                     {"PropertyGroup", "tag", "</TargetFramework>"},
                                     {"&amp;", "string.escape"},
                                     {"a.cs", "string"},
                                     {"/>", "punctuation.bracket"},
                                     {"net8.0", "<none>"},
                                 });
}

TEST(SyntaxHighlight, Json) {
    expectKinds(LanguageId::Json, {
                                      {"\"name\"", "property"},
                                      {"\"frost\"", "string"},
                                      {"1.5", "number"},
                                      {"\\n", "string.escape"},
                                      {"true", "boolean"},
                                      {"null", "constant"},
                                      {"\"inner\"", "property"},
                                      {"false", "boolean"},
                                      {"{", "punctuation.bracket"},
                                      {":", "punctuation"},
                                  });
}

TEST(SyntaxHighlight, Markdown) {
    expectKinds(LanguageId::Markdown, {
                                          {"# Title", "markup.heading"},
                                          {"## Section", "markup.heading"},
                                          {"Some text.", "<none>"},
                                          {"- ", "markup.list"},
                                          {"> quoted", "markup.quote"},
                                          {"cpp", "label"},
                                          {"int x;", "markup.raw"},
                                      });
}

TEST(SyntaxHighlight, SpansAreSortedDisjointAndClipped) {
    for (const LanguageId id : frost::syntax::kAllLanguages) {
        const SyntaxTree tree = parseNow(id, sample(id));
        ASSERT_FALSE(tree.empty()) << frost::syntax::toString(id);
        const std::uint64_t size = tree.text().size().value();
        // A window in the middle: spans are clipped to it, sorted and disjoint.
        const ByteRange window{ByteOffset{size / 4}, ByteOffset{size / 2}};
        const HighlightResult result = tree.highlights(window, frost::syntax::testing::unlimited());
        EXPECT_TRUE(result.complete);
        ASSERT_FALSE(result.spans.empty()) << frost::syntax::toString(id);
        std::uint64_t last = window.begin.value();
        for (const HighlightSpan& span : result.spans) {
            EXPECT_LE(last, span.range.begin.value()) << frost::syntax::toString(id);
            EXPECT_LT(span.range.begin.value(), span.range.end.value());
            EXPECT_LE(span.range.end.value(), window.end.value());
            last = span.range.end.value();
        }
    }
}

TEST(SyntaxHighlight, LimitsMarkTheResultIncomplete) {
    const SyntaxTree tree = parseNow(LanguageId::Cpp, sample(LanguageId::Cpp));
    ASSERT_FALSE(tree.empty());
    frost::syntax::QueryLimits limits = frost::syntax::testing::unlimited();
    limits.maxResults = 3;
    const HighlightResult capped = tree.highlights(ByteRange{ByteOffset{0}, tree.text().size()}, limits);
    EXPECT_FALSE(capped.complete);
    EXPECT_LE(capped.spans.size(), 3u);
    // An empty tree highlights nothing.
    const HighlightResult none = SyntaxTree{}.highlights(ByteRange{});
    EXPECT_TRUE(none.spans.empty());
    EXPECT_FALSE(none.complete);
}

// A clock that moves one second forward on every read: any deadline passes at its first check.
class SteppingClock final : public frost::IClock {
public:
    [[nodiscard]] SteadyTime steadyNow() const noexcept override {
        return SteadyTime(Duration(++m_reads * 1'000'000'000LL));
    }
    [[nodiscard]] SystemTime systemNow() const noexcept override { return SystemTime(Duration(m_reads)); }

private:
    mutable std::int64_t m_reads = 0;
};

TEST(SyntaxHighlight, BudgetedQueriesHaltAtTheDeadline) {
    // Regression: the query cursor keeps a pointer to its options, which once lived on the stack
    // of QueryRun's constructor.
    std::string text;
    for (int i = 0; i < 400; ++i) {
        text += "int value" + std::to_string(i) + " = " + std::to_string(i) + "; // note\n";
    }
    const SyntaxTree tree = parseNow(LanguageId::C, text);
    ASSERT_FALSE(tree.empty());
    const ByteRange all{ByteOffset{0}, tree.text().size()};
    // A budget that never runs out (a manual clock does not move): complete.
    const frost::ManualClock still;
    frost::syntax::QueryLimits patient;
    patient.clock = &still;
    patient.budget = std::chrono::seconds(1);
    const HighlightResult full = tree.highlights(all, patient);
    EXPECT_TRUE(full.complete);
    EXPECT_EQ(full.spans, tree.highlights(all, frost::syntax::testing::unlimited()).spans);
    EXPECT_TRUE(tree.outline(patient).complete);
    // A budget that has run out at the first progress check: incomplete, but well formed.
    const SteppingClock stepping;
    frost::syntax::QueryLimits hasty;
    hasty.clock = &stepping;
    hasty.budget = std::chrono::nanoseconds(1);
    const HighlightResult cut = tree.highlights(all, hasty);
    EXPECT_FALSE(cut.complete);
    EXPECT_LT(cut.spans.size(), full.spans.size());
}

// --- Outline ---

TEST(SyntaxOutline, C) {
    EXPECT_EQ(outlineText(parseNow(LanguageId::C, sample(LanguageId::C))), "macro MAX_SIZE\n"
                                                                           "struct point\n"
                                                                           "  field x\n"
                                                                           "  field y\n"
                                                                           "type_alias size_type\n"
                                                                           "enum color\n"
                                                                           "  enum_member RED\n"
                                                                           "  enum_member GREEN\n"
                                                                           "function add\n"
                                                                           "function main\n");
}

TEST(SyntaxOutline, Cpp) {
    EXPECT_EQ(outlineText(parseNow(LanguageId::Cpp, sample(LanguageId::Cpp))), "namespace frost::demo\n"
                                                                               "  class Box\n"
                                                                               "    constructor Box\n"
                                                                               "    destructor ~Box\n"
                                                                               "    method get\n"
                                                                               "    method set\n"
                                                                               "    field m_value\n"
                                                                               "  enum Mode\n"
                                                                               "    enum_member Fast\n"
                                                                               "    enum_member Slow\n"
                                                                               "  function twice\n"
                                                                               "function main\n");
}

TEST(SyntaxOutline, CSharp) {
    EXPECT_EQ(outlineText(parseNow(LanguageId::CSharp, sample(LanguageId::CSharp))), "namespace Frost.Demo\n"
                                                                                     "  class Widget\n"
                                                                                     "    field _count\n"
                                                                                     "    property Name\n"
                                                                                     "    event Changed\n"
                                                                                     "    constructor Widget\n"
                                                                                     "    method Dispose\n"
                                                                                     "    method IsReady\n"
                                                                                     "  enum Color\n"
                                                                                     "    enum_member Red\n"
                                                                                     "    enum_member Green\n"
                                                                                     "  interface IShape\n"
                                                                                     "    method Area\n");
}

TEST(SyntaxOutline, CMake) {
    EXPECT_EQ(outlineText(parseNow(LanguageId::CMake, sample(LanguageId::CMake))), "command frost_helper\n"
                                                                                   "target demo\n"
                                                                                   "target demo_app\n");
}

TEST(SyntaxOutline, Xml) {
    EXPECT_EQ(outlineText(parseNow(LanguageId::Xml, sample(LanguageId::Xml))), "element Project\n"
                                                                               "  element PropertyGroup\n"
                                                                               "    element TargetFramework\n"
                                                                               "    element Text\n"
                                                                               "  element ItemGroup\n"
                                                                               "    element Compile\n");
}

TEST(SyntaxOutline, Json) {
    EXPECT_EQ(outlineText(parseNow(LanguageId::Json, sample(LanguageId::Json))), "key name\n"
                                                                                 "key version\n"
                                                                                 "key tags\n"
                                                                                 "key nested\n"
                                                                                 "  key inner\n");
}

TEST(SyntaxOutline, Markdown) {
    EXPECT_EQ(outlineText(parseNow(LanguageId::Markdown, sample(LanguageId::Markdown))), "heading Title\n"
                                                                                         "  heading Section\n");
}

TEST(SyntaxOutline, ParentsDepthsAndRanges) {
    const SyntaxTree tree = parseNow(LanguageId::Cpp, sample(LanguageId::Cpp));
    const frost::syntax::Outline outline = tree.outline(frost::syntax::testing::unlimited());
    ASSERT_TRUE(outline.complete);
    const std::string text = tree.text().text();
    for (std::size_t i = 0; i < outline.items.size(); ++i) {
        const frost::syntax::OutlineItem& item = outline.items[i];
        // The name range lies inside the item and spells the name.
        EXPECT_LE(item.range.begin, item.nameRange.begin);
        EXPECT_LE(item.nameRange.end, item.range.end);
        EXPECT_EQ(text.substr(item.nameRange.begin.value(), item.nameRange.size()), item.name);
        if (item.parent == frost::syntax::kNoParent) {
            EXPECT_EQ(item.depth, 0u);
            continue;
        }
        ASSERT_LT(item.parent, i);
        const frost::syntax::OutlineItem& parent = outline.items[item.parent];
        EXPECT_EQ(item.depth, parent.depth + 1);
        EXPECT_LE(parent.range.begin, item.range.begin);
        EXPECT_LE(item.range.end, parent.range.end);
    }
    // maxDepth drops deeper items: depth 0 only keeps the namespace and main.
    frost::syntax::QueryLimits shallow = frost::syntax::testing::unlimited();
    shallow.maxDepth = 1;
    const frost::syntax::Outline top = tree.outline(shallow);
    ASSERT_EQ(top.items.size(), 2u);
    EXPECT_EQ(top.items[0].name, "frost::demo");
    EXPECT_EQ(top.items[1].name, "main");
}

// --- Enclosing ranges ---

TEST(SyntaxEnclosing, GrowsFromTheInnermostNamedNodeToTheRoot) {
    const SyntaxTree tree = parseNow(LanguageId::C, sample(LanguageId::C));
    const std::string text = tree.text().text();
    const std::size_t at = text.find("a + b");
    ASSERT_NE(at, std::string::npos);
    // A caret on 'b'.
    const ByteRange caret{ByteOffset{at + 4}, ByteOffset{at + 4}};
    const std::vector<ByteRange> ranges = tree.enclosingRanges(caret);
    ASSERT_GE(ranges.size(), 4u);
    EXPECT_EQ(text.substr(ranges[0].begin.value(), ranges[0].size()), "b");
    EXPECT_EQ(text.substr(ranges[1].begin.value(), ranges[1].size()), "a + b");
    EXPECT_EQ(text.substr(ranges[2].begin.value(), ranges[2].size()), "return a + b;");
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        EXPECT_LE(ranges[i].begin, ranges[i - 1].begin);
        EXPECT_LE(ranges[i - 1].end, ranges[i].end);
        EXPECT_NE(ranges[i], ranges[i - 1]);
    }
    EXPECT_EQ(ranges.back().begin.value(), 0u);
    EXPECT_EQ(ranges.back().end.value(), text.size());
    EXPECT_TRUE(SyntaxTree{}.enclosingRanges(caret).empty());
}

} // namespace
