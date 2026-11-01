// SPDX-License-Identifier: Apache-2.0
//
// JSON over yyjson (json.hpp): parsing scope files, decoding values by schema, encoding values, and
// the scanner that finds each top-level member's key and value spans in text yyjson has accepted.

#include "json.hpp"

#include "frost/io/utf.hpp"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <limits>
#include <memory>
#include <type_traits>
#include <variant>

#include <yyjson.h>

namespace frost::settings::detail {

namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr yyjson_read_flag kReadFlags = YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS;

struct DocDeleter {
    void operator()(yyjson_doc* doc) const noexcept { yyjson_doc_free(doc); }
};
struct MutDocDeleter {
    void operator()(yyjson_mut_doc* doc) const noexcept { yyjson_mut_doc_free(doc); }
};
using DocPtr = std::unique_ptr<yyjson_doc, DocDeleter>;
using MutDocPtr = std::unique_ptr<yyjson_mut_doc, MutDocDeleter>;

// Parses one JSON text; on failure `error` holds yyjson's reason and position.
[[nodiscard]] DocPtr read(std::string_view text, yyjson_read_err& error) {
    // Without YYJSON_READ_INSITU yyjson only reads the buffer, so dropping const is safe.
    return DocPtr(yyjson_read_opts(const_cast<char*>(text.data()), text.size(), kReadFlags, nullptr, &error));
}

[[nodiscard]] std::string_view stringOf(yyjson_val* value) noexcept {
    return {yyjson_get_str(value), yyjson_get_len(value)};
}

// The scanner. Runs on text yyjson accepted, so it only needs to be bounds-safe, not to diagnose.
class Scanner {
public:
    Scanner(std::string_view text, std::size_t start) noexcept : m_text(text), m_pos(start) {}

    [[nodiscard]] std::size_t pos() const noexcept { return m_pos; }
    [[nodiscard]] bool atEnd() const noexcept { return m_pos >= m_text.size(); }
    [[nodiscard]] char peek() const noexcept { return atEnd() ? '\0' : m_text[m_pos]; }
    void advance() noexcept { ++m_pos; }

    // Skips whitespace, // line comments and /* block */ comments.
    void skipTrivia() noexcept {
        while (!atEnd()) {
            const char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++m_pos;
            } else if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '/') {
                const std::size_t end = m_text.find('\n', m_pos + 2);
                m_pos = end == std::string_view::npos ? m_text.size() : end;
            } else if (c == '/' && m_pos + 1 < m_text.size() && m_text[m_pos + 1] == '*') {
                const std::size_t end = m_text.find("*/", m_pos + 2);
                m_pos = end == std::string_view::npos ? m_text.size() : end + 2;
            } else {
                return;
            }
        }
    }

    // At '"': skips the string literal.
    [[nodiscard]] bool skipString() noexcept {
        ++m_pos;
        while (!atEnd()) {
            const char c = m_text[m_pos++];
            if (c == '\\') {
                ++m_pos;
            } else if (c == '"') {
                return true;
            }
        }
        return false;
    }

    // Skips one value: a string, a nested object or array, or a scalar token.
    [[nodiscard]] bool skipValue() noexcept {
        const char first = peek();
        if (first == '"') {
            return skipString();
        }
        if (first == '{' || first == '[') {
            std::size_t depth = 0;
            while (!atEnd()) {
                const char c = m_text[m_pos];
                if (c == '"') {
                    if (!skipString()) {
                        return false;
                    }
                    continue;
                }
                if (c == '/' && m_pos + 1 < m_text.size() && (m_text[m_pos + 1] == '/' || m_text[m_pos + 1] == '*')) {
                    skipTrivia();
                    continue;
                }
                ++m_pos;
                if (c == '{' || c == '[') {
                    ++depth;
                } else if (c == '}' || c == ']') {
                    if (--depth == 0) {
                        return true;
                    }
                }
            }
            return false;
        }
        const std::size_t start = m_pos;
        while (!atEnd()) {
            const char c = m_text[m_pos];
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '/') {
                break;
            }
            ++m_pos;
        }
        return m_pos > start;
    }

private:
    std::string_view m_text;
    std::size_t m_pos;
};

[[nodiscard]] std::unexpected<Error> corrupt(std::string_view text, std::size_t offset, std::string_view reason) {
    const auto [line, column] = lineColumn(text, offset);
    return fail(ErrorCode::Corrupt, std::format("line {}, column {}: {}", line, column, reason));
}

[[nodiscard]] Result<std::vector<MemberSpan>> scanMembers(std::string_view text, std::size_t open, std::size_t& close) {
    std::vector<MemberSpan> spans;
    Scanner scanner(text, open + 1);
    for (;;) {
        scanner.skipTrivia();
        if (scanner.peek() == '}') {
            close = scanner.pos();
            return spans;
        }
        MemberSpan span;
        span.keyBegin = scanner.pos();
        if (scanner.peek() != '"' || !scanner.skipString()) {
            break;
        }
        span.keyEnd = scanner.pos();
        scanner.skipTrivia();
        if (scanner.peek() != ':') {
            break;
        }
        scanner.advance();
        scanner.skipTrivia();
        span.valueBegin = scanner.pos();
        if (!scanner.skipValue()) {
            break;
        }
        span.valueEnd = scanner.pos();
        scanner.skipTrivia();
        if (scanner.peek() == ',') {
            scanner.advance();
            span.commaEnd = scanner.pos();
        } else if (scanner.peek() != '}') {
            break;
        }
        spans.push_back(span);
    }
    return fail(ErrorCode::Internal, std::format("settings scanner lost its place at byte {}", scanner.pos()));
}

[[nodiscard]] Result<std::string> fieldString(yyjson_val* value, std::string_view what) {
    if (!yyjson_is_str(value)) {
        return fail(ErrorCode::InvalidArgument, std::format("{} must be a string", what));
    }
    return std::string(stringOf(value));
}

[[nodiscard]] Result<StringList> stringList(yyjson_val* value, std::string_view what) {
    if (!yyjson_is_arr(value)) {
        return fail(ErrorCode::InvalidArgument, std::format("{} must be an array of strings", what));
    }
    StringList list;
    list.reserve(yyjson_arr_size(value));
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(value, &iter);
    while (yyjson_val* item = yyjson_arr_iter_next(&iter)) {
        if (!yyjson_is_str(item)) {
            return fail(ErrorCode::InvalidArgument, std::format("{} must be an array of strings", what));
        }
        list.emplace_back(stringOf(item));
    }
    return list;
}

[[nodiscard]] Result<std::int64_t> integerOf(yyjson_val* value, std::string_view what) {
    if (yyjson_is_sint(value)) {
        return yyjson_get_sint(value);
    }
    if (yyjson_is_uint(value)) {
        const std::uint64_t number = yyjson_get_uint(value);
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return fail(ErrorCode::InvalidArgument, std::format("{} is too large", what));
        }
        return static_cast<std::int64_t>(number);
    }
    return fail(ErrorCode::InvalidArgument, std::format("{} must be an integer", what));
}

[[nodiscard]] Result<Record> decodeRecord(yyjson_val* value, const Schema& schema, std::size_t index) {
    if (!yyjson_is_obj(value)) {
        return fail(ErrorCode::InvalidArgument, std::format("entry {} must be an object", index + 1));
    }
    std::vector<RecordField> fields;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(value, &iter);
    while (yyjson_val* key = yyjson_obj_iter_next(&iter)) {
        yyjson_val* fieldValue = yyjson_obj_iter_get_val(key);
        const std::string name(stringOf(key));
        const auto spec = std::ranges::find(schema.fields, name, &FieldSpec::name);
        if (spec == schema.fields.end()) {
            return fail(ErrorCode::InvalidArgument, std::format("entry {}: unknown field \"{}\"", index + 1, name));
        }
        const std::string what = std::format("entry {}: field \"{}\"", index + 1, name);
        FieldValue decoded;
        switch (spec->type) {
        case FieldType::Bool:
            if (!yyjson_is_bool(fieldValue)) {
                return fail(ErrorCode::InvalidArgument, what + " must be true or false");
            }
            decoded = yyjson_get_bool(fieldValue);
            break;
        case FieldType::Integer: {
            auto number = integerOf(fieldValue, what);
            if (!number) {
                return std::unexpected(number.error());
            }
            decoded = *number;
            break;
        }
        case FieldType::String: {
            auto text = fieldString(fieldValue, what);
            if (!text) {
                return std::unexpected(text.error());
            }
            decoded = std::move(*text);
            break;
        }
        case FieldType::StringList: {
            auto list = stringList(fieldValue, what);
            if (!list) {
                return std::unexpected(list.error());
            }
            decoded = std::move(*list);
            break;
        }
        }
        fields.push_back(RecordField{.name = name, .value = std::move(decoded)});
    }
    return makeRecord(std::move(fields));
}

[[nodiscard]] yyjson_mut_val* makeString(yyjson_mut_doc* doc, std::string_view text) {
    return yyjson_mut_strncpy(doc, text.data(), text.size());
}

[[nodiscard]] yyjson_mut_val* makeStringList(yyjson_mut_doc* doc, const StringList& list) {
    yyjson_mut_val* array = yyjson_mut_arr(doc);
    for (const std::string& item : list) {
        yyjson_mut_arr_append(array, makeString(doc, item));
    }
    return array;
}

[[nodiscard]] yyjson_mut_val* makeValue(yyjson_mut_doc* doc, const Value& value) {
    if (const auto* flag = std::get_if<bool>(&value)) {
        return yyjson_mut_bool(doc, *flag);
    }
    if (const auto* number = std::get_if<std::int64_t>(&value)) {
        return yyjson_mut_sint(doc, *number);
    }
    if (const auto* real = std::get_if<double>(&value)) {
        return yyjson_mut_real(doc, *real);
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        return makeString(doc, *text);
    }
    if (const auto* list = std::get_if<StringList>(&value)) {
        return makeStringList(doc, *list);
    }
    yyjson_mut_val* array = yyjson_mut_arr(doc);
    for (const Record& record : std::get<RecordList>(value)) {
        yyjson_mut_val* object = yyjson_mut_obj(doc);
        for (const RecordField& field : record) {
            yyjson_mut_val* fieldValue = std::visit(
                [&](const auto& alternative) -> yyjson_mut_val* {
                    using T = std::decay_t<decltype(alternative)>;
                    if constexpr (std::is_same_v<T, bool>) {
                        return yyjson_mut_bool(doc, alternative);
                    } else if constexpr (std::is_same_v<T, std::int64_t>) {
                        return yyjson_mut_sint(doc, alternative);
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        return makeString(doc, alternative);
                    } else {
                        return makeStringList(doc, alternative);
                    }
                },
                field.value);
            yyjson_mut_obj_add(object, makeString(doc, field.name), fieldValue);
        }
        yyjson_mut_arr_append(array, object);
    }
    return array;
}

[[nodiscard]] Result<std::string> write(yyjson_mut_doc* doc, yyjson_mut_val* root) {
    yyjson_mut_doc_set_root(doc, root);
    std::size_t length = 0;
    yyjson_write_err error{};
    char* text = yyjson_mut_write_opts(doc, YYJSON_WRITE_NOFLAG, nullptr, &length, &error);
    if (text == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("cannot be written as JSON ({})", error.msg != nullptr ? error.msg : "unknown"));
    }
    std::string result(text, length);
    std::free(text); // NOLINT(cppcoreguidelines-no-malloc): yyjson's default allocator is malloc.
    return result;
}

} // namespace

std::pair<std::uint32_t, std::uint32_t> lineColumn(std::string_view text, std::size_t offset) noexcept {
    std::uint32_t line = 1;
    std::uint32_t column = 1;
    const std::size_t end = std::min(offset, text.size());
    for (std::size_t i = 0; i < end; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte == '\n') {
            ++line;
            column = 1;
        } else if ((byte & 0xC0U) != 0x80U) { // Count code points: skip continuation bytes.
            ++column;
        }
    }
    return {line, column};
}

Result<ParsedObject> parseObject(std::string_view text) {
    if (!io::isValidUtf8(text)) {
        return corrupt(text, io::validUtf8Prefix(text), "the file is not valid UTF-8");
    }
    const std::size_t start = text.starts_with(kBom) ? kBom.size() : 0;
    ParsedObject parsed;
    Scanner probe(text, start);
    probe.skipTrivia();
    if (probe.atEnd()) {
        return parsed; // Nothing but whitespace and comments: an empty document.
    }
    const std::string_view body = text.substr(start);
    yyjson_read_err error{};
    const DocPtr doc = read(body, error);
    if (!doc) {
        return corrupt(text, start + error.pos, error.msg != nullptr ? error.msg : "invalid JSON");
    }
    yyjson_val* root = yyjson_doc_get_root(doc.get());
    if (!yyjson_is_obj(root)) {
        return corrupt(text, probe.pos(), "a settings file must hold one JSON object");
    }
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(root, &iter);
    while (yyjson_val* key = yyjson_obj_iter_next(&iter)) {
        parsed.keys.emplace_back(stringOf(key));
    }
    parsed.hasObject = true;
    parsed.open = probe.pos();
    auto spans = scanMembers(text, parsed.open, parsed.close);
    if (!spans) {
        return std::unexpected(spans.error());
    }
    if (spans->size() != parsed.keys.size()) {
        return fail(ErrorCode::Internal, "settings scanner and parser disagree on the member count");
    }
    parsed.spans = std::move(*spans);
    return parsed;
}

Result<void> checkSingleValue(std::string_view json) {
    yyjson_read_err error{};
    if (!read(json, error)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("not one JSON value ({})", error.msg != nullptr ? error.msg : "invalid"));
    }
    return {};
}

Result<Value> decodeValue(std::string_view json, const Schema& schema) {
    yyjson_read_err error{};
    const DocPtr doc = read(json, error);
    if (!doc) {
        return fail(ErrorCode::InvalidArgument, "is not valid JSON");
    }
    yyjson_val* value = yyjson_doc_get_root(doc.get());
    switch (schema.type) {
    case ValueType::Bool:
        if (!yyjson_is_bool(value)) {
            return fail(ErrorCode::InvalidArgument, "must be true or false");
        }
        return Value(yyjson_get_bool(value));
    case ValueType::Integer: {
        auto number = integerOf(value, "the value");
        if (!number) {
            return fail(ErrorCode::InvalidArgument, "must be an integer");
        }
        return Value(*number);
    }
    case ValueType::Number:
        if (!yyjson_is_num(value)) {
            return fail(ErrorCode::InvalidArgument, "must be a number");
        }
        return Value(yyjson_get_num(value));
    case ValueType::String:
    case ValueType::Enum:
        if (!yyjson_is_str(value)) {
            return fail(ErrorCode::InvalidArgument, "must be a string");
        }
        return Value(std::string(stringOf(value)));
    case ValueType::StringList: {
        auto list = stringList(value, "the value");
        if (!list) {
            return fail(ErrorCode::InvalidArgument, "must be an array of strings");
        }
        return Value(std::move(*list));
    }
    case ValueType::RecordList: {
        if (!yyjson_is_arr(value)) {
            return fail(ErrorCode::InvalidArgument, "must be an array of objects");
        }
        RecordList records;
        yyjson_arr_iter iter;
        yyjson_arr_iter_init(value, &iter);
        while (yyjson_val* item = yyjson_arr_iter_next(&iter)) {
            auto record = decodeRecord(item, schema, records.size());
            if (!record) {
                return std::unexpected(record.error());
            }
            records.push_back(std::move(*record));
        }
        return Value(std::move(records));
    }
    }
    return fail(ErrorCode::Internal, "unknown value type");
}

Result<std::string> encodeValue(const Value& value) {
    const MutDocPtr doc(yyjson_mut_doc_new(nullptr));
    if (!doc) {
        return fail(ErrorCode::Internal, "out of memory");
    }
    return write(doc.get(), makeValue(doc.get(), value));
}

Result<std::string> encodeString(std::string_view text) {
    const MutDocPtr doc(yyjson_mut_doc_new(nullptr));
    if (!doc) {
        return fail(ErrorCode::Internal, "out of memory");
    }
    return write(doc.get(), makeString(doc.get(), text));
}

} // namespace frost::settings::detail
