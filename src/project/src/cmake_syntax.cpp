// SPDX-License-Identifier: Apache-2.0
// Lossless CMake command and argument scanner.
#include "frost/project/cmake_syntax.hpp"

namespace frost::project {
namespace {
bool white(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool ident(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

class Scanner {
public:
    explicit Scanner(std::string_view text) : m_text(text) {}
    Result<std::vector<CMakeSyntaxCommand>> scan(bool prefix = false) {
        std::vector<CMakeSyntaxCommand> commands;
        const auto reject = [&](std::string message) -> Result<std::vector<CMakeSyntaxCommand>> {
            if (prefix) return commands;
            return fail(ErrorCode::Unsupported, std::move(message));
        };
        std::vector<std::string> scopes;
        if (m_text.starts_with("\xEF\xBB\xBF"))
            m_pos = 3;
        while (true) {
            if (!trivia())
                return reject("Unterminated bracket comment");
            if (m_pos == m_text.size()) {
                if (!scopes.empty())
                    return reject("Unterminated CMake scope");
                return commands;
            }
            if (!ident(m_text[m_pos]))
                return reject("Expected CMake command");
            CMakeSyntaxCommand command;
            command.line = m_line;
            while (m_pos < m_text.size() && (ident(m_text[m_pos]) || digit(m_text[m_pos]))) {
                char c = m_text[m_pos++];
                command.name += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            }
            if (!trivia() || m_pos == m_text.size() || m_text[m_pos] != '(')
                return reject("Expected command opening parenthesis");
            ++m_pos;
            unsigned depth = 1;
            while (depth) {
                if (!trivia())
                    return reject("Unterminated bracket comment");
                if (m_pos == m_text.size())
                    return reject("Unterminated CMake command");
                if (m_text[m_pos] == '(') {
                    command.nestedArguments = true;
                    ++depth;
                    ++m_pos;
                    continue;
                }
                if (m_text[m_pos] == ')') {
                    if (--depth == 0)
                        command.close = m_pos;
                    ++m_pos;
                    continue;
                }
                CMakeSyntaxArgument arg;
                arg.begin = m_pos;
                bool literal = depth == 1;
                if (m_text[m_pos] == '"') {
                    ++m_pos;
                    while (m_pos < m_text.size() && m_text[m_pos] != '"') {
                        char c = m_text[m_pos++];
                        if (c == '\n')
                            ++m_line;
                        if (c == '$' || c == ';')
                            literal = false;
                        if (c == '\\') {
                            literal = false;
                            if (m_pos == m_text.size())
                                return reject("Unterminated escape");
                            if (m_text[m_pos++] == '\n')
                                ++m_line;
                        } else
                            arg.literal += c;
                    }
                    if (m_pos == m_text.size())
                        return reject("Unterminated quote");
                    ++m_pos;
                } else if (const auto equals = bracketOpen(m_pos); equals != std::string_view::npos) {
                    std::size_t begin = m_pos + equals + 2;
                    if (begin < m_text.size() && m_text[begin] == '\n')
                        ++begin;
                    else if (begin + 1 < m_text.size() && m_text[begin] == '\r' && m_text[begin + 1] == '\n')
                        begin += 2;
                    if (!bracket())
                        return reject("Unterminated bracket argument");
                    arg.literal = std::string(m_text.substr(begin, m_pos - equals - 2 - begin));
                } else {
                    while (m_pos < m_text.size() && !white(m_text[m_pos]) && m_text[m_pos] != '(' &&
                           m_text[m_pos] != ')' && m_text[m_pos] != '#') {
                        char c = m_text[m_pos++];
                        if (c == '$' || c == ';' || c == '"')
                            literal = false;
                        if (c == '\\') {
                            literal = false;
                            if (m_pos == m_text.size())
                                return reject("Unterminated escape");
                            if (m_text[m_pos++] == '\n')
                                ++m_line;
                        } else
                            arg.literal += c;
                    }
                }
                arg.end = m_pos;
                arg.isLiteral = literal;
                if (!literal)
                    arg.literal.clear();
                command.arguments.push_back(std::move(arg));
            }
            const auto& name = command.name;
            const bool closing = name == "endif" || name == "endforeach" || name == "endwhile" ||
                                 name == "endfunction" || name == "endmacro" || name == "endblock";
            if (closing) {
                if (scopes.empty() || "end" + scopes.back() != name)
                    return reject("Mismatched CMake scope");
                scopes.pop_back();
            }
            command.scopeDepth = scopes.size();
            if (name == "if" || name == "foreach" || name == "while" || name == "function" || name == "macro" ||
                name == "block")
                scopes.push_back(name);
            commands.push_back(std::move(command));
        }
    }

private:
    static bool digit(char c) { return c >= '0' && c <= '9'; }
    // Returns the count of '=' in a bracket opener, or npos.
    std::size_t bracketOpen(std::size_t at) const {
        if (at == m_text.size() || m_text[at] != '[')
            return std::string_view::npos;
        std::size_t p = at + 1;
        while (p < m_text.size() && m_text[p] == '=')
            ++p;
        return p < m_text.size() && m_text[p] == '[' ? p - at - 1 : std::string_view::npos;
    }
    bool bracket() {
        const auto count = bracketOpen(m_pos);
        if (count == std::string_view::npos)
            return false;
        m_pos += count + 2;
        while (m_pos < m_text.size()) {
            if (m_text[m_pos] == ']') {
                std::size_t p = m_pos + 1;
                while (p < m_text.size() && m_text[p] == '=')
                    ++p;
                if (p - m_pos - 1 == count && p < m_text.size() && m_text[p] == ']') {
                    m_pos = p + 1;
                    return true;
                }
            }
            if (m_text[m_pos++] == '\n')
                ++m_line;
        }
        return false;
    }
    bool trivia() {
        while (m_pos < m_text.size()) {
            if (white(m_text[m_pos])) {
                if (m_text[m_pos++] == '\n')
                    ++m_line;
                continue;
            }
            if (m_text[m_pos] != '#')
                break;
            ++m_pos;
            if (bracketOpen(m_pos) != std::string_view::npos) {
                if (!bracket())
                    return false;
            } else
                while (m_pos < m_text.size() && m_text[m_pos] != '\n')
                    ++m_pos;
        }
        return true;
    }
    std::string_view m_text;
    std::size_t m_pos = 0;
    std::size_t m_line = 1;
};
} // namespace
Result<std::vector<CMakeSyntaxCommand>> scanCMakeSyntax(std::string_view text) { return Scanner(text).scan(); }
std::vector<CMakeSyntaxCommand> scanCMakeSyntaxPrefix(std::string_view text) {
    return Scanner(text).scan(true).value_or(std::vector<CMakeSyntaxCommand>{});
}
} // namespace frost::project
