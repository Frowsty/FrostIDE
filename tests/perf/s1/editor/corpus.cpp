// SPDX-License-Identifier: Apache-2.0
//
// Implementation of the S1 corpus generator and lexer (see corpus.hpp).

#include "corpus.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <span>
#include <string>

namespace frost::s1 {

namespace {

// xorshift64* (Vigna, public domain): small, fast and deterministic across platforms.
class Random {
public:
    explicit Random(std::uint64_t seed) : m_state(seed != 0 ? seed : 0x9E3779B97F4A7C15ULL) {}
    std::uint64_t next() {
        m_state ^= m_state >> 12U;
        m_state ^= m_state << 25U;
        m_state ^= m_state >> 27U;
        return m_state * 0x2545F4914F6CDD1DULL;
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }

private:
    std::uint64_t m_state;
};

constexpr std::array<std::string_view, 24> kNames{"buffer", "index",  "count",  "result", "offset",  "length",
                                                  "value",  "entry",  "line",   "column", "context", "parser",
                                                  "token",  "source", "target", "range",  "anchor",  "cursor",
                                                  "layout", "glyph",  "width",  "height", "cache",   "node"};
constexpr std::array<std::string_view, 10> kTypes{"std::size_t", "std::string",  "int",     "bool",    "double",
                                                  "auto",        "Result<Node>", "QString", "Span<T>", "Range"};
constexpr std::array<std::string_view, 12> kWords{"the",   "piece", "tree",   "keeps",   "every",  "edit",
                                                  "until", "the",   "buffer", "flushes", "layout", "again"};

std::string_view pick(Random& random, std::span<const std::string_view> items) {
    return items[random.below(items.size())];
}

std::string comment(Random& random, std::size_t words) {
    std::string text = "// ";
    for (std::size_t i = 0; i < words; ++i) {
        if (i > 0) {
            text += ' ';
        }
        text += pick(random, kWords);
    }
    return text;
}

std::string statement(Random& random) {
    const auto a = pick(random, kNames);
    const auto b = pick(random, kNames);
    const auto c = pick(random, kNames);
    switch (random.below(9)) {
    case 0: return std::format("{} {} = {}.{}({}, {});", pick(random, kTypes), a, b, c, random.below(4096), a);
    case 1: return std::format("if ({} < {} && !{}.empty()) {{", a, b, c);
    case 2: return std::format("for (std::size_t {} = 0; {} < {}.size(); ++{}) {{", a, a, b, a);
    case 3: return std::format("return {}::{}({}, \"{} {}\");", a, b, c, pick(random, kWords), b);
    case 4: return std::format("{}[{}] = 0x{:04X} + {} * {};", a, b, random.below(65536), c, random.below(100));
    case 5: return std::format("while ({}->{} != nullptr) {{", a, b);
    case 6: return std::format("const auto {} = std::min({}, {} - {});", a, b, c, random.below(64));
    case 7: return std::format("{}.push_back({{{}, {}, {}}});", a, b, c, random.below(10));
    default: return comment(random, 4 + random.below(8));
    }
}

bool isKeyword(std::string_view word) {
    constexpr std::array<std::string_view, 22> kKeywords{
        "if",     "for",    "while",    "return",   "const",    "auto",      "static", "namespace",
        "class",  "struct", "template", "typename", "nullptr",  "constexpr", "void",   "else",
        "switch", "case",   "break",    "continue", "noexcept", "override"};
    return std::ranges::find(kKeywords, word) != kKeywords.end();
}

bool isType(std::string_view word) {
    constexpr std::array<std::string_view, 9> kTypeNames{"int",    "bool",  "double",  "std",   "size_t",
                                                         "string", "Range", "QString", "Result"};
    return std::ranges::find(kTypeNames, word) != kTypeNames.end();
}

bool isIdentifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

} // namespace

std::vector<std::string> makeCorpus(std::size_t lineCount, std::uint64_t seed) {
    Random random(seed);
    std::vector<std::string> lines;
    lines.reserve(lineCount);
    int depth = 0;
    while (lines.size() < lineCount) {
        const std::size_t roll = random.below(100);
        std::string line;
        if (depth == 0) {
            if (roll < 10) {
                line = "#include <" + std::string(pick(random, kNames)) + ".hpp>";
            } else if (roll < 30) {
                line = comment(random, 6 + random.below(10));
            } else if (roll < 40) {
                line.clear(); // Blank line between functions.
            } else {
                line = std::format("{} {}::{}({} {}, {} {}) {{", pick(random, kTypes), pick(random, kNames),
                                   pick(random, kNames), pick(random, kTypes), pick(random, kNames),
                                   pick(random, kTypes), pick(random, kNames));
                ++depth;
            }
        } else if (roll < 3) {
            // A long line: generated tables and long string literals exist in real code.
            line = std::string(static_cast<std::size_t>(depth) * 4, ' ') + "constexpr char kTable[] = \"";
            const std::size_t length = 250 + random.below(250);
            while (line.size() < length) {
                line += pick(random, kWords);
                line += ' ';
            }
            line += "\";";
        } else if (roll < 18 || depth > 5) {
            --depth;
            line = std::string(static_cast<std::size_t>(depth) * 4, ' ') + "}";
        } else {
            line = std::string(static_cast<std::size_t>(depth) * 4, ' ') + statement(random);
            if (line.back() == '{') {
                ++depth;
            }
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

void classifyLine(std::string_view line, std::vector<TokenClass>& out) {
    out.assign(line.size(), TokenClass::Text);
    std::size_t first = line.find_first_not_of(' ');
    if (first != std::string_view::npos && line[first] == '#') {
        std::fill(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(), TokenClass::Preprocessor);
        return;
    }
    std::size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(i), out.end(), TokenClass::Comment);
            return;
        }
        if (c == '"') {
            std::size_t end = line.find('"', i + 1);
            end = end == std::string_view::npos ? line.size() : end + 1;
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(i), out.begin() + static_cast<std::ptrdiff_t>(end),
                      TokenClass::String);
            i = end;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            std::size_t end = i;
            while (end < line.size() && isIdentifierChar(line[end])) {
                ++end;
            }
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(i), out.begin() + static_cast<std::ptrdiff_t>(end),
                      TokenClass::Number);
            i = end;
            continue;
        }
        if (isIdentifierChar(c)) {
            std::size_t end = i;
            while (end < line.size() && isIdentifierChar(line[end])) {
                ++end;
            }
            const std::string_view word = line.substr(i, end - i);
            const TokenClass cls = isKeyword(word) ? TokenClass::Keyword
                                   : isType(word)  ? TokenClass::Type
                                                   : TokenClass::Text;
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(i), out.begin() + static_cast<std::ptrdiff_t>(end),
                      cls);
            i = end;
            continue;
        }
        if (c != ' ') {
            out[i] = TokenClass::Punctuation;
        }
        ++i;
    }
}

TextModel::TextModel(std::vector<std::string> lines)
    : m_lines(std::move(lines)), m_classes(m_lines.size()), m_revisions(m_lines.size(), 0) {
    for (std::size_t i = 0; i < m_lines.size(); ++i) {
        classifyLine(m_lines[i], m_classes[i]);
    }
}

std::size_t TextModel::insert(std::size_t line, std::size_t column, char c) {
    std::string& text = m_lines[line];
    column = std::min(column, text.size());
    text.insert(text.begin() + static_cast<std::ptrdiff_t>(column), c);
    classifyLine(text, m_classes[line]);
    ++m_revisions[line];
    return column + 1;
}

double Fling::offsetPx(double seconds) const noexcept {
    return velocityPxPerS * tauS * (1.0 - std::exp(-seconds / tauS));
}

double Fling::velocityAt(double seconds) const noexcept { return velocityPxPerS * std::exp(-seconds / tauS); }

} // namespace frost::s1
