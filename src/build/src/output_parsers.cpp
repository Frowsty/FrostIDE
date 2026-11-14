// SPDX-License-Identifier: Apache-2.0
//
// The output parsers (frost/build/output_parsers.hpp): the GNU-format scanner, the CMake message
// block reader and the per-stream parser objects BuildSession runs.

#include "frost/build/output_parsers.hpp"
#include "json.hpp"

#include <charconv>
#include <utility>

namespace frost::build {

namespace {

constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool isAlpha(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

std::string_view trimLeft(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    return text;
}

std::string_view trimRight(std::string_view text) noexcept {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// `line` without ANSI CSI escape sequences ("\x1b[1;31m").
std::string stripEscapes(std::string_view line) {
    std::string out;
    out.reserve(line.size());
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
            std::size_t j = i + 2;
            while (j < line.size() && !(line[j] >= '@' && line[j] <= '~')) {
                ++j;
            }
            i = j; // Skips the final byte too.
            continue;
        }
        out += line[i];
    }
    return out;
}

// Reads a positive decimal number at the front of `text`, consuming it.
std::optional<int> takeNumber(std::string_view& text) noexcept {
    std::size_t length = 0;
    while (length < text.size() && isDigit(text[length])) {
        ++length;
    }
    if (length == 0) {
        return std::nullopt;
    }
    int value = 0;
    // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): from_chars reads its explicit begin/end range.
    const auto [end, error] = std::from_chars(text.data(), text.data() + length, value);
    if (error != std::errc{} || value <= 0) {
        return std::nullopt;
    }
    text.remove_prefix(length);
    return value;
}

// Where the origin's own colons end: after a Windows drive prefix ("C:\", "C:/"), else 0.
std::size_t originSearchStart(std::string_view line) noexcept {
    if (line.size() >= 3 && isAlpha(line[0]) && line[1] == ':' && (line[2] == '\\' || line[2] == '/')) {
        return 2;
    }
    return 0;
}

// "path:line[:col]" followed by ':' or ',' (or the end), for the include-chain lines.
std::optional<Diagnostic> includeSite(std::string_view rest) {
    rest = trimRight(rest);
    while (!rest.empty() && (rest.back() == ':' || rest.back() == ',')) {
        rest.remove_suffix(1);
    }
    // From the end: [":" col] ":" line.
    const auto lastNumber = [](std::string_view text, std::size_t& start) -> std::optional<int> {
        std::size_t i = text.size();
        while (i > 0 && isDigit(text[i - 1])) {
            --i;
        }
        if (i == text.size() || i == 0 || text[i - 1] != ':') {
            return std::nullopt;
        }
        int value = 0;
        std::from_chars(text.data() + i, text.data() + text.size(), value);
        start = i - 1;
        return value;
    };
    std::size_t cut = 0;
    auto second = lastNumber(rest, cut);
    if (!second) {
        return std::nullopt;
    }
    std::string_view head = rest.substr(0, cut);
    int line = *second;
    int column = 0;
    if (auto first = lastNumber(head, cut); first && cut > originSearchStart(head)) {
        line = *first;
        column = *second;
        head = head.substr(0, cut);
    }
    if (head.empty() || line <= 0) {
        return std::nullopt;
    }
    return Diagnostic{.severity = Severity::Message,
                      .code = {},
                      .message = "In file included from here",
                      .file = std::string(head),
                      .line = line,
                      .column = column,
                      .project = {}};
}

// The severity word at the front of `text` (" error: ..."), consuming it and the ": ".
std::optional<Severity> takeSeverity(std::string_view& text) noexcept {
    static constexpr std::pair<std::string_view, Severity> kWords[] = {
        {"fatal error:", Severity::Error}, {"error:", Severity::Error},    {"warning:", Severity::Warning},
        {"note:", Severity::Message},      {"remark:", Severity::Message},
    };
    std::string_view rest = trimLeft(text);
    if (rest.size() == text.size()) {
        return std::nullopt; // The severity follows ": ", so at least one space.
    }
    for (const auto& [word, severity] : kWords) {
        if (rest.starts_with(word)) {
            rest.remove_prefix(word.size());
            text = trimLeft(rest);
            return severity;
        }
    }
    return std::nullopt;
}

// Moves a trailing "[-Wflag]" or "[-Werror,-Wflag]" from the message to the code.
void takeFlag(Diagnostic& diagnostic) {
    std::string_view message = trimRight(diagnostic.message);
    if (!message.ends_with(']')) {
        return;
    }
    const std::size_t open = message.rfind(" [-");
    if (open == std::string_view::npos) {
        return;
    }
    std::string_view flags = message.substr(open + 2, message.size() - open - 3);
    if (flags.find(' ') != std::string_view::npos) {
        return;
    }
    if (const std::size_t comma = flags.rfind(','); comma != std::string_view::npos) {
        flags = flags.substr(comma + 1);
    }
    diagnostic.code = std::string(flags);
    diagnostic.message = std::string(trimRight(message.substr(0, open)));
}

class LineParser final : public IOutputParser {
public:
    explicit LineParser(std::optional<Diagnostic> (*parse)(std::string_view)) noexcept : m_parse(parse) {}
    void feed(std::string_view line, const DiagnosticSink& sink) override {
        if (auto diagnostic = m_parse(line)) {
            sink(std::move(*diagnostic));
        }
    }

private:
    std::optional<Diagnostic> (*m_parse)(std::string_view);
};

// CMake's message blocks (output_parsers.hpp, "CMake format").
class CMakeOutputParser final : public IOutputParser {
public:
    void feed(std::string_view raw, const DiagnosticSink& sink) override {
        const std::string clean = stripEscapes(raw);
        const std::string_view line = trimRight(clean);
        if (m_open) {
            if (line.empty()) {
                return;
            }
            if (line.front() == ' ' || line.front() == '\t') {
                if (!m_inCallStack) {
                    appendText(trimLeft(line));
                }
                return;
            }
            if (line == "Call Stack (most recent call first):") {
                m_inCallStack = true;
                return;
            }
            emit(sink);
        }
        start(line, sink);
    }

    void finish(const DiagnosticSink& sink) override {
        if (m_open) {
            emit(sink);
        }
    }

private:
    void appendText(std::string_view text) {
        if (!m_current.message.empty()) {
            m_current.message += ' ';
        }
        m_current.message += text;
    }

    void emit(const DiagnosticSink& sink) {
        m_open = false;
        m_inCallStack = false;
        if (m_current.message.empty()) {
            m_current.message = m_fallbackText;
        }
        sink(std::move(m_current));
        m_current = {};
    }

    // Opens a block when `line` is a CMake message header.
    void start(std::string_view line, const DiagnosticSink& sink) {
        if (!line.starts_with("CMake ")) {
            return;
        }
        std::string_view rest = line.substr(6);
        Severity severity = Severity::Error;
        if (rest.starts_with("Error")) {
            rest.remove_prefix(5);
        } else if (rest.starts_with("Warning (dev)")) {
            severity = Severity::Warning;
            rest.remove_prefix(13);
        } else if (rest.starts_with("Warning")) {
            severity = Severity::Warning;
            rest.remove_prefix(7);
        } else if (rest.starts_with("Deprecation Error")) {
            rest.remove_prefix(17);
        } else if (rest.starts_with("Deprecation Warning")) {
            severity = Severity::Warning;
            rest.remove_prefix(19);
        } else {
            return;
        }
        m_current = Diagnostic{
            .severity = severity, .code = {}, .message = {}, .file = {}, .line = 0, .column = 0, .project = {}};
        m_fallbackText.clear();
        if (rest.starts_with(": ")) {
            // "CMake Error: text": one line, but indented continuation lines may follow.
            appendText(trimLeft(rest.substr(2)));
            m_open = true;
            return;
        }
        if (rest.starts_with(" at ")) {
            rest.remove_prefix(4);
            // "<file>:<line> (<command>):", where file may contain ':' (a drive) and spaces.
            const std::size_t paren = rest.rfind(" (");
            std::string_view location = paren == std::string_view::npos ? rest : rest.substr(0, paren);
            if (paren != std::string_view::npos) {
                std::string_view command = rest.substr(paren + 2);
                if (command.ends_with("):")) {
                    command.remove_suffix(2);
                }
                m_fallbackText = std::string(command);
            }
            if (location.ends_with(':')) {
                location.remove_suffix(1);
            }
            const std::size_t colon = location.rfind(':');
            if (colon != std::string_view::npos && colon > originSearchStart(location)) {
                std::string_view number = location.substr(colon + 1);
                if (auto parsed = takeNumber(number); parsed && number.empty()) {
                    m_current.file = std::string(location.substr(0, colon));
                    m_current.line = *parsed;
                }
            }
            if (m_current.file.empty()) {
                m_current.file = std::string(location);
            }
            m_open = true;
            return;
        }
        if (rest.starts_with(" in ")) {
            std::string_view file = rest.substr(4);
            if (file.ends_with(':')) {
                file.remove_suffix(1);
            }
            m_current.file = std::string(file);
            m_open = true;
            return;
        }
        if (rest == ":") {
            m_open = true; // "CMake Error:" with the text on the next lines.
            return;
        }
        static_cast<void>(sink);
    }

    bool m_open = false;
    bool m_inCallStack = false;
    Diagnostic m_current{};
    std::string m_fallbackText;
};

} // namespace

void IOutputParser::finish(const DiagnosticSink& /*sink*/) {}

std::optional<Diagnostic> parseGnuDiagnosticLine(std::string_view raw) {
    const std::string clean = stripEscapes(raw);
    std::string_view line = trimRight(clean);
    if (line.starts_with("In file included from ")) {
        return includeSite(line.substr(22));
    }
    if (const std::string_view indented = trimLeft(line);
        indented.size() < line.size() && indented.starts_with("from ")) {
        return includeSite(indented.substr(5));
    }
    if (line.empty() || line.front() == ' ' || line.front() == '\t') {
        return std::nullopt;
    }
    const std::size_t colon = line.find(':', originSearchStart(line));
    if (colon == std::string_view::npos || colon == 0) {
        return std::nullopt;
    }
    Diagnostic diagnostic;
    diagnostic.file = std::string(line.substr(0, colon));
    std::string_view rest = line.substr(colon + 1);
    if (auto number = takeNumber(rest)) {
        diagnostic.line = *number;
        if (!rest.starts_with(':')) {
            return std::nullopt;
        }
        rest.remove_prefix(1);
        std::string_view afterColumn = rest;
        if (auto column = takeNumber(afterColumn); column && afterColumn.starts_with(':')) {
            diagnostic.column = *column;
            rest = afterColumn.substr(1);
        }
    } else {
        // The tool form: an origin that is a bare program name.
        const std::string_view origin = diagnostic.file;
        if (origin.find_first_of(" \t/\\") != std::string_view::npos) {
            return std::nullopt;
        }
    }
    auto severity = takeSeverity(rest);
    if (!severity) {
        return std::nullopt;
    }
    diagnostic.severity = *severity;
    diagnostic.message = std::string(rest);
    takeFlag(diagnostic);
    return diagnostic;
}

std::optional<Diagnostic> parseCompilerDiagnosticLine(std::string_view line) {
    if (auto canonical = parseDiagnosticLine(line)) {
        return canonical;
    }
    return parseGnuDiagnosticLine(line);
}

std::optional<Diagnostic> parseStructuredMsBuildLine(std::string_view line) {
    constexpr std::string_view prefix = "FROSTBUILD1 ";
    if (!line.starts_with(prefix) || line.size() > 4 * 1024 * 1024)
        return std::nullopt;
    auto event = detail::parseJson(line.substr(prefix.size()));
    if (!event || event->kind != detail::JsonValue::Kind::Object)
        return std::nullopt;
    const auto severity = event->stringMember("Severity");
    if (severity != "error" && severity != "warning")
        return std::nullopt;
    const auto number = [&](std::string_view key) {
        const auto* member = event->find(key);
        int value = 0;
        if (!member || member->kind != detail::JsonValue::Kind::Number)
            return 0;
        const auto [end, error] = std::from_chars(member->text.data(), member->text.data() + member->text.size(), value);
        return error == std::errc{} && end == member->text.data() + member->text.size() && value >= 0 ? value : 0;
    };
    return Diagnostic{.severity = severity == "error" ? Severity::Error : Severity::Warning,
                      .code = event->stringMember("Code"), .message = event->stringMember("Message"),
                      .file = event->stringMember("File"), .line = number("Line"), .column = number("Column"),
                      .project = event->stringMember("Project")};
}

std::unique_ptr<IOutputParser> makeStructuredMsBuildOutputParser() {
    return std::make_unique<LineParser>(&parseStructuredMsBuildLine);
}

std::unique_ptr<IOutputParser> makeMsBuildOutputParser() { return std::make_unique<LineParser>(&parseDiagnosticLine); }

std::unique_ptr<IOutputParser> makeCompilerOutputParser() {
    return std::make_unique<LineParser>(&parseCompilerDiagnosticLine);
}

std::unique_ptr<IOutputParser> makeCMakeOutputParser() { return std::make_unique<CMakeOutputParser>(); }

} // namespace frost::build
