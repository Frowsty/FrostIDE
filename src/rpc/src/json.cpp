// SPDX-License-Identifier: Apache-2.0
//
// frost::rpc::Json (json.hpp): the value type, the compact writer and the parser over yyjson.
// Moved here from frost::lang (ADR 0050) so the DAP client shares it with the LSP client.

#include "frost/rpc/json.hpp"

#include "frost/core/assert.hpp"

#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <memory>

#include <yyjson.h>

namespace frost::rpc {
namespace {

constexpr int kMaxDepth = 256;

struct DocDeleter {
    void operator()(yyjson_doc* doc) const noexcept { yyjson_doc_free(doc); }
};
using DocPtr = std::unique_ptr<yyjson_doc, DocDeleter>;

// Converts a yyjson value tree; false when nesting exceeds kMaxDepth.
bool convert(yyjson_val* value, Json& out, int depth) {
    if (depth > kMaxDepth) {
        return false;
    }
    switch (yyjson_get_type(value)) {
    case YYJSON_TYPE_NULL: out = Json(); return true;
    case YYJSON_TYPE_BOOL: out = Json(yyjson_get_bool(value)); return true;
    case YYJSON_TYPE_NUM:
        if (yyjson_is_sint(value)) {
            out = Json(yyjson_get_sint(value));
        } else if (yyjson_is_uint(value)) {
            const std::uint64_t number = yyjson_get_uint(value);
            if (number <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                out = Json(static_cast<std::int64_t>(number));
            } else {
                out = Json(static_cast<double>(number));
            }
        } else {
            out = Json(yyjson_get_real(value));
        }
        return true;
    case YYJSON_TYPE_STR: out = Json(std::string(yyjson_get_str(value), yyjson_get_len(value))); return true;
    case YYJSON_TYPE_ARR: {
        out = Json::array();
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(value, &iter);
        while (yyjson_val* item = yyjson_arr_iter_next(&iter)) {
            Json converted;
            if (!convert(item, converted, depth + 1)) {
                return false;
            }
            out.push(std::move(converted));
        }
        return true;
    }
    case YYJSON_TYPE_OBJ: {
        out = Json::object();
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(value, &iter);
        while (yyjson_val* key = yyjson_obj_iter_next(&iter)) {
            Json converted;
            if (!convert(yyjson_obj_iter_get_val(key), converted, depth + 1)) {
                return false;
            }
            // A duplicate key replaces the earlier value in place (json.hpp).
            out.set(std::string(yyjson_get_str(key), yyjson_get_len(key)), std::move(converted));
        }
        return true;
    }
    default: return false;
    }
}

void appendEscaped(std::string& out, std::string_view text) {
    out += '"';
    for (char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (byte < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(byte));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

} // namespace

Json Json::array(std::vector<Json> items) {
    Json value;
    value.m_kind = Kind::Array;
    value.m_items = std::move(items);
    return value;
}

Json Json::object() {
    Json value;
    value.m_kind = Kind::Object;
    return value;
}

std::optional<bool> Json::asBool() const noexcept {
    return m_kind == Kind::Bool ? std::optional<bool>(m_bool) : std::nullopt;
}

std::optional<std::int64_t> Json::asInteger() const noexcept {
    if (m_kind == Kind::Integer) {
        return m_integer;
    }
    // 2^63 is exactly representable; values in [-2^63, 2^63) convert without overflow.
    constexpr double kLimit = 9223372036854775808.0;
    if (m_kind == Kind::Double && std::isfinite(m_double) && std::trunc(m_double) == m_double && m_double >= -kLimit &&
        m_double < kLimit) {
        return static_cast<std::int64_t>(m_double);
    }
    return std::nullopt;
}

std::optional<double> Json::asDouble() const noexcept {
    if (m_kind == Kind::Integer) {
        return static_cast<double>(m_integer);
    }
    return m_kind == Kind::Double ? std::optional<double>(m_double) : std::nullopt;
}

const std::string* Json::asString() const noexcept { return m_kind == Kind::String ? &m_string : nullptr; }

Json& Json::push(Json value) & {
    FROST_PRE(m_kind == Kind::Array || m_kind == Kind::Null);
    m_kind = Kind::Array;
    m_items.push_back(std::move(value));
    return *this;
}

Json&& Json::push(Json value) && { return std::move(push(std::move(value))); }

const Json* Json::find(std::string_view key) const noexcept {
    if (m_kind != Kind::Object) {
        return nullptr;
    }
    for (std::size_t i = 0; i < m_keys.size(); ++i) {
        if (m_keys[i] == key) {
            return &m_items[i];
        }
    }
    return nullptr;
}

Json* Json::find(std::string_view key) noexcept { return const_cast<Json*>(std::as_const(*this).find(key)); }

Json& Json::set(std::string key, Json value) & {
    FROST_PRE(m_kind == Kind::Object || m_kind == Kind::Null);
    m_kind = Kind::Object;
    for (std::size_t i = 0; i < m_keys.size(); ++i) {
        if (m_keys[i] == key) {
            m_items[i] = std::move(value);
            return *this;
        }
    }
    m_keys.push_back(std::move(key));
    m_items.push_back(std::move(value));
    return *this;
}

Json&& Json::set(std::string key, Json value) && { return std::move(set(std::move(key), std::move(value))); }

std::string_view Json::stringAt(std::string_view key, std::string_view fallback) const noexcept {
    const Json* member = find(key);
    const std::string* text = member != nullptr ? member->asString() : nullptr;
    return text != nullptr ? std::string_view(*text) : fallback;
}

std::optional<std::int64_t> Json::integerAt(std::string_view key) const noexcept {
    const Json* member = find(key);
    return member != nullptr ? member->asInteger() : std::nullopt;
}

std::optional<bool> Json::boolAt(std::string_view key) const noexcept {
    const Json* member = find(key);
    return member != nullptr ? member->asBool() : std::nullopt;
}

std::string Json::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

void Json::dumpTo(std::string& out) const {
    switch (m_kind) {
    case Kind::Null: out += "null"; return;
    case Kind::Bool: out += m_bool ? "true" : "false"; return;
    case Kind::Integer: out += std::to_string(m_integer); return;
    case Kind::Double: {
        if (!std::isfinite(m_double)) {
            out += "null";
            return;
        }
        char buffer[64];
        const auto [end, error] = std::to_chars(buffer, buffer + sizeof buffer, m_double);
        out.append(buffer, error == std::errc() ? end : buffer);
        return;
    }
    case Kind::String: appendEscaped(out, m_string); return;
    case Kind::Array:
        out += '[';
        for (std::size_t i = 0; i < m_items.size(); ++i) {
            if (i != 0) {
                out += ',';
            }
            m_items[i].dumpTo(out);
        }
        out += ']';
        return;
    case Kind::Object:
        out += '{';
        for (std::size_t i = 0; i < m_items.size(); ++i) {
            if (i != 0) {
                out += ',';
            }
            appendEscaped(out, m_keys[i]);
            out += ':';
            m_items[i].dumpTo(out);
        }
        out += '}';
        return;
    }
}

bool operator==(const Json& a, const Json& b) noexcept {
    if (a.isNumber() && b.isNumber()) {
        if (a.m_kind == Json::Kind::Integer && b.m_kind == Json::Kind::Integer) {
            return a.m_integer == b.m_integer;
        }
        return a.asDouble() == b.asDouble();
    }
    if (a.m_kind != b.m_kind) {
        return false;
    }
    switch (a.m_kind) {
    case Json::Kind::Null: return true;
    case Json::Kind::Bool: return a.m_bool == b.m_bool;
    case Json::Kind::String: return a.m_string == b.m_string;
    case Json::Kind::Array: return a.m_items == b.m_items;
    case Json::Kind::Object: return a.m_keys == b.m_keys && a.m_items == b.m_items;
    default: return false;
    }
}

Result<Json> parseJson(std::string_view text) {
    yyjson_read_err error{};
    // Without YYJSON_READ_INSITU yyjson only reads the buffer, so dropping const is safe. Invalid
    // UTF-8 inside strings is accepted (a server may echo file bytes in a message) and kept as is.
    DocPtr doc(yyjson_read_opts(const_cast<char*>(text.data()), text.size(), YYJSON_READ_ALLOW_INVALID_UNICODE, nullptr,
                                &error));
    if (!doc) {
        return fail(ErrorCode::Corrupt,
                    std::format("invalid JSON at byte {}: {}", error.pos, error.msg != nullptr ? error.msg : "?"));
    }
    Json out;
    if (!convert(yyjson_doc_get_root(doc.get()), out, 0)) {
        return fail(ErrorCode::Corrupt, std::format("JSON nested deeper than {} levels", kMaxDepth));
    }
    return out;
}

} // namespace frost::rpc
