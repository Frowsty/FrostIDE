// SPDX-License-Identifier: Apache-2.0
//
// The fuzzy path matcher (frost/search/fuzzy_path.hpp): a dynamic programme over (pattern
// character, text position) that keeps, for each position, the best score of a match ending there.

#include "frost/search/fuzzy_path.hpp"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cstdint>
#include <system_error>

namespace frost::search {
namespace {

constexpr int kNone = INT_MIN / 4;
constexpr int kMatch = 1;
constexpr int kStartBonus = 10;
constexpr int kWordBonus = 8;
constexpr int kConsecutiveBonus = 6;
constexpr int kExactBonus = 50;
constexpr int kPrefixBonus = 20;

[[nodiscard]] constexpr char fold(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}
[[nodiscard]] constexpr bool isUpper(char c) noexcept { return c >= 'A' && c <= 'Z'; }
[[nodiscard]] constexpr bool isLower(char c) noexcept { return c >= 'a' && c <= 'z'; }
[[nodiscard]] constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
[[nodiscard]] constexpr bool isAlpha(char c) noexcept { return isUpper(c) || isLower(c); }
[[nodiscard]] constexpr bool isSeparator(char c) noexcept {
    return c == '.' || c == '_' || c == '-' || c == '/' || c == '\\' || c == ' ';
}

// Whether text[j] starts a word (see the header comment).
[[nodiscard]] bool wordStart(std::string_view text, std::size_t j) noexcept {
    if (j == 0) {
        return true;
    }
    const char before = text[j - 1];
    const char here = text[j];
    return isSeparator(before) || (isLower(before) && isUpper(here)) || (isAlpha(before) && isDigit(here)) ||
           (isDigit(before) && isAlpha(here));
}

[[nodiscard]] std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), fold);
    return out;
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// A positive decimal number of at most 9 digits, or nullopt.
[[nodiscard]] std::optional<int> positiveNumber(std::string_view digits) noexcept {
    if (digits.empty() || digits.size() > 9 || !std::ranges::all_of(digits, isDigit)) {
        return std::nullopt;
    }
    int value = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc{} || value < 1) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string_view fileNameOf(std::string_view path) noexcept {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace

LocationQuery parseLocationQuery(std::string_view text) {
    text = trim(text);
    LocationQuery query;
    // "name(line)": the compiler-message form.
    if (text.ends_with(')')) {
        if (const std::size_t open = text.rfind('('); open != std::string_view::npos) {
            if (const auto line = positiveNumber(text.substr(open + 1, text.size() - open - 2)); line) {
                query.pattern = std::string(trim(text.substr(0, open)));
                query.line = line;
                return query;
            }
        }
    }
    // "name:line" or "name:line:column".
    if (const std::size_t last = text.rfind(':'); last != std::string_view::npos) {
        if (const auto number = positiveNumber(text.substr(last + 1)); number) {
            const std::string_view head = text.substr(0, last);
            if (const std::size_t previous = head.rfind(':'); previous != std::string_view::npos) {
                if (const auto line = positiveNumber(head.substr(previous + 1)); line) {
                    query.pattern = std::string(trim(head.substr(0, previous)));
                    query.line = line;
                    query.column = number;
                    return query;
                }
            }
            query.pattern = std::string(trim(head));
            query.line = number;
            return query;
        }
        if (last + 1 == text.size()) {
            // A trailing ':' while the user is still typing the line.
            query.pattern = std::string(trim(text.substr(0, last)));
            return query;
        }
    }
    query.pattern = std::string(text);
    return query;
}

std::optional<int> scoreWord(std::string_view pattern, std::string_view text) {
    const std::size_t n = pattern.size();
    const std::size_t m = text.size();
    if (n == 0 || n > m) {
        return std::nullopt;
    }
    std::vector<int> previous(m, kNone);
    std::vector<int> current(m, kNone);
    for (std::size_t i = 0; i < n; ++i) {
        // Best of previous[k] + k over k <= j - 2: a match at j after a gap of j - k - 1 characters
        // costs one per skipped character, previous[k] - (j - k - 1) = (previous[k] + k) - (j - 1).
        int running = kNone;
        bool any = false;
        for (std::size_t j = 0; j < m; ++j) {
            if (i > 0 && j >= 2 && previous[j - 2] > kNone) {
                running = std::max(running, previous[j - 2] + static_cast<int>(j - 2));
            }
            if (fold(text[j]) != pattern[i]) {
                current[j] = kNone;
                continue;
            }
            const int bonus = kMatch + (j == 0 ? kStartBonus : (wordStart(text, j) ? kWordBonus : 0));
            int best = kNone;
            if (i == 0) {
                best = 0;
            } else {
                if (j >= 1 && previous[j - 1] > kNone) {
                    best = previous[j - 1] + kConsecutiveBonus;
                }
                if (running > kNone) {
                    best = std::max(best, running - static_cast<int>(j - 1));
                }
            }
            current[j] = best > kNone ? best + bonus : kNone;
            any = any || current[j] > kNone;
        }
        if (!any) {
            return std::nullopt;
        }
        std::swap(previous, current);
    }
    int best = *std::ranges::max_element(previous);
    if (best <= kNone) {
        return std::nullopt;
    }
    const std::string folded = lowered(text);
    if (folded == pattern) {
        best += kExactBonus;
    } else if (std::string_view(folded).starts_with(pattern)) {
        best += kPrefixBonus;
    }
    return best;
}

PathMatcher::PathMatcher(std::string_view pattern) {
    const std::string text = lowered(trim(pattern));
    if (text.empty()) {
        return;
    }
    std::string segment;
    bool separated = false;
    for (const char c : text) {
        if (c == '/' || c == '\\') {
            separated = true;
            if (!segment.empty()) {
                m_segments.push_back(std::move(segment));
                segment.clear();
            }
            continue;
        }
        if (c != ' ') {
            segment.push_back(c);
        }
    }
    // A trailing separator ("src/") leaves an empty last segment: any file name matches.
    m_segments.push_back(std::move(segment));
    if (!separated && m_segments.front().empty()) {
        m_segments.clear();
    }
}

std::optional<int> PathMatcher::score(std::string_view path) const {
    if (m_segments.empty()) {
        return 0;
    }
    const std::string_view name = fileNameOf(path);
    if (m_segments.size() == 1) {
        if (const auto inName = scoreWord(m_segments.front(), name); inName) {
            return 2 * *inName;
        }
        return scoreWord(m_segments.front(), path);
    }
    int total = 0;
    if (!m_segments.back().empty()) {
        const auto inName = scoreWord(m_segments.back(), name);
        if (!inName) {
            return std::nullopt;
        }
        total = 2 * *inName;
    }
    // The directory components, in order, before the file name.
    std::vector<std::string_view> directories;
    std::string_view rest = path.substr(0, path.size() - name.size());
    while (!rest.empty()) {
        const std::size_t slash = rest.find_first_of("/\\");
        const std::string_view component = rest.substr(0, slash);
        if (!component.empty()) {
            directories.push_back(component);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(slash + 1);
    }
    std::size_t next = 0;
    for (std::size_t s = 0; s + 1 < m_segments.size(); ++s) {
        const std::string& segment = m_segments[s];
        if (segment.empty()) {
            continue;
        }
        bool found = false;
        for (; next < directories.size(); ++next) {
            if (const auto inDirectory = scoreWord(segment, directories[next]); inDirectory) {
                total += *inDirectory;
                ++next;
                found = true;
                break;
            }
        }
        if (!found) {
            return std::nullopt;
        }
    }
    return total;
}

std::vector<RankedPath> rankPaths(std::string_view pattern, std::span<const std::string> paths, std::size_t limit) {
    const PathMatcher matcher(pattern);
    std::vector<RankedPath> ranked;
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (const auto score = matcher.score(paths[i]); score) {
            ranked.push_back(RankedPath{i, *score});
        }
    }
    if (!matcher.empty()) {
        std::ranges::stable_sort(ranked, [&](const RankedPath& a, const RankedPath& b) {
            if (a.score != b.score) {
                return a.score > b.score;
            }
            const std::string& left = paths[a.index];
            const std::string& right = paths[b.index];
            if (left.size() != right.size()) {
                return left.size() < right.size();
            }
            return left < right;
        });
    }
    if (limit != 0 && ranked.size() > limit) {
        ranked.resize(limit);
    }
    return ranked;
}

} // namespace frost::search
