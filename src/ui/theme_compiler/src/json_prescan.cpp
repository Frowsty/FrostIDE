// SPDX-License-Identifier: Apache-2.0
//
// The JSON pre-scan (json_prescan.hpp): a small recursive-descent reader of the RFC 8259
// grammar that keeps only nesting depth and member names.

#include "json_prescan.hpp"

#include <cstdint>
#include <set>

namespace frost::ui::detail {

namespace {

class Scanner {
public:
    Scanner(std::string_view text, int maxDepth) : m_text(text), m_maxDepth(maxDepth) {}

    PrescanResult run() {
        skipSpace();
        if (!value("", 1)) {
            if (!m_result.depthExceeded) {
                m_result.syntaxError = true;
            }
            return std::move(m_result);
        }
        skipSpace();
        if (m_pos != m_text.size()) {
            m_result.syntaxError = true;
        }
        return std::move(m_result);
    }

private:
    [[nodiscard]] bool atEnd() const noexcept { return m_pos >= m_text.size(); }
    [[nodiscard]] char peek() const noexcept { return atEnd() ? '\0' : m_text[m_pos]; }

    void skipSpace() noexcept {
        while (!atEnd()) {
            const char c = m_text[m_pos];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                break;
            }
            ++m_pos;
        }
    }

    bool literal(std::string_view word) noexcept {
        if (m_text.substr(m_pos, word.size()) != word) {
            return false;
        }
        m_pos += word.size();
        return true;
    }

    // Appends the UTF-8 encoding of `cp` (a scalar value) to `out`.
    static void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }

    bool hex4(std::uint32_t& out) noexcept {
        if (m_pos + 4 > m_text.size()) {
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos++];
            std::uint32_t digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
            out = out * 16 + digit;
        }
        return true;
    }

    // A string starting at '"'; its decoded value goes to `out`. A lone surrogate escape
    // decodes to U+FFFD (it can only make two names compare equal if both use it).
    bool string(std::string& out) {
        if (peek() != '"') {
            return false;
        }
        ++m_pos;
        while (!atEnd()) {
            const char c = m_text[m_pos++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false; // Control characters must be escaped.
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (atEnd()) {
                return false;
            }
            const char e = m_text[m_pos++];
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp)) {
                    return false;
                }
                if (cp >= 0xd800 && cp <= 0xdbff && m_text.substr(m_pos, 2) == "\\u") {
                    const std::size_t save = m_pos;
                    m_pos += 2;
                    std::uint32_t low = 0;
                    if (hex4(low) && low >= 0xdc00 && low <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    } else {
                        m_pos = save;
                        cp = 0xfffd;
                    }
                } else if (cp >= 0xd800 && cp <= 0xdfff) {
                    cp = 0xfffd;
                }
                appendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool number() noexcept {
        const std::size_t start = m_pos;
        if (peek() == '-') {
            ++m_pos;
        }
        if (peek() == '0') {
            ++m_pos;
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') {
                ++m_pos;
            }
        } else {
            return false;
        }
        if (peek() == '.') {
            ++m_pos;
            if (!(peek() >= '0' && peek() <= '9')) {
                return false;
            }
            while (peek() >= '0' && peek() <= '9') {
                ++m_pos;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++m_pos;
            if (peek() == '+' || peek() == '-') {
                ++m_pos;
            }
            if (!(peek() >= '0' && peek() <= '9')) {
                return false;
            }
            while (peek() >= '0' && peek() <= '9') {
                ++m_pos;
            }
        }
        return m_pos > start;
    }

    bool enter(int depth) noexcept {
        if (depth > m_result.depth) {
            m_result.depth = depth;
        }
        if (depth > m_maxDepth) {
            m_result.depthExceeded = true;
            return false;
        }
        return true;
    }

    bool value(const std::string& path, int depth) {
        switch (peek()) {
        case '{': return object(path, depth);
        case '[': return array(path, depth);
        case '"': {
            std::string ignored;
            return string(ignored);
        }
        case 't': return literal("true");
        case 'f': return literal("false");
        case 'n': return literal("null");
        default: return number();
        }
    }

    bool object(const std::string& path, int depth) {
        if (!enter(depth)) {
            return false;
        }
        ++m_pos; // '{'
        skipSpace();
        if (peek() == '}') {
            ++m_pos;
            return true;
        }
        std::set<std::string> names;
        while (true) {
            std::string name;
            if (!string(name)) {
                return false;
            }
            std::string memberPath = path.empty() ? name : path + "." + name;
            if (!names.insert(name).second) {
                m_result.duplicates.push_back({memberPath});
            }
            skipSpace();
            if (peek() != ':') {
                return false;
            }
            ++m_pos;
            skipSpace();
            if (!value(memberPath, depth + 1)) {
                return false;
            }
            skipSpace();
            if (peek() == ',') {
                ++m_pos;
                skipSpace();
                continue;
            }
            if (peek() == '}') {
                ++m_pos;
                return true;
            }
            return false;
        }
    }

    bool array(const std::string& path, int depth) {
        if (!enter(depth)) {
            return false;
        }
        ++m_pos; // '['
        skipSpace();
        if (peek() == ']') {
            ++m_pos;
            return true;
        }
        for (std::size_t index = 0;; ++index) {
            if (!value(path + "[" + std::to_string(index) + "]", depth + 1)) {
                return false;
            }
            skipSpace();
            if (peek() == ',') {
                ++m_pos;
                skipSpace();
                continue;
            }
            if (peek() == ']') {
                ++m_pos;
                return true;
            }
            return false;
        }
    }

    std::string_view m_text;
    int m_maxDepth;
    std::size_t m_pos = 0;
    PrescanResult m_result;
};

} // namespace

PrescanResult prescanJson(std::string_view text, int maxDepth) { return Scanner(text, maxDepth).run(); }

} // namespace frost::ui::detail
