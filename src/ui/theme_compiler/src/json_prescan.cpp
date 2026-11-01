// SPDX-License-Identifier: Apache-2.0
//
// The JSON pre-scan (json_prescan.hpp): a small recursive-descent reader of the RFC 8259
// grammar that keeps only nesting depth and member names.

#include "json_prescan.hpp"

#include <cstdint>
#include <set>

namespace frost::ui::detail {

namespace {

constexpr bool isContinuationByte(char c) noexcept { return (static_cast<unsigned char>(c) & 0xc0U) == 0x80U; }

class Scanner {
public:
    Scanner(std::string_view text, int maxDepth, std::size_t maxReported)
        : m_text(text), m_maxDepth(maxDepth), m_maxReported(maxReported) {}

    PrescanResult run() {
        skipSpace();
        if (!value(1)) {
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

    bool value(int depth) {
        switch (peek()) {
        case '{': return object(depth);
        case '[': return array(depth);
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

    /*
     * The path of the innermost member or element, joined from the segment stack. Called only
     * for reported duplicates, so a long name shared by many members is copied at most
     * maxReported times rather than once per member.
     */
    [[nodiscard]] std::string currentPath() const {
        std::string out;
        for (const Segment& segment : m_segments) {
            if (segment.name != nullptr) {
                if (!out.empty()) {
                    out.push_back('.');
                }
                out += *segment.name;
            } else {
                out += "[" + std::to_string(segment.index) + "]";
            }
        }
        return out;
    }

    bool object(int depth) {
        if (!enter(depth)) {
            return false;
        }
        ++m_pos; // '{'
        skipSpace();
        if (peek() == '}') {
            ++m_pos;
            return true;
        }
        // Set nodes never move, so the segment stack can point at the names they hold.
        std::set<std::string> names;
        while (true) {
            std::string name;
            if (!string(name)) {
                return false;
            }
            const auto [it, inserted] = names.insert(std::move(name));
            m_segments.push_back({&*it, 0});
            if (!inserted) {
                if (m_result.duplicates.size() < m_maxReported) {
                    m_result.duplicates.push_back({elideForDiagnostic(currentPath())});
                }
                ++m_result.duplicateCount;
            }
            skipSpace();
            if (peek() != ':') {
                return false;
            }
            ++m_pos;
            skipSpace();
            if (!value(depth + 1)) {
                return false;
            }
            m_segments.pop_back();
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

    bool array(int depth) {
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
            m_segments.push_back({nullptr, index});
            if (!value(depth + 1)) {
                return false;
            }
            m_segments.pop_back();
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

    // One step of the current path: a member name (owned by its object's name set) or, when
    // `name` is null, array element `index`.
    struct Segment {
        const std::string* name;
        std::size_t index;
    };

    std::string_view m_text;
    int m_maxDepth;
    std::size_t m_maxReported;
    std::size_t m_pos = 0;
    std::vector<Segment> m_segments; // at most maxDepth + 1 deep
    PrescanResult m_result;
};

} // namespace

PrescanResult prescanJson(std::string_view text, int maxDepth, std::size_t maxReported) {
    return Scanner(text, maxDepth, maxReported).run();
}

std::string elideForDiagnostic(std::string_view text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) {
        return std::string(text);
    }
    // Cut the head before a continuation byte and start the tail after one, so both halves
    // stay valid UTF-8 when `text` is.
    std::size_t head = maxBytes / 2;
    while (head > 0 && isContinuationByte(text[head])) {
        --head;
    }
    std::size_t tail = text.size() - maxBytes / 2;
    while (tail < text.size() && isContinuationByte(text[tail])) {
        ++tail;
    }
    std::string out(text.substr(0, head));
    out += "...";
    out += text.substr(tail);
    return out;
}

} // namespace frost::ui::detail
