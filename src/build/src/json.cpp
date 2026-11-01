// SPDX-License-Identifier: Apache-2.0
//
// The minimal JSON reader of frost::build (see json.hpp): a recursive-descent parser written from
// RFC 8259's grammar.

#include "json.hpp"

#include <cstdint>
#include <format>

namespace frost::build::detail {

namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view text) noexcept : m_text(text) {}

    Result<JsonValue> document() {
        if (m_text.starts_with("\xEF\xBB\xBF")) {
            m_pos = 3;
        }
        JsonValue value;
        if (auto parsed = parseValue(value, 0); !parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        skipWhitespace();
        if (m_pos != m_text.size()) {
            return error("unexpected text after the value");
        }
        return value;
    }

private:
    std::unexpected<Error> error(std::string_view what) const {
        return fail(ErrorCode::Corrupt, std::format("JSON: {} at byte {}", what, m_pos));
    }

    void skipWhitespace() noexcept {
        while (m_pos < m_text.size() &&
               (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\r' || m_text[m_pos] == '\n')) {
            ++m_pos;
        }
    }

    bool consume(std::string_view literal) noexcept {
        if (m_text.substr(m_pos).starts_with(literal)) {
            m_pos += literal.size();
            return true;
        }
        return false;
    }

    Result<void> parseValue(JsonValue& out, int depth) {
        if (depth >= kMaxDepth) {
            return error("nesting too deep");
        }
        skipWhitespace();
        if (m_pos >= m_text.size()) {
            return error("unexpected end");
        }
        const char c = m_text[m_pos];
        if (c == '{') {
            return parseObject(out, depth);
        }
        if (c == '[') {
            return parseArray(out, depth);
        }
        if (c == '"') {
            out.kind = JsonValue::Kind::String;
            return parseString(out.text);
        }
        if (consume("true")) {
            out.kind = JsonValue::Kind::Bool;
            out.boolean = true;
            return {};
        }
        if (consume("false")) {
            out.kind = JsonValue::Kind::Bool;
            return {};
        }
        if (consume("null")) {
            out.kind = JsonValue::Kind::Null;
            return {};
        }
        return parseNumber(out);
    }

    Result<void> parseObject(JsonValue& out, int depth) {
        out.kind = JsonValue::Kind::Object;
        ++m_pos; // '{'
        skipWhitespace();
        if (consume("}")) {
            return {};
        }
        while (true) {
            skipWhitespace();
            if (m_pos >= m_text.size() || m_text[m_pos] != '"') {
                return error("expected a member name");
            }
            std::string name;
            if (auto parsed = parseString(name); !parsed) {
                return parsed;
            }
            skipWhitespace();
            if (!consume(":")) {
                return error("expected ':'");
            }
            JsonValue value;
            if (auto parsed = parseValue(value, depth + 1); !parsed) {
                return parsed;
            }
            out.memberNames.push_back(std::move(name));
            out.memberValues.push_back(std::move(value));
            skipWhitespace();
            if (consume("}")) {
                return {};
            }
            if (!consume(",")) {
                return error("expected ',' or '}'");
            }
        }
    }

    Result<void> parseArray(JsonValue& out, int depth) {
        out.kind = JsonValue::Kind::Array;
        ++m_pos; // '['
        skipWhitespace();
        if (consume("]")) {
            return {};
        }
        while (true) {
            JsonValue value;
            if (auto parsed = parseValue(value, depth + 1); !parsed) {
                return parsed;
            }
            out.items.push_back(std::move(value));
            skipWhitespace();
            if (consume("]")) {
                return {};
            }
            if (!consume(",")) {
                return error("expected ',' or ']'");
            }
        }
    }

    // A number's literal text, checked against the grammar: -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
    Result<void> parseNumber(JsonValue& out) {
        const std::size_t start = m_pos;
        auto digits = [&] {
            const std::size_t from = m_pos;
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') {
                ++m_pos;
            }
            return m_pos - from;
        };
        consume("-");
        if (consume("0")) {
            // No leading zeros.
        } else if (digits() == 0) {
            return error("unexpected character");
        }
        if (consume(".") && digits() == 0) {
            return error("expected a digit");
        }
        if (consume("e") || consume("E")) {
            if (!consume("+")) {
                consume("-");
            }
            if (digits() == 0) {
                return error("expected a digit");
            }
        }
        out.kind = JsonValue::Kind::Number;
        out.text = std::string(m_text.substr(start, m_pos - start));
        return {};
    }

    Result<unsigned> hex4() {
        if (m_text.size() - m_pos < 4) {
            return error("short \\u escape");
        }
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_text[m_pos++];
            value <<= 4U;
            if (c >= '0' && c <= '9') {
                value |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return error("bad \\u escape");
            }
        }
        return value;
    }

    static void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
    }

    Result<void> parseString(std::string& out) {
        ++m_pos; // '"'
        while (true) {
            if (m_pos >= m_text.size()) {
                return error("unterminated string");
            }
            const char c = m_text[m_pos++];
            if (c == '"') {
                return {};
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return error("control character in string");
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (m_pos >= m_text.size()) {
                return error("unterminated escape");
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
                auto unit = hex4();
                if (!unit) {
                    return std::unexpected(std::move(unit.error()));
                }
                std::uint32_t cp = *unit;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (!consume("\\u")) {
                        return error("unpaired surrogate");
                    }
                    auto low = hex4();
                    if (!low) {
                        return std::unexpected(std::move(low.error()));
                    }
                    if (*low < 0xDC00 || *low > 0xDFFF) {
                        return error("unpaired surrogate");
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10U) + (*low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return error("unpaired surrogate");
                }
                appendUtf8(out, cp);
                break;
            }
            default: return error("bad escape");
            }
        }
    }

    std::string_view m_text;
    std::size_t m_pos = 0;
};

} // namespace

const JsonValue* JsonValue::find(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < memberNames.size(); ++i) {
        if (memberNames[i] == name) {
            return &memberValues[i];
        }
    }
    return nullptr;
}

std::string JsonValue::stringMember(std::string_view name) const {
    const JsonValue* value = find(name);
    return value != nullptr && value->kind == Kind::String ? value->text : std::string();
}

bool JsonValue::boolMember(std::string_view name, bool fallback) const noexcept {
    const JsonValue* value = find(name);
    return value != nullptr && value->kind == Kind::Bool ? value->boolean : fallback;
}

Result<JsonValue> parseJson(std::string_view text) { return Parser(text).document(); }

} // namespace frost::build::detail
