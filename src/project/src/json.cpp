// SPDX-License-Identifier: Apache-2.0
//
// The private JSON reader of frost::project (json.hpp): a yyjson document converted to JsonNode.

#include "json.hpp"

#include <format>
#include <limits>
#include <memory>

#include <yyjson.h>

namespace frost::project::detail {
namespace {

constexpr int kMaxDepth = 128;

struct DocDeleter {
    void operator()(yyjson_doc* doc) const noexcept { yyjson_doc_free(doc); }
};
using DocPtr = std::unique_ptr<yyjson_doc, DocDeleter>;

// Converts a yyjson value tree; false when nesting exceeds kMaxDepth.
bool convert(yyjson_val* value, JsonNode& out, int depth) {
    if (depth > kMaxDepth) {
        return false;
    }
    switch (yyjson_get_type(value)) {
    case YYJSON_TYPE_NULL: out.kind = JsonNode::Kind::Null; return true;
    case YYJSON_TYPE_BOOL:
        out.kind = JsonNode::Kind::Bool;
        out.boolean = yyjson_get_bool(value);
        return true;
    case YYJSON_TYPE_NUM:
        out.kind = JsonNode::Kind::Number;
        if (yyjson_is_sint(value)) {
            out.integer = yyjson_get_sint(value);
            out.number = static_cast<double>(*out.integer);
        } else if (yyjson_is_uint(value)) {
            const std::uint64_t n = yyjson_get_uint(value);
            if (n <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                out.integer = static_cast<std::int64_t>(n);
            }
            out.number = static_cast<double>(n);
        } else {
            out.number = yyjson_get_real(value);
        }
        return true;
    case YYJSON_TYPE_STR:
        out.kind = JsonNode::Kind::String;
        out.text.assign(yyjson_get_str(value), yyjson_get_len(value));
        return true;
    case YYJSON_TYPE_ARR: {
        out.kind = JsonNode::Kind::Array;
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(value, &iter);
        while (yyjson_val* item = yyjson_arr_iter_next(&iter)) {
            JsonNode converted;
            if (!convert(item, converted, depth + 1)) {
                return false;
            }
            out.items.push_back(std::move(converted));
        }
        return true;
    }
    case YYJSON_TYPE_OBJ: {
        out.kind = JsonNode::Kind::Object;
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(value, &iter);
        while (yyjson_val* key = yyjson_obj_iter_next(&iter)) {
            JsonNode converted;
            if (!convert(yyjson_obj_iter_get_val(key), converted, depth + 1)) {
                return false;
            }
            out.keys.emplace_back(yyjson_get_str(key), yyjson_get_len(key));
            out.items.push_back(std::move(converted));
        }
        return true;
    }
    default: return false;
    }
}

} // namespace

const JsonNode* JsonNode::find(std::string_view key) const noexcept {
    if (kind != Kind::Object) {
        return nullptr;
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i] == key) {
            return &items[i];
        }
    }
    return nullptr;
}

std::string JsonNode::stringAt(std::string_view key, std::string_view fallback) const {
    const JsonNode* member = find(key);
    return member != nullptr && member->isString() ? member->text : std::string(fallback);
}

bool JsonNode::boolAt(std::string_view key, bool fallback) const noexcept {
    const JsonNode* member = find(key);
    return member != nullptr && member->isBool() ? member->boolean : fallback;
}

std::optional<std::int64_t> JsonNode::integerAt(std::string_view key) const noexcept {
    const JsonNode* member = find(key);
    return member != nullptr && member->isNumber() ? member->integer : std::nullopt;
}

Result<JsonNode> parseJson(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    yyjson_read_err error{};
    // Without YYJSON_READ_INSITU yyjson only reads the buffer, so dropping const is safe.
    DocPtr doc(yyjson_read_opts(const_cast<char*>(text.data()), text.size(), YYJSON_READ_NOFLAG, nullptr, &error));
    if (!doc) {
        return fail(ErrorCode::Corrupt,
                    std::format("invalid JSON at byte {}: {}", error.pos, error.msg != nullptr ? error.msg : "?"));
    }
    JsonNode out;
    if (!convert(yyjson_doc_get_root(doc.get()), out, 0)) {
        return fail(ErrorCode::Corrupt, std::format("JSON nested deeper than {} levels", kMaxDepth));
    }
    return out;
}

} // namespace frost::project::detail
