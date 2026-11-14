// SPDX-License-Identifier: Apache-2.0
//
// The LSP snippet parser (snippet.hpp): recursive descent over the specification's grammar, with
// malformed constructs kept as literal text.

#include "frost/lang/snippet.hpp"

#include <algorithm>
#include <limits>

namespace frost::lang {
namespace {

[[nodiscard]] constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
[[nodiscard]] constexpr bool isVariableStart(char c) noexcept {
    return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
[[nodiscard]] constexpr bool isVariableChar(char c) noexcept { return isVariableStart(c) || isDigit(c); }

class Parser {
public:
    explicit Parser(std::string_view snippet) : m_s(snippet),
        m_budget(std::min(snippet.size(), (std::numeric_limits<std::size_t>::max()-64)/8)*8+64) {}

    ParsedSnippet run() {
        parseAny(false);
        if (m_exhausted) return ParsedSnippet{std::string(m_s),{},true};
        ParsedSnippet out;
        out.text = std::move(m_out);
        out.tabStops = std::move(m_stops);
        std::ranges::stable_sort(out.tabStops, [](const SnippetTabStop& a, const SnippetTabStop& b) {
            const auto key = [](std::uint32_t index) { return index == 0 ? UINT32_MAX : index; };
            return key(a.index) < key(b.index);
        });
        return out;
    }

private:
    bool work() {
        if (!m_budget) { m_exhausted = true; return false; }
        --m_budget; return true;
    }
    struct Mark {
        std::size_t position;
        std::size_t outSize;
        std::size_t stops;
    };

    [[nodiscard]] Mark mark() const noexcept { return Mark{m_i, m_out.size(), m_stops.size()}; }
    void restore(const Mark& at) {
        m_i = at.position;
        m_out.resize(at.outSize);
        m_stops.resize(at.stops);
    }
    [[nodiscard]] std::uint32_t here() const noexcept { return static_cast<std::uint32_t>(m_out.size()); }
    [[nodiscard]] bool at(char c) const noexcept { return m_i < m_s.size() && m_s[m_i] == c; }

    // Text and constructs up to the end, or (nested) up to an unescaped '}' left unconsumed.
    void parseAny(bool nested) {
        if (m_depth > 64) { m_exhausted = true; return; }
        ++m_depth;
        struct Depth { std::size_t& value; ~Depth() { --value; } } depth{m_depth};
        while (m_i < m_s.size() && !m_exhausted && work()) {
            const char c = m_s[m_i];
            if (c == '\\' && m_i + 1 < m_s.size() &&
                (m_s[m_i + 1] == '$' || m_s[m_i + 1] == '}' || m_s[m_i + 1] == '\\')) {
                m_out += m_s[m_i + 1];
                m_i += 2;
                continue;
            }
            if (nested && c == '}') {
                return;
            }
            if (c == '$' && parseDollar()) {
                continue;
            }
            m_out += c;
            ++m_i;
        }
    }

    [[nodiscard]] std::uint32_t parseNumber() {
        std::uint32_t value = 0;
        while (m_i < m_s.size() && isDigit(m_s[m_i]) && work()) {
            value = std::min<std::uint32_t>(value * 10 + static_cast<std::uint32_t>(m_s[m_i] - '0'), 1'000'000);
            ++m_i;
        }
        return value;
    }

    void skipVariableName() {
        while (m_i < m_s.size() && isVariableChar(m_s[m_i]) && work()) {
            ++m_i;
        }
    }

    // At '$': consumes one construct and returns true, or consumes nothing and returns false.
    bool parseDollar() {
        const Mark start = mark();
        ++m_i; // '$'
        if (m_i >= m_s.size()) {
            restore(start);
            return false;
        }
        if (isDigit(m_s[m_i])) {
            const std::uint32_t index = parseNumber();
            m_stops.push_back(SnippetTabStop{.index = index, .begin = here(), .end = here()});
            return true;
        }
        if (isVariableStart(m_s[m_i])) {
            skipVariableName(); // Unknown variables resolve to nothing.
            return true;
        }
        if (m_s[m_i] != '{') {
            restore(start);
            return false;
        }
        ++m_i; // '{'
        if (m_i < m_s.size() && isDigit(m_s[m_i])) {
            const std::uint32_t index = parseNumber();
            if (at('}')) {
                ++m_i;
                m_stops.push_back(SnippetTabStop{.index = index, .begin = here(), .end = here()});
                return true;
            }
            if (at(':')) {
                ++m_i;
                const std::uint32_t begin = here();
                const std::size_t stopSlot = m_stops.size();
                parseAny(true);
                if (at('}')) {
                    ++m_i;
                    // The outer stop goes before the nested ones it contains.
                    m_stops.insert(m_stops.begin() + static_cast<std::ptrdiff_t>(stopSlot),
                                   SnippetTabStop{.index = index, .begin = begin, .end = here()});
                    return true;
                }
            } else if (at('|')) {
                ++m_i;
                if (parseChoice(index)) {
                    return true;
                }
            }
            restore(start);
            return false;
        }
        if (m_i < m_s.size() && isVariableStart(m_s[m_i])) {
            skipVariableName();
            if (at('}')) {
                ++m_i;
                return true;
            }
            if (at(':')) {
                ++m_i;
                parseAny(true); // The default text.
                if (at('}')) {
                    ++m_i;
                    return true;
                }
            } else if (at('/')) {
                // A transform: skip to its closing brace (the variable resolves to nothing).
                while (m_i < m_s.size() && m_s[m_i] != '}' && work()) {
                    m_i += (m_s[m_i] == '\\' && m_i + 1 < m_s.size()) ? 2 : 1;
                }
                if (at('}')) {
                    ++m_i;
                    return true;
                }
            }
        }
        restore(start);
        return false;
    }

    // After "${n|": options separated by ',' up to "|}"; inserts the first.
    bool parseChoice(std::uint32_t index) {
        const std::uint32_t begin = here();
        bool first = true;
        while (m_i < m_s.size() && work()) {
            const char c = m_s[m_i];
            if (c == '\\' && m_i + 1 < m_s.size()) {
                if (first) {
                    m_out += m_s[m_i + 1];
                }
                m_i += 2;
                continue;
            }
            if (c == ',') {
                first = false;
                ++m_i;
                continue;
            }
            if (c == '|' && m_i + 1 < m_s.size() && m_s[m_i + 1] == '}') {
                m_i += 2;
                m_stops.push_back(SnippetTabStop{.index = index, .begin = begin, .end = here()});
                return true;
            }
            if (first) {
                m_out += c;
            }
            ++m_i;
        }
        return false;
    }

    std::string_view m_s;
    std::size_t m_i = 0;
    std::string m_out;
    std::vector<SnippetTabStop> m_stops;
    std::size_t m_budget = 0;
    std::size_t m_depth = 0;
    bool m_exhausted = false;
};

} // namespace

std::uint32_t ParsedSnippet::caret() const noexcept {
    if (!tabStops.empty()) {
        return tabStops.front().begin; // The first numbered stop, or $0 when it is the only kind.
    }
    return static_cast<std::uint32_t>(text.size());
}

ParsedSnippet parseSnippet(std::string_view snippet) { return Parser(snippet).run(); }

} // namespace frost::lang
