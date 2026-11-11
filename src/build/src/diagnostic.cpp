// SPDX-License-Identifier: Apache-2.0
//
// The canonical-format diagnostic parser and DiagnosticSet (see frost/build/diagnostic.hpp).
// A hand-written scanner, not a regular expression: it runs on every output line of a build.

#include "frost/build/diagnostic.hpp"

#include <array>
#include <charconv>
#include <utility>

namespace frost::build {

namespace {

constexpr bool isSpace(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool isAlpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

std::string_view trimLeft(std::string_view text) noexcept {
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    return text;
}

std::string_view trimRight(std::string_view text) noexcept {
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view trim(std::string_view text) noexcept { return trimRight(trimLeft(text)); }

// ASCII case-insensitive comparison with a lower-case `word`.
bool equalsLower(std::string_view text, std::string_view word) noexcept {
    if (text.size() != word.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != word[i]) {
            return false;
        }
    }
    return true;
}

// MSBuild's multi-processor prefix, "12>" or "1:2>", which -m adds from normal verbosity up.
std::string_view skipNodePrefix(std::string_view line) noexcept {
    std::size_t i = 0;
    while (i < line.size() && isDigit(line[i])) {
        ++i;
    }
    if (i == 0) {
        return line;
    }
    if (i < line.size() && line[i] == ':') {
        std::size_t j = i + 1;
        while (j < line.size() && isDigit(line[j])) {
            ++j;
        }
        if (j == i + 1) {
            return line;
        }
        i = j;
    }
    if (i < line.size() && line[i] == '>') {
        return line.substr(i + 1);
    }
    return line;
}

// A Windows drive path ("C:\", "C:/"), a UNC or verbatim path ("\\server", "\\?\") or a POSIX path.
bool looksAbsolute(std::string_view path) noexcept {
    if (path.size() >= 3 && isAlpha(path[0]) && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
        return true;
    }
    return path.starts_with("\\\\") || path.starts_with('/');
}

// Splits MSBuild's " [<absolute project path>]" suffix off `line` and returns the path. A project
// path may itself contain " [", so earlier brackets are tried until one holds an absolute path.
std::string_view takeProject(std::string_view& line) noexcept {
    if (!line.ends_with(']')) {
        return {};
    }
    const std::size_t close = line.size() - 1;
    std::size_t open = line.rfind(" [", close);
    while (open != std::string_view::npos) {
        const std::string_view inside = line.substr(open + 2, close - open - 2);
        if (looksAbsolute(inside)) {
            line = trimRight(line.substr(0, open));
            return inside;
        }
        if (open == 0) {
            break;
        }
        open = line.rfind(" [", open - 1);
    }
    return {};
}

std::optional<Severity> categoryOf(std::string_view word) noexcept {
    if (equalsLower(word, "error")) {
        return Severity::Error;
    }
    if (equalsLower(word, "warning")) {
        return Severity::Warning;
    }
    if (equalsLower(word, "message") || equalsLower(word, "note")) {
        return Severity::Message;
    }
    return std::nullopt;
}

// A diagnostic code: letters, digits, '_', '-' or '.', with at least one digit (C2065, MSB3073).
bool isCode(std::string_view word) noexcept {
    bool digit = false;
    for (const char c : word) {
        if (isDigit(c)) {
            digit = true;
        } else if (!isAlpha(c) && c != '_' && c != '-' && c != '.') {
            return false;
        }
    }
    return digit && word.size() <= 32;
}

bool isWord(std::string_view word) noexcept {
    for (const char c : word) {
        if (!isAlpha(c)) {
            return false;
        }
    }
    return true;
}

struct Head {
    Severity severity = Severity::Error;
    std::string_view code;
};

// "[subcategory] category [code]": the part before the ':' that starts the text.
std::optional<Head> parseHead(std::string_view head) noexcept {
    constexpr std::size_t kMaxWords = 4; // Two subcategory words, the category and the code.
    std::array<std::string_view, kMaxWords> words{};
    std::size_t count = 0;
    head = trim(head);
    while (!head.empty()) {
        std::size_t end = 0;
        while (end < head.size() && !isSpace(head[end])) {
            ++end;
        }
        if (count == kMaxWords) {
            return std::nullopt;
        }
        words[count++] = head.substr(0, end);
        head = trimLeft(head.substr(end));
    }
    if (count == 0) {
        return std::nullopt;
    }
    std::size_t category = count - 1;
    std::optional<Severity> severity = categoryOf(words[category]);
    std::string_view code;
    if (!severity && count >= 2) {
        category = count - 2;
        severity = categoryOf(words[category]);
        code = words[count - 1];
        if (severity && !isCode(code)) {
            return std::nullopt;
        }
    }
    if (!severity || category > 2) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < category; ++i) {
        if (!isWord(words[i])) {
            return std::nullopt;
        }
    }
    return Head{*severity, code};
}

std::optional<int> parseNumber(std::string_view text) noexcept {
    int value = 0;
    const char* end = text.data() + text.size();
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): from_chars reads its explicit begin/end range.
    const auto [ptr, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || ptr != end || text.empty()) {
        return std::nullopt;
    }
    return value;
}

struct Location {
    int line = 0;
    int column = 0;
};

// The inside of "(line)", "(line-line2)", "(line,col)", "(line,col-col2)" or "(line,col,line2,col2)".
std::optional<Location> parseLocation(std::string_view inside) noexcept {
    std::array<std::string_view, 4> parts{};
    std::size_t count = 0;
    while (true) {
        const std::size_t comma = inside.find(',');
        if (count == parts.size()) {
            return std::nullopt;
        }
        parts[count++] = inside.substr(0, comma);
        if (comma == std::string_view::npos) {
            break;
        }
        inside.remove_prefix(comma + 1);
    }
    if (count == 3) {
        return std::nullopt;
    }
    std::array<int, 2> first{};
    for (std::size_t i = 0; i < count; ++i) {
        std::string_view part = parts[i];
        const std::size_t dash = part.find('-');
        if (dash != std::string_view::npos) {
            if (count == 4 || !parseNumber(part.substr(dash + 1))) {
                return std::nullopt; // Ranges appear only in the one- and two-part forms.
            }
            part = part.substr(0, dash);
        }
        const std::optional<int> number = parseNumber(part);
        if (!number || *number < 0) {
            return std::nullopt;
        }
        if (i < first.size()) {
            first[i] = *number;
        }
    }
    return Location{first[0], count >= 2 ? first[1] : 0};
}

} // namespace

std::string_view toString(Severity severity) noexcept {
    switch (severity) {
    case Severity::Error: return "error";
    case Severity::Warning: return "warning";
    case Severity::Message: return "message";
    }
    return "message";
}

std::optional<Diagnostic> parseDiagnosticLine(std::string_view raw) {
    std::string_view line = trimLeft(skipNodePrefix(trim(raw)));
    if (line.empty()) {
        return std::nullopt;
    }
    const std::string_view project = takeProject(line);

    // The origin cannot contain ':' except a drive letter's, so the first other ':' ends it.
    std::size_t from = 0;
    if (line.size() >= 2 && isAlpha(line[0]) && line[1] == ':') {
        from = 2;
    } else if (line.size() >= 6 && line.starts_with("\\\\?\\") && isAlpha(line[4]) && line[5] == ':') {
        from = 6;
    }
    const std::size_t first = line.find(':', from);
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view origin;
    std::optional<Head> head;
    std::size_t textStart = 0;
    if (from == 0) {
        // No origin ("error MSB1009: ..."). Only errors and warnings take this form: a bare
        // "Note: including file: ..." is /showIncludes output, not a diagnostic.
        head = parseHead(line.substr(0, first));
        if (head && head->severity == Severity::Message) {
            head.reset();
        }
    }
    if (head) {
        textStart = first + 1;
    } else {
        origin = trim(line.substr(0, first));
        const std::size_t second = line.find(':', first + 1);
        if (origin.empty() || second == std::string_view::npos) {
            return std::nullopt;
        }
        head = parseHead(line.substr(first + 1, second - first - 1));
        if (!head) {
            return std::nullopt;
        }
        textStart = second + 1;
    }

    Diagnostic diagnostic;
    diagnostic.severity = head->severity;
    diagnostic.code = std::string(head->code);
    diagnostic.project = std::string(project);
    if (origin.ends_with(')')) {
        const std::size_t open = origin.rfind('(');
        if (open != std::string_view::npos) {
            if (const auto location = parseLocation(origin.substr(open + 1, origin.size() - open - 2))) {
                diagnostic.line = location->line;
                diagnostic.column = location->column;
                origin = trimRight(origin.substr(0, open));
            }
        }
    }
    diagnostic.file = std::string(origin);

    std::string_view text = trim(line.substr(textStart));
    // clang names the warning's flag at the end: "unused variable 'a' [-Wunused-variable]".
    if (diagnostic.code.empty() && text.ends_with(']')) {
        const std::size_t open = text.rfind(" [-W");
        if (open != std::string_view::npos && text.find(']', open) == text.size() - 1) {
            diagnostic.code = std::string(text.substr(open + 2, text.size() - open - 3));
            text = trimRight(text.substr(0, open));
        }
    }
    diagnostic.message = std::string(text);
    return diagnostic;
}

bool DiagnosticSet::add(Diagnostic diagnostic) {
    Key key{diagnostic.severity, diagnostic.code,   diagnostic.message, diagnostic.file,
            diagnostic.line,     diagnostic.column, diagnostic.project};
    if (!m_keys.insert(std::move(key)).second) {
        return false;
    }
    switch (diagnostic.severity) {
    case Severity::Error: ++m_errors; break;
    case Severity::Warning: ++m_warnings; break;
    case Severity::Message: ++m_messages; break;
    }
    m_items.push_back(std::move(diagnostic));
    return true;
}

} // namespace frost::build
