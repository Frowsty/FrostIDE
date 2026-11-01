// SPDX-License-Identifier: Apache-2.0
//
// IgnoreRules and IgnoreStack (frost/io/ignore_rules.hpp; ADR 0020): line parsing, the pattern compiler
// and the matcher. Written from the rules gitignore(5) describes, without consulting git's sources.
//
/*
 * Matcher. A pattern compiles to tokens: literal bytes, '?', bracket sets (256-bit sets with '/'
 * removed and, for caseInsensitive, closed under ASCII case before a negation is applied), '*' (any
 * run without '/'), the directory globstar (zero or more "name/" segments) and the final globstar
 * (anything). Matching simulates the token NFA over the path's bytes: state 2i means "at token i",
 * state 2i+1 "inside a segment of the directory globstar at token i", state 2n accepts. The optional
 * tokens ('*' and both globstars) have an epsilon edge to the next token; because those edges only go
 * forward, one ascending pass computes a closure. That gives O(tokens x bytes) without backtracking,
 * whatever the input (the fuzz target relies on it). Literal patterns, a lone final globstar and
 * "*" + literal patterns skip the simulation; every other pattern first checks its literal tail.
 */

#include "frost/io/ignore_rules.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::io {
namespace ignore_detail {

[[nodiscard]] constexpr unsigned char foldAscii(unsigned char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c - 'A' + 'a') : c;
}

class ByteSet {
public:
    void set(unsigned char c) noexcept { m_bits[c >> 6] |= std::uint64_t{1} << (c & 63); }
    void clear(unsigned char c) noexcept { m_bits[c >> 6] &= ~(std::uint64_t{1} << (c & 63)); }
    [[nodiscard]] bool test(unsigned char c) const noexcept { return ((m_bits[c >> 6] >> (c & 63)) & 1) != 0; }
    void invert() noexcept {
        for (std::uint64_t& word : m_bits) {
            word = ~word;
        }
    }

private:
    std::array<std::uint64_t, 4> m_bits{};
};

enum class TokenKind : unsigned char { Literal, AnyChar, Class, Star, DirStar, AllStar };

struct Token {
    TokenKind kind = TokenKind::Literal;
    unsigned char byte = 0;     // Literal (folded when case-insensitive).
    std::uint32_t setIndex = 0; // Class.
};

[[nodiscard]] constexpr bool optional(TokenKind kind) noexcept {
    return kind == TokenKind::Star || kind == TokenKind::DirStar || kind == TokenKind::AllStar;
}

struct Pattern {
    enum class Fast : unsigned char { None, Literal, Suffix, Everything };
    Fast fast = Fast::None;
    std::string literal; // Literal: the whole pattern; Suffix: what follows the leading '*'.
    std::string tail;    // The literal bytes after the last non-literal token (a quick reject).
    std::vector<Token> tokens;
    std::vector<ByteSet> sets;
};

// The POSIX character classes bracket expressions may name.
[[nodiscard]] bool addNamedClass(std::string_view name, ByteSet& set) {
    const auto add = [&](auto predicate) {
        for (unsigned value = 0; value < 128; ++value) {
            if (predicate(static_cast<unsigned char>(value))) {
                set.set(static_cast<unsigned char>(value));
            }
        }
        return true;
    };
    const auto upper = [](unsigned char c) { return c >= 'A' && c <= 'Z'; };
    const auto lower = [](unsigned char c) { return c >= 'a' && c <= 'z'; };
    const auto digit = [](unsigned char c) { return c >= '0' && c <= '9'; };
    const auto alpha = [&](unsigned char c) { return upper(c) || lower(c); };
    const auto space = [](unsigned char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
    const auto print = [](unsigned char c) { return c >= 0x20 && c < 0x7f; };
    if (name == "alnum")
        return add([&](unsigned char c) { return alpha(c) || digit(c); });
    if (name == "alpha")
        return add(alpha);
    if (name == "blank")
        return add([](unsigned char c) { return c == ' ' || c == '\t'; });
    if (name == "cntrl")
        return add([](unsigned char c) { return c < 0x20 || c == 0x7f; });
    if (name == "digit")
        return add(digit);
    if (name == "graph")
        return add([&](unsigned char c) { return print(c) && c != ' '; });
    if (name == "lower")
        return add(lower);
    if (name == "print")
        return add(print);
    if (name == "punct")
        return add([&](unsigned char c) { return print(c) && c != ' ' && !alpha(c) && !digit(c); });
    if (name == "space")
        return add(space);
    if (name == "upper")
        return add(upper);
    if (name == "xdigit")
        return add([&](unsigned char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); });
    return false;
}

/*
 * Parses the bracket expression starting at body[at] == '['. On success returns the index after its
 * closing ']' and fills `set` (negation, folding and the '/' exclusion applied); on failure returns
 * nullopt with `error` set.
 */
[[nodiscard]] std::optional<std::size_t> parseBracket(std::string_view body, std::size_t at, bool fold, ByteSet& set,
                                                      std::string& error) {
    std::size_t k = at + 1;
    bool negate = false;
    if (k < body.size() && (body[k] == '!' || body[k] == '^')) {
        negate = true;
        ++k;
    }
    ByteSet members;
    bool first = true;
    // Reads one member byte at k (a backslash escapes); nullopt at the end of the pattern.
    const auto member = [&](std::size_t& pos) -> std::optional<unsigned char> {
        if (pos >= body.size()) {
            return std::nullopt;
        }
        if (body[pos] == '\\') {
            if (pos + 1 >= body.size()) {
                return std::nullopt;
            }
            pos += 2;
            return static_cast<unsigned char>(body[pos - 1]);
        }
        ++pos;
        return static_cast<unsigned char>(body[pos - 1]);
    };
    for (;;) {
        if (k >= body.size()) {
            error = "an unterminated bracket expression";
            return std::nullopt;
        }
        if (body[k] == ']' && !first) {
            ++k;
            break;
        }
        first = false;
        if (body[k] == '[' && k + 1 < body.size() && body[k + 1] == ':') {
            const std::size_t close = body.find(":]", k + 2);
            const std::size_t bracket = body.find(']', k + 2);
            if (close != std::string_view::npos && close + 1 == bracket) {
                const std::string_view name = body.substr(k + 2, close - (k + 2));
                if (!addNamedClass(name, members)) {
                    error = std::format("an unknown character class '[:{}:]'", name);
                    return std::nullopt;
                }
                k = close + 2;
                continue;
            }
            // No ":]" before the next ']': the '[' is an ordinary member.
        }
        const auto low = member(k);
        if (!low) {
            error = "an unterminated bracket expression";
            return std::nullopt;
        }
        if (k + 1 < body.size() && body[k] == '-' && body[k + 1] != ']') {
            std::size_t hiPos = k + 1;
            const auto high = member(hiPos);
            if (!high) {
                error = "an unterminated bracket expression";
                return std::nullopt;
            }
            k = hiPos;
            for (unsigned value = *low; value <= *high; ++value) {
                members.set(static_cast<unsigned char>(value));
            }
        } else {
            members.set(*low);
        }
    }
    if (fold) {
        for (unsigned value = 'A'; value <= 'Z'; ++value) {
            const auto up = static_cast<unsigned char>(value);
            const auto down = foldAscii(up);
            if (members.test(up) || members.test(down)) {
                members.set(up);
                members.set(down);
            }
        }
    }
    if (negate) {
        members.invert();
    }
    members.clear('/');
    set = members;
    return k;
}

// Compiles a pattern body (flags and the anchoring slash removed). False with `error` if it can never match.
[[nodiscard]] bool compile(std::string_view body, bool fold, Pattern& out, std::string& error) {
    std::size_t i = 0;
    while (i < body.size()) {
        const char c = body[i];
        if (c == '\\') {
            if (i + 1 == body.size()) {
                error = "a trailing backslash";
                return false;
            }
            const auto byte = static_cast<unsigned char>(body[i + 1]);
            out.tokens.push_back(Token{TokenKind::Literal, fold ? foldAscii(byte) : byte, 0});
            i += 2;
        } else if (c == '?') {
            out.tokens.push_back(Token{TokenKind::AnyChar, 0, 0});
            ++i;
        } else if (c == '[') {
            ByteSet set;
            const auto next = parseBracket(body, i, fold, set, error);
            if (!next) {
                return false;
            }
            out.tokens.push_back(Token{TokenKind::Class, 0, static_cast<std::uint32_t>(out.sets.size())});
            out.sets.push_back(set);
            i = *next;
        } else if (c == '*') {
            std::size_t j = i;
            while (j < body.size() && body[j] == '*') {
                ++j;
            }
            // At the start, after a '/', or right after a directory globstar (which ended with one).
            const bool afterSlash =
                out.tokens.empty() || out.tokens.back().kind == TokenKind::DirStar ||
                (out.tokens.back().kind == TokenKind::Literal && out.tokens.back().byte == '/' && body[i - 1] == '/');
            const bool beforeSlash = j == body.size() || body[j] == '/';
            if (j - i >= 2 && afterSlash && beforeSlash) {
                if (j == body.size()) {
                    out.tokens.push_back(Token{TokenKind::AllStar, 0, 0});
                    i = j;
                } else {
                    out.tokens.push_back(Token{TokenKind::DirStar, 0, 0});
                    i = j + 1; // The '/' belongs to the globstar.
                }
            } else {
                // Consecutive single stars are one star (the NFA would accept the same paths).
                if (out.tokens.empty() || out.tokens.back().kind != TokenKind::Star) {
                    out.tokens.push_back(Token{TokenKind::Star, 0, 0});
                }
                i = j;
            }
        } else {
            const auto byte = static_cast<unsigned char>(c);
            out.tokens.push_back(Token{TokenKind::Literal, fold ? foldAscii(byte) : byte, 0});
            ++i;
        }
    }
    // The fast paths and the literal tail.
    const auto isLiteral = [](const Token& token) { return token.kind == TokenKind::Literal; };
    if (std::ranges::all_of(out.tokens, isLiteral)) {
        out.fast = Pattern::Fast::Literal;
        for (const Token& token : out.tokens) {
            out.literal.push_back(static_cast<char>(token.byte));
        }
    } else if (out.tokens.size() == 1 && out.tokens[0].kind == TokenKind::AllStar) {
        out.fast = Pattern::Fast::Everything;
    } else if (out.tokens[0].kind == TokenKind::Star &&
               std::all_of(out.tokens.begin() + 1, out.tokens.end(), isLiteral)) {
        out.fast = Pattern::Fast::Suffix;
        for (std::size_t t = 1; t < out.tokens.size(); ++t) {
            out.literal.push_back(static_cast<char>(out.tokens[t].byte));
        }
    } else {
        std::size_t start = out.tokens.size();
        while (start > 0 && isLiteral(out.tokens[start - 1])) {
            --start;
        }
        for (std::size_t t = start; t < out.tokens.size(); ++t) {
            out.tail.push_back(static_cast<char>(out.tokens[t].byte));
        }
    }
    return true;
}

[[nodiscard]] bool equalBytes(std::string_view text, std::string_view folded, bool fold) noexcept {
    if (text.size() != folded.size()) {
        return false;
    }
    if (!fold) {
        return text == folded;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (foldAscii(static_cast<unsigned char>(text[i])) != static_cast<unsigned char>(folded[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool endsWith(std::string_view text, std::string_view folded, bool fold) noexcept {
    return text.size() >= folded.size() && equalBytes(text.substr(text.size() - folded.size()), folded, fold);
}

// A bit set over NFA states; on the stack for patterns of up to 127 tokens.
class StateSet {
public:
    explicit StateSet(std::size_t states) : m_words((states + 63) / 64) {
        if (m_words > m_inline.size()) {
            m_heap.resize(m_words);
        }
    }
    [[nodiscard]] std::uint64_t* data() noexcept { return m_heap.empty() ? m_inline.data() : m_heap.data(); }
    [[nodiscard]] const std::uint64_t* data() const noexcept {
        return m_heap.empty() ? m_inline.data() : m_heap.data();
    }
    [[nodiscard]] std::size_t words() const noexcept { return m_words; }
    void set(std::size_t state) noexcept { data()[state >> 6] |= std::uint64_t{1} << (state & 63); }
    [[nodiscard]] bool test(std::size_t state) const noexcept {
        return ((data()[state >> 6] >> (state & 63)) & 1) != 0;
    }
    void clear() noexcept { std::fill_n(data(), m_words, std::uint64_t{0}); }
    [[nodiscard]] bool none() const noexcept {
        return std::all_of(data(), data() + m_words, [](std::uint64_t w) { return w == 0; });
    }

private:
    std::size_t m_words;
    std::array<std::uint64_t, 4> m_inline{};
    std::vector<std::uint64_t> m_heap;
};

[[nodiscard]] bool simulate(const Pattern& pattern, std::string_view text, bool fold) {
    const std::vector<Token>& tokens = pattern.tokens;
    const std::size_t n = tokens.size();
    const std::size_t accept = 2 * n;
    StateSet current(accept + 1);
    StateSet next(accept + 1);
    const auto close = [&](StateSet& states) {
        for (std::size_t i = 0; i < n; ++i) {
            if (optional(tokens[i].kind) && states.test(2 * i)) {
                states.set(2 * (i + 1));
            }
        }
    };
    current.set(0);
    close(current);
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        const unsigned char folded = fold ? foldAscii(c) : c;
        next.clear();
        const std::uint64_t* words = current.data();
        for (std::size_t w = 0; w < current.words(); ++w) {
            std::uint64_t bits = words[w];
            while (bits != 0) {
                const std::size_t state = w * 64 + static_cast<std::size_t>(std::countr_zero(bits));
                bits &= bits - 1;
                if (state == accept) {
                    continue;
                }
                const std::size_t i = state / 2;
                const Token& token = tokens[i];
                if ((state & 1) != 0) {
                    next.set(c == '/' ? 2 * i : 2 * i + 1); // Inside a directory-globstar segment.
                    continue;
                }
                switch (token.kind) {
                case TokenKind::Literal:
                    if (folded == token.byte) {
                        next.set(2 * (i + 1));
                    }
                    break;
                case TokenKind::AnyChar:
                    if (c != '/') {
                        next.set(2 * (i + 1));
                    }
                    break;
                case TokenKind::Class:
                    if (pattern.sets[token.setIndex].test(c)) {
                        next.set(2 * (i + 1));
                    }
                    break;
                case TokenKind::Star:
                    if (c != '/') {
                        next.set(2 * i);
                    }
                    break;
                case TokenKind::DirStar: next.set(c == '/' ? 2 * i : 2 * i + 1); break;
                case TokenKind::AllStar: next.set(2 * i); break;
                }
            }
        }
        close(next);
        if (next.none()) {
            return false;
        }
        std::swap(current, next);
    }
    return current.test(accept);
}

[[nodiscard]] bool matches(const Pattern& pattern, std::string_view text, bool fold) {
    switch (pattern.fast) {
    case Pattern::Fast::Literal: return equalBytes(text, pattern.literal, fold);
    case Pattern::Fast::Everything: return true;
    case Pattern::Fast::Suffix:
        return endsWith(text, pattern.literal, fold) &&
               text.substr(0, text.size() - pattern.literal.size()).find('/') == std::string_view::npos;
    case Pattern::Fast::None: break;
    }
    if (!endsWith(text, pattern.tail, fold)) {
        return false;
    }
    return simulate(pattern, text, fold);
}

// The line without its unescaped trailing spaces.
[[nodiscard]] std::string_view trimTrailingSpaces(std::string_view line) noexcept {
    std::size_t end = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            ++i; // The escaped byte is content, a space included.
            end = i + 1;
        } else if (line[i] != ' ') {
            end = i + 1;
        }
    }
    return line.substr(0, end);
}

} // namespace ignore_detail

struct IgnoreRules::Compiled {
    std::vector<ignore_detail::Pattern> patterns; // Parallel to IgnoreRules::m_rules.
};

IgnoreRules IgnoreRules::parse(std::string_view text, std::string base, std::string source, IgnoreOptions options) {
    IgnoreRules result;
    result.m_base = std::move(base);
    result.m_source = std::move(source);
    result.m_options = options;
    auto compiled = std::make_shared<Compiled>();
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    std::uint32_t lineNumber = 0;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        ++lineNumber;
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        line = ignore_detail::trimTrailingSpaces(line);
        if (line.empty()) {
            continue;
        }
        IgnoreRule rule;
        rule.text = std::string(line);
        rule.line = lineNumber;
        std::string_view body = line;
        if (body.front() == '!') {
            rule.negated = true;
            body.remove_prefix(1);
        }
        if (body.ends_with('/')) {
            rule.directoryOnly = true;
            body.remove_suffix(1);
        }
        if (body.find('/') != std::string_view::npos) {
            rule.anchored = true;
            if (body.front() == '/') {
                body.remove_prefix(1);
            }
        }
        ignore_detail::Pattern pattern;
        std::string error;
        if (body.empty()) {
            error = "nothing to match";
        } else if (!ignore_detail::compile(body, options.caseInsensitive, pattern, error)) {
            pattern = {};
        }
        if (!error.empty()) {
            rule.valid = false;
            result.m_diagnostics.push_back(
                IgnoreDiagnostic{lineNumber, std::format("'{}' never matches: {}", rule.text, error)});
        }
        result.m_rules.push_back(std::move(rule));
        compiled->patterns.push_back(std::move(pattern));
    }
    result.m_compiled = std::move(compiled);
    return result;
}

const IgnoreRule* IgnoreRules::lastMatch(std::string_view path, bool isDirectory) const {
    if (m_rules.empty()) {
        return nullptr;
    }
    std::string_view relative = path;
    if (!m_base.empty()) {
        if (path.size() <= m_base.size() + 1 || !path.starts_with(m_base) || path[m_base.size()] != '/') {
            return nullptr; // Not below this file's directory.
        }
        relative.remove_prefix(m_base.size() + 1);
    }
    if (relative.empty()) {
        return nullptr;
    }
    const std::size_t slash = relative.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? relative : relative.substr(slash + 1);
    const bool fold = m_options.caseInsensitive;
    for (std::size_t i = m_rules.size(); i-- > 0;) {
        const IgnoreRule& rule = m_rules[i];
        if (!rule.valid || (rule.directoryOnly && !isDirectory)) {
            continue;
        }
        if (ignore_detail::matches(m_compiled->patterns[i], rule.anchored ? relative : name, fold)) {
            return &rule;
        }
    }
    return nullptr;
}

struct IgnoreStack::Frame {
    IgnoreRules rules;
    std::shared_ptr<const Frame> parent;
};

IgnoreStack IgnoreStack::pushed(IgnoreRules rules) const {
    if (rules.empty()) {
        return *this;
    }
    IgnoreStack result;
    result.m_top = std::make_shared<const Frame>(Frame{std::move(rules), m_top});
    return result;
}

IgnoreMatch IgnoreStack::match(std::string_view path, bool isDirectory) const {
    for (const Frame* frame = m_top.get(); frame != nullptr; frame = frame->parent.get()) {
        if (const IgnoreRule* rule = frame->rules.lastMatch(path, isDirectory)) {
            return IgnoreMatch{!rule->negated, rule, &frame->rules};
        }
    }
    return {};
}

IgnoreMatch IgnoreStack::matchWithParents(std::string_view path, bool isDirectory) const {
    for (std::size_t slash = path.find('/'); slash != std::string_view::npos; slash = path.find('/', slash + 1)) {
        if (IgnoreMatch parent = match(path.substr(0, slash), true); parent.ignored) {
            return parent;
        }
    }
    return match(path, isDirectory);
}

} // namespace frost::io
