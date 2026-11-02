// SPDX-License-Identifier: Apache-2.0
//
// Schema checks and schema builders for setting values (value.hpp).

#include "frost/settings/value.hpp"

#include "frost/io/utf.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace frost::settings {

namespace {

[[nodiscard]] Result<void> invalid(std::string message) { return fail(ErrorCode::InvalidArgument, std::move(message)); }

[[nodiscard]] Result<void> checkString(const Schema& schema, const std::string& text) {
    if (!io::isValidUtf8(text)) {
        return invalid("must be valid UTF-8");
    }
    if (text.size() > schema.maxLength) {
        return invalid(std::format("must be at most {} bytes long", schema.maxLength));
    }
    if (text.empty() && !schema.allowEmpty) {
        return invalid("must not be empty");
    }
    if (text.find('\0') != std::string::npos) {
        return invalid("must not contain NUL characters");
    }
    return {};
}

[[nodiscard]] bool fieldHolds(const FieldValue& value, FieldType type) noexcept {
    switch (type) {
    case FieldType::Bool: return std::holds_alternative<bool>(value);
    case FieldType::Integer: return std::holds_alternative<std::int64_t>(value);
    case FieldType::String: return std::holds_alternative<std::string>(value);
    case FieldType::StringList: return std::holds_alternative<StringList>(value);
    }
    return false;
}

[[nodiscard]] Result<void> checkRecord(const Schema& schema, const Record& record, std::size_t index) {
    for (std::size_t i = 0; i < record.size(); ++i) {
        const RecordField& field = record[i];
        if (i > 0 && !(record[i - 1].name < field.name)) {
            return invalid(std::format("entry {}: fields must be unique (\"{}\")", index + 1, field.name));
        }
        const auto spec = std::ranges::find(schema.fields, field.name, &FieldSpec::name);
        if (spec == schema.fields.end()) {
            return invalid(std::format("entry {}: unknown field \"{}\"", index + 1, field.name));
        }
        if (!fieldHolds(field.value, spec->type)) {
            return invalid(std::format("entry {}: field \"{}\" has the wrong type", index + 1, field.name));
        }
        if (const auto* text = std::get_if<std::string>(&field.value)) {
            if (auto ok = checkString(schema, *text); !ok) {
                return invalid(std::format("entry {}: field \"{}\" {}", index + 1, field.name, ok.error().message()));
            }
        } else if (const auto* list = std::get_if<StringList>(&field.value)) {
            for (const std::string& item : *list) {
                if (auto ok = checkString(schema, item); !ok) {
                    return invalid(
                        std::format("entry {}: field \"{}\": an item {}", index + 1, field.name, ok.error().message()));
                }
            }
        }
    }
    for (const FieldSpec& spec : schema.fields) {
        if (spec.required && findField(record, spec.name) == nullptr) {
            return invalid(std::format("entry {}: field \"{}\" is required", index + 1, spec.name));
        }
    }
    return {};
}

} // namespace

std::string_view toString(ValueType type) noexcept {
    switch (type) {
    case ValueType::Bool: return "bool";
    case ValueType::Integer: return "integer";
    case ValueType::Number: return "number";
    case ValueType::String: return "string";
    case ValueType::Enum: return "enum";
    case ValueType::StringList: return "string list";
    case ValueType::RecordList: return "record list";
    }
    return "ValueType(?)";
}

bool holdsType(const Value& value, ValueType type) noexcept {
    switch (type) {
    case ValueType::Bool: return std::holds_alternative<bool>(value);
    case ValueType::Integer: return std::holds_alternative<std::int64_t>(value);
    case ValueType::Number: return std::holds_alternative<double>(value);
    case ValueType::String:
    case ValueType::Enum: return std::holds_alternative<std::string>(value);
    case ValueType::StringList: return std::holds_alternative<StringList>(value);
    case ValueType::RecordList: return std::holds_alternative<RecordList>(value);
    }
    return false;
}

Result<void> validateValue(const Schema& schema, const Value& value) {
    if (!holdsType(value, schema.type)) {
        return invalid(std::format("must be a {}", toString(schema.type)));
    }
    switch (schema.type) {
    case ValueType::Bool: return {};
    case ValueType::Integer: {
        const std::int64_t number = std::get<std::int64_t>(value);
        if (number < schema.minInteger || number > schema.maxInteger) {
            return invalid(std::format("must be between {} and {}", schema.minInteger, schema.maxInteger));
        }
        return {};
    }
    case ValueType::Number: {
        const double number = std::get<double>(value);
        if (!std::isfinite(number) || number < schema.minNumber || number > schema.maxNumber) {
            return invalid(std::format("must be between {} and {}", schema.minNumber, schema.maxNumber));
        }
        return {};
    }
    case ValueType::String: return checkString(schema, std::get<std::string>(value));
    case ValueType::Enum: {
        const std::string& text = std::get<std::string>(value);
        if (std::ranges::find(schema.options, text, &EnumOption::value) == schema.options.end()) {
            std::string allowed;
            for (const EnumOption& option : schema.options) {
                allowed += allowed.empty() ? "" : ", ";
                allowed += '"' + option.value + '"';
            }
            return invalid(std::format("must be one of {}", allowed));
        }
        return {};
    }
    case ValueType::StringList: {
        const StringList& list = std::get<StringList>(value);
        if (list.size() > schema.maxItems) {
            return invalid(std::format("must have at most {} items", schema.maxItems));
        }
        for (const std::string& item : list) {
            if (auto ok = checkString(schema, item); !ok) {
                return invalid("an item " + ok.error().message());
            }
        }
        return {};
    }
    case ValueType::RecordList: {
        const RecordList& records = std::get<RecordList>(value);
        if (records.size() > schema.maxItems) {
            return invalid(std::format("must have at most {} entries", schema.maxItems));
        }
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (auto ok = checkRecord(schema, records[i], i); !ok) {
                return ok;
            }
        }
        return {};
    }
    }
    return invalid("has an unknown type");
}

Schema boolSchema() {
    Schema schema;
    schema.type = ValueType::Bool;
    return schema;
}

Schema integerSchema(std::int64_t min, std::int64_t max) {
    Schema schema;
    schema.type = ValueType::Integer;
    schema.minInteger = min;
    schema.maxInteger = max;
    return schema;
}

Schema numberSchema(double min, double max) {
    Schema schema;
    schema.type = ValueType::Number;
    schema.minNumber = min;
    schema.maxNumber = max;
    return schema;
}

Schema stringSchema(std::size_t maxLength, StringHint hint, std::vector<EnumOption> suggestions) {
    Schema schema;
    schema.type = ValueType::String;
    schema.maxLength = maxLength;
    schema.options = std::move(suggestions);
    schema.hint = hint;
    return schema;
}

Schema enumSchema(std::vector<EnumOption> options) {
    Schema schema;
    schema.type = ValueType::Enum;
    schema.maxLength = 256;
    schema.allowEmpty = false;
    schema.options = std::move(options);
    return schema;
}

Schema stringListSchema(std::size_t maxItems, std::size_t maxLength, StringHint hint) {
    Schema schema;
    schema.type = ValueType::StringList;
    schema.maxLength = maxLength;
    schema.maxItems = maxItems;
    schema.hint = hint;
    return schema;
}

Schema recordListSchema(std::vector<FieldSpec> fields, std::size_t maxItems) {
    Schema schema;
    schema.type = ValueType::RecordList;
    schema.maxItems = maxItems;
    schema.fields = std::move(fields);
    return schema;
}

Record makeRecord(std::vector<RecordField> fields) {
    std::ranges::stable_sort(fields, {}, &RecordField::name);
    return fields;
}

const FieldValue* findField(const Record& record, std::string_view name) noexcept {
    const auto it = std::ranges::lower_bound(record, name, {},
                                             [](const RecordField& field) { return std::string_view(field.name); });
    return it != record.end() && it->name == name ? &it->value : nullptr;
}

} // namespace frost::settings
