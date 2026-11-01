// SPDX-License-Identifier: Apache-2.0
//
/*
 * Fuzz harness for the gitignore matcher (frost/io/ignore_rules.hpp; ADR 0020): arbitrary ignore files
 * and paths must parse and match without crashing or slowing down (the matcher is an NFA simulation,
 * linear in the path for a given pattern), and on short inputs every line's verdict must equal a
 * separate backtracking matcher written in this file from the same rules.
 *
 * Input format:
 *   byte 0      bit 0: case-insensitive matching; bits 1-7: ignored;
 *   then        the ignore file, up to the first NUL byte (or the end);
 *   then        NUL-separated paths, at most 64; a path ending in '/' is a directory (the '/' is
 *               removed). Paths that are empty, start with '/' or contain "//" are only matched,
 *               not compared.
 * Every line of the file is also parsed on its own, so each verdict is one pattern's. The
 * comparison runs for patterns of at most 40 bytes and paths of at most 64 bytes (the reference
 * is memoised, so these bounds only keep each input fast).
 */

#include "frost/io/ignore_rules.hpp"

#include "fuzz/fuzz_harness.hpp"
#include "support/crash_dialogs.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::fuzz {
namespace {

using io::IgnoreOptions;
using io::IgnoreRules;

[[nodiscard]] unsigned char lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + 32) : c;
}

// The reference: a memoised backtracking matcher over the raw pattern bytes.
class Reference {
public:
    Reference(std::string_view pattern, std::string_view text, bool fold)
        : m_p(pattern), m_t(text), m_fold(fold), m_memo((pattern.size() + 1) * (text.size() + 1), 0) {}

    [[nodiscard]] bool run() { return match(0, 0); }

    // Whether the pattern can match at all (the never-matching forms of ignore_rules.hpp).
    [[nodiscard]] static bool valid(std::string_view p) {
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (p[i] == '\\') {
                if (i + 1 == p.size()) {
                    return false;
                }
                ++i;
            } else if (p[i] == '[') {
                Bracket bracket;
                if (!parseBracket(p, i, bracket)) {
                    return false;
                }
                i = bracket.end - 1;
            }
        }
        return true;
    }

private:
    struct Bracket {
        bool negated = false;
        std::vector<std::pair<unsigned char, unsigned char>> ranges;
        std::vector<std::string> classes;
        std::size_t end = 0; // Index after the closing ']'.
    };

    static bool inClass(std::string_view name, unsigned char c) {
        const bool up = c >= 'A' && c <= 'Z';
        const bool low = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        const bool print = c >= 0x20 && c < 0x7f;
        if (name == "alnum")
            return up || low || digit;
        if (name == "alpha")
            return up || low;
        if (name == "blank")
            return c == ' ' || c == '\t';
        if (name == "cntrl")
            return c < 0x20 || c == 0x7f;
        if (name == "digit")
            return digit;
        if (name == "graph")
            return print && c != ' ';
        if (name == "lower")
            return low;
        if (name == "print")
            return print;
        if (name == "punct")
            return print && c != ' ' && !up && !low && !digit;
        if (name == "space")
            return c == ' ' || (c >= '\t' && c <= '\r');
        if (name == "upper")
            return up;
        if (name == "xdigit")
            return digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        return false;
    }

    static bool knownClass(std::string_view name) {
        for (const std::string_view known : {"alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print",
                                             "punct", "space", "upper", "xdigit"}) {
            if (name == known) {
                return true;
            }
        }
        return false;
    }

    static bool parseBracket(std::string_view p, std::size_t at, Bracket& out) {
        std::size_t k = at + 1;
        if (k < p.size() && (p[k] == '!' || p[k] == '^')) {
            out.negated = true;
            ++k;
        }
        const auto member = [&](std::size_t& pos) -> std::optional<unsigned char> {
            if (pos >= p.size() || (p[pos] == '\\' && pos + 1 >= p.size())) {
                return std::nullopt;
            }
            if (p[pos] == '\\') {
                pos += 2;
            } else {
                pos += 1;
            }
            return static_cast<unsigned char>(p[pos - 1]);
        };
        for (bool first = true;; first = false) {
            if (k >= p.size()) {
                return false;
            }
            if (p[k] == ']' && !first) {
                out.end = k + 1;
                return true;
            }
            if (p[k] == '[' && k + 1 < p.size() && p[k + 1] == ':') {
                const std::size_t close = p.find(":]", k + 2);
                if (close != std::string_view::npos && close + 1 == p.find(']', k + 2)) {
                    const std::string name(p.substr(k + 2, close - k - 2));
                    if (!knownClass(name)) {
                        return false;
                    }
                    out.classes.push_back(name);
                    k = close + 2;
                    continue;
                }
            }
            const auto low = member(k);
            if (!low) {
                return false;
            }
            if (k + 1 < p.size() && p[k] == '-' && p[k + 1] != ']') {
                std::size_t at2 = k + 1;
                const auto high = member(at2);
                if (!high) {
                    return false;
                }
                k = at2;
                out.ranges.emplace_back(*low, *high);
            } else {
                out.ranges.emplace_back(*low, *low);
            }
        }
    }

    [[nodiscard]] bool bracketHas(const Bracket& bracket, unsigned char c) const {
        const auto has = [&](unsigned char x) {
            for (const auto& [lo, hi] : bracket.ranges) {
                if (x >= lo && x <= hi) {
                    return true;
                }
            }
            for (const std::string& name : bracket.classes) {
                if (inClass(name, x)) {
                    return true;
                }
            }
            return false;
        };
        bool in = has(c);
        if (m_fold && !in) {
            const unsigned char other = c >= 'a' && c <= 'z' ? static_cast<unsigned char>(c - 32) : lower(c);
            in = has(other);
        }
        return bracket.negated ? !in : in;
    }

    [[nodiscard]] bool same(unsigned char a, unsigned char b) const { return m_fold ? lower(a) == lower(b) : a == b; }

    bool match(std::size_t pi, std::size_t ti) {
        char& memo = m_memo[pi * (m_t.size() + 1) + ti];
        if (memo != 0) {
            return memo == 1;
        }
        const bool result = compute(pi, ti);
        memo = result ? 1 : 2;
        return result;
    }

    bool compute(std::size_t pi, std::size_t ti) {
        if (pi == m_p.size()) {
            return ti == m_t.size();
        }
        const char c = m_p[pi];
        if (c == '*') {
            std::size_t j = pi;
            while (j < m_p.size() && m_p[j] == '*') {
                ++j;
            }
            const bool start = pi == 0 || m_p[pi - 1] == '/';
            const bool end = j == m_p.size() || m_p[j] == '/';
            if (j - pi >= 2 && start && end) {
                if (j == m_p.size()) {
                    return true;
                }
                if (match(j + 1, ti)) {
                    return true;
                }
                for (std::size_t k = ti; k < m_t.size(); ++k) {
                    if (m_t[k] == '/' && match(j + 1, k + 1)) {
                        return true;
                    }
                }
                return false;
            }
            for (std::size_t k = ti;; ++k) {
                if (match(j, k)) {
                    return true;
                }
                if (k == m_t.size() || m_t[k] == '/') {
                    return false;
                }
            }
        }
        if (ti == m_t.size()) {
            return false;
        }
        const auto t = static_cast<unsigned char>(m_t[ti]);
        if (c == '?') {
            return t != '/' && match(pi + 1, ti + 1);
        }
        if (c == '[') {
            Bracket bracket;
            if (!parseBracket(m_p, pi, bracket)) {
                return false;
            }
            return t != '/' && bracketHas(bracket, t) && match(bracket.end, ti + 1);
        }
        if (c == '\\') {
            return pi + 1 < m_p.size() && same(static_cast<unsigned char>(m_p[pi + 1]), t) && match(pi + 2, ti + 1);
        }
        return same(static_cast<unsigned char>(c), t) && match(pi + 1, ti + 1);
    }

    std::string_view m_p;
    std::string_view m_t;
    bool m_fold;
    std::vector<char> m_memo; // 0 unknown, 1 match, 2 no match.
};

// One line as the reference reads it; nullopt for lines that hold no pattern.
struct RefRule {
    bool negated = false;
    bool directoryOnly = false;
    bool anchored = false;
    std::string body;
};

std::optional<RefRule> refRule(std::string_view line) {
    if (line.ends_with('\r')) {
        line.remove_suffix(1);
    }
    if (line.empty() || line.front() == '#') {
        return std::nullopt;
    }
    // Trailing spaces go unless escaped.
    std::size_t end = line.size();
    while (end > 0 && line[end - 1] == ' ') {
        std::size_t backslashes = 0;
        while (backslashes < end - 1 && line[end - 2 - backslashes] == '\\') {
            ++backslashes;
        }
        if (backslashes % 2 == 1) {
            break; // Escaped.
        }
        --end;
    }
    line = line.substr(0, end);
    if (line.empty()) {
        return std::nullopt;
    }
    RefRule rule;
    if (line.front() == '!') {
        rule.negated = true;
        line.remove_prefix(1);
    }
    if (line.ends_with('/')) {
        rule.directoryOnly = true;
        line.remove_suffix(1);
    }
    if (line.find('/') != std::string_view::npos) {
        rule.anchored = true;
        if (line.front() == '/') {
            line.remove_prefix(1);
        }
    }
    rule.body = std::string(line);
    return rule;
}

[[nodiscard]] bool wellFormed(std::string_view path) {
    return !path.empty() && path.front() != '/' && path.back() != '/' && path.find("//") == std::string_view::npos;
}

} // namespace

void initialize() { frost::test::disableCrashDialogs(); }

Result<void> runInput(std::span<const std::uint8_t> data) {
    if (data.empty()) {
        return {};
    }
    const bool fold = (data[0] & 1) != 0;
    const std::string_view input(reinterpret_cast<const char*>(data.data()) + 1, data.size() - 1);
    const std::size_t nul = input.find('\0');
    const std::string_view text = input.substr(0, nul);
    std::vector<std::pair<std::string_view, bool>> paths;
    if (nul != std::string_view::npos) {
        std::string_view rest = input.substr(nul + 1);
        while (!rest.empty() && paths.size() < 64) {
            const std::size_t next = rest.find('\0');
            std::string_view path = rest.substr(0, next);
            rest.remove_prefix(next == std::string_view::npos ? rest.size() : next + 1);
            const bool isDirectory = path.ends_with('/');
            if (isDirectory) {
                path.remove_suffix(1);
            }
            paths.emplace_back(path, isDirectory);
        }
    }
    const IgnoreOptions options{.caseInsensitive = fold};
    // The whole file, through a stack with a subdirectory copy too.
    const IgnoreRules whole = IgnoreRules::parse(text, {}, "fuzz", options);
    const io::IgnoreStack stack =
        io::IgnoreStack().pushed(whole).pushed(IgnoreRules::parse(text, "sub", "fuzz/sub", options));
    for (const auto& [path, isDirectory] : paths) {
        static_cast<void>(whole.lastMatch(path, isDirectory));
        static_cast<void>(stack.matchWithParents(path, isDirectory));
    }
    // Each line on its own, against the reference.
    std::string_view rest = text;
    if (rest.starts_with("\xEF\xBB\xBF")) {
        rest.remove_prefix(3);
    }
    std::size_t lineNumber = 0;
    while (!rest.empty()) {
        const std::size_t newline = rest.find('\n');
        const std::string_view line = rest.substr(0, newline);
        rest.remove_prefix(newline == std::string_view::npos ? rest.size() : newline + 1);
        ++lineNumber;
        if (line.starts_with("\xEF\xBB\xBF")) {
            continue; // A mark inside the file is pattern bytes, but parsed on its own it would be dropped.
        }
        const auto ref = refRule(line);
        const IgnoreRules single = IgnoreRules::parse(line, {}, {}, options);
        if (!ref || ref->body.empty()) {
            const bool none = single.rules().empty() || !single.rules()[0].valid;
            if (!none) {
                return fail(ErrorCode::Internal,
                            std::format("line {} '{}': a pattern where the reference has none", lineNumber, line));
            }
            continue;
        }
        if (single.rules().size() != 1) {
            return fail(ErrorCode::Internal,
                        std::format("line {}: {} rules from one line", lineNumber, single.rules().size()));
        }
        const io::IgnoreRule& rule = single.rules()[0];
        const bool valid = Reference::valid(ref->body);
        if (rule.valid != valid || rule.negated != ref->negated || rule.directoryOnly != ref->directoryOnly ||
            rule.anchored != ref->anchored) {
            return fail(ErrorCode::Internal,
                        std::format("line {} '{}': flags differ from the reference", lineNumber, line));
        }
        if (ref->body.size() > 40) {
            continue;
        }
        for (const auto& [path, isDirectory] : paths) {
            if (!wellFormed(path) || path.size() > 64) {
                continue;
            }
            const std::size_t slash = path.rfind('/');
            const std::string_view subject =
                ref->anchored || slash == std::string_view::npos ? path : path.substr(slash + 1);
            const bool expected =
                valid && (!ref->directoryOnly || isDirectory) && Reference(ref->body, subject, fold).run();
            const bool actual = single.lastMatch(path, isDirectory) != nullptr;
            if (expected != actual) {
                return fail(ErrorCode::Internal,
                            std::format("line {} '{}' on '{}'{}: the matcher says {}, the reference {}", lineNumber,
                                        line, path, isDirectory ? " (directory)" : "", actual, expected));
            }
        }
    }
    return {};
}

} // namespace frost::fuzz
