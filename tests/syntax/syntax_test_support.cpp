// SPDX-License-Identifier: Apache-2.0
//
// Shared helpers for frost::syntax's tests (ADR 0023): samples, the fresh-parse oracle and span
// lookups (see syntax_test_support.hpp).

#include "syntax/syntax_test_support.hpp"

#include "frost/sched/sim_executor.hpp"
#include "frost/syntax/outline.hpp"
#include "frost/syntax/syntax_session.hpp"
#include "frost/text/text_buffer.hpp"

#include "language_state.hpp"
#include "tree_state.hpp"
#include "ts.hpp"

namespace frost::syntax::testing {

namespace {

constexpr std::string_view kC = R"(/** Doc comment. */
#include <stdio.h>
#define MAX_SIZE 16
struct point { int x; int y; };
typedef unsigned long size_type;
enum color { RED, GREEN };
static int add(int a, int b) { return a + b; } // sum
int main(void) {
    char c = 'x';
    printf("%d\n", add(1, 2));
    return MAX_SIZE;
}
)";

constexpr std::string_view kCpp = R"(// Line comment
#include "frost/core/error.hpp"
namespace frost::demo {
/// Doc comment.
template<class T>
class Box {
public:
    explicit Box(T value) : m_value(value) {}
    ~Box() = default;
    T get() const noexcept { return m_value; }
    void set(T value);
private:
    T m_value;
};
enum class Mode { Fast, Slow };
inline constexpr int kLimit = 0x10;
auto twice(int x) -> int { return x * 2; }
} // namespace frost::demo
int main() {
    frost::demo::Box<int> box{1};
    const char* text = R"x(raw)x";
    bool ok = true && box.get() == 1;
    return ok ? twice(3) : 0;
}
)";

constexpr std::string_view kCSharp = R"(using System.Collections.Generic;
namespace Frost.Demo
{
    /// <summary>Doc comment.</summary>
    [Serializable]
    public sealed class Widget : IDisposable
    {
        private readonly int _count = 42;
        public string Name { get; set; } = "widget";
        public event EventHandler Changed;
        public Widget(int count) { _count = count; }
        public void Dispose() { Console.WriteLine($"bye {Name}\n"); }
        #region Helpers
        private static bool IsReady(char c) => c == 'x' || true;
        #endregion
    }
    public enum Color { Red, Green }
    interface IShape { double Area(); }
}
)";

constexpr std::string_view kCMake = R"(# Comment
cmake_minimum_required(VERSION 3.21)
project(Demo LANGUAGES CXX)
set(SOURCES main.cpp "util.cpp")
function(frost_helper name)
    message(STATUS "helper ${name}")
endfunction()
add_library(demo STATIC ${SOURCES})
if(WIN32 AND ON)
    target_compile_definitions(demo PRIVATE WIN32_LEAN_AND_MEAN)
endif()
add_executable(demo_app main.cpp)
)";

constexpr std::string_view kXml = R"(<?xml version="1.0" encoding="utf-8"?>
<!-- Comment -->
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <Text>a &amp; b</Text>
  </PropertyGroup>
  <ItemGroup><Compile Include="a.cs" /></ItemGroup>
</Project>
)";

constexpr std::string_view kJson = R"({
  "name": "frost",
  "version": 1.5,
  "tags": ["a\nb", true, null],
  "nested": { "inner": false }
}
)";

constexpr std::string_view kMarkdown = R"(# Title

Some text.

## Section

- item one
- item two

> quoted

```cpp
int x;
```
)";

} // namespace

std::string_view sample(LanguageId language) noexcept {
    switch (language) {
    case LanguageId::C: return kC;
    case LanguageId::Cpp: return kCpp;
    case LanguageId::CSharp: return kCSharp;
    case LanguageId::CMake: return kCMake;
    case LanguageId::Xml: return kXml;
    case LanguageId::Json: return kJson;
    case LanguageId::Markdown: return kMarkdown;
    case LanguageId::None: break;
    }
    return {};
}

std::string normaliseLoneCr(std::string_view text) {
    std::string out(text);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r' && (i + 1 == out.size() || out[i + 1] != '\n')) {
            out[i] = '\n';
        }
    }
    return out;
}

std::string freshDump(LanguageId language, std::string_view text) {
    static const LanguageRegistry registry;
    const Result<const Language*> found = registry.get(language);
    if (!found) {
        return "no grammar";
    }
    const std::string normalised = normaliseLoneCr(text);
    detail::ParserPtr parser(ts_parser_new());
    if (!ts_parser_set_language(parser.get(), (*found)->grammar)) {
        return "set_language failed";
    }
    detail::TreePtr tree(ts_parser_parse_string(parser.get(), nullptr, normalised.data(),
                                                static_cast<std::uint32_t>(normalised.size())));
    if (!tree) {
        return "parse failed";
    }
    return detail::dumpNode(ts_tree_root_node(tree.get()), true);
}

std::string kindAt(const SyntaxTree& tree, std::string_view needle, std::size_t from) {
    const std::string text = tree.text().text();
    const std::size_t at = text.find(needle, from);
    if (at == std::string::npos) {
        return "<absent>";
    }
    const HighlightResult result =
        tree.highlights(text::ByteRange{ByteOffset{0}, ByteOffset{text.size()}}, unlimited());
    for (const HighlightSpan& span : result.spans) {
        if (span.range.begin.value() <= at && at + needle.size() <= span.range.end.value()) {
            return std::string(captureName(span.kind));
        }
    }
    return "<none>";
}

std::string outlineText(const SyntaxTree& tree) {
    const Outline outline = tree.outline(unlimited());
    std::string out;
    for (const OutlineItem& item : outline.items) {
        out.append(2 * static_cast<std::size_t>(item.depth), ' ');
        out += outlineCaptureSuffix(item.kind);
        out += ' ';
        out += item.name;
        out += '\n';
    }
    if (!outline.complete) {
        out += "<incomplete>\n";
    }
    return out;
}

SyntaxTree parseNow(LanguageId language, std::string_view text) {
    static const LanguageRegistry registry;
    if (!registry.prepare(language)) {
        return {};
    }
    Result<text::TextBuffer> buffer = text::TextBuffer::fromUtf8(std::string(text));
    if (!buffer) {
        return {};
    }
    sched::SimExecutor sim;
    SyntaxSession session(registry, sim, sim);
    session.open(language, buffer->current());
    sim.runUntilIdle();
    (void)session.poll();
    if (session.state() != SyntaxState::Current) {
        return {};
    }
    return session.tree();
}

QueryLimits unlimited() noexcept {
    QueryLimits limits;
    limits.budget = IClock::Duration::zero();
    limits.matchLimit = 0xFFFF'FFFF;
    return limits;
}

} // namespace frost::syntax::testing
