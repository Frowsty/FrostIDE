// SPDX-License-Identifier: Apache-2.0
//
// Setting values and their schemas (PLAN.md section 3.6, ADR 0021): the value types a registry key
// can have, the schema each key declares (ranges, enum options, record fields, a widget hint for the
// M15 Settings dialog), schema checks, and helpers that build schemas for registry tables.
/*
 * Value model. JSON scope files hold one value per key; the registry's schema says how it is read:
 *   ValueType   JSON                    C++ alternative of Value
 *   Bool        true / false            bool
 *   Integer     an integer              std::int64_t (a number with a fraction is refused)
 *   Number      any number              double
 *   String      a string                std::string (Schema::options are suggestions only)
 *   Enum        a string from options   std::string (Schema::options is the closed set)
 *   StringList  an array of strings     StringList
 *   RecordList  an array of objects     RecordList (fields from Schema::fields; unknown fields refused)
 * Everything here is a plain value: copyable, comparable, thread-compatible.
 */
#pragma once

#include "frost/core/error.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace frost::settings {

enum class ValueType : std::uint8_t { Bool, Integer, Number, String, Enum, StringList, RecordList };

// "bool", "integer", "number", "string", "enum", "string list", "record list".
[[nodiscard]] std::string_view toString(ValueType type) noexcept;

using StringList = std::vector<std::string>;

// The value of one field of a record (RecordList entries).
using FieldValue = std::variant<bool, std::int64_t, std::string, StringList>;

struct RecordField {
    std::string name;
    FieldValue value;
    friend bool operator==(const RecordField&, const RecordField&) = default;
};

// One record: its fields sorted by name, each name once.
using Record = std::vector<RecordField>;
using RecordList = std::vector<Record>;

// A setting's value; the alternative is fixed by the key's ValueType (table above).
using Value = std::variant<bool, std::int64_t, double, std::string, StringList, RecordList>;

// The C++ types a typed key (keys.hpp) may have.
template<class T>
concept SettingType = std::same_as<T, bool> || std::same_as<T, std::int64_t> || std::same_as<T, double> ||
                      std::same_as<T, std::string> || std::same_as<T, StringList> || std::same_as<T, RecordList>;

// Field types of RecordList records.
enum class FieldType : std::uint8_t { Bool, Integer, String, StringList };

struct FieldSpec {
    std::string name; // The JSON member name ("action").
    FieldType type = FieldType::String;
    bool required = true;    // A record without a required field is refused.
    std::string description; // English, for the dialog.
};

// One allowed value (Enum) or suggestion (String).
struct EnumOption {
    std::string value;       // As written in JSON ("crlf").
    std::string label;       // Display text ("Windows (CRLF)").
    std::string description; // Optional longer text.
};

// What the M15 dialog should offer for a String (or StringList item); validation ignores it.
enum class StringHint : std::uint8_t {
    Plain,
    FontFamily, // A font family name; the dialog lists installed fonts.
    ThemeId,    // A theme id or "sync-with-os".
    Encoding,   // An encoding name as frost::text accepts ("UTF-8", "windows-1252").
    Executable, // An absolute path of a program (an `executes` key).
    Path,       // An absolute file or directory path.
    Command,    // A command line or program argument (an `executes` key).
    Shortcut,   // A keyboard shortcut in frost::input's strict syntax ("Ctrl+K, Ctrl+C").
    ActionId,   // An action id ("editor.duplicateLine").
};

/*
 * A key's schema. Only the members that apply to `type` are used:
 *   Integer       minInteger, maxInteger (inclusive)
 *   Number        minNumber, maxNumber (inclusive; NaN and infinities never pass)
 *   String, Enum  maxLength (bytes), allowEmpty, options, hint
 *   StringList    maxItems, maxLength and allowEmpty per item, hint
 *   RecordList    maxItems, fields (string fields use maxLength)
 */
struct Schema {
    ValueType type = ValueType::Bool;
    std::int64_t minInteger = std::numeric_limits<std::int64_t>::min();
    std::int64_t maxInteger = std::numeric_limits<std::int64_t>::max();
    double minNumber = std::numeric_limits<double>::lowest();
    double maxNumber = std::numeric_limits<double>::max();
    std::size_t maxLength = 4096;
    std::size_t maxItems = 1024;
    bool allowEmpty = true;
    std::vector<EnumOption> options;
    std::vector<FieldSpec> fields;
    StringHint hint = StringHint::Plain;
};

// True when `value` holds the alternative `type` uses. O(1).
[[nodiscard]] bool holdsType(const Value& value, ValueType type) noexcept;

/*
 * Checks `value` against `schema`: the alternative, ranges, lengths, enum membership, record fields
 * (sorted, unique, known, required ones present, of the declared type). Errors: InvalidArgument
 * with a short English reason ("must be between 1 and 32"), suitable after "key: ". O(size).
 */
[[nodiscard]] Result<void> validateValue(const Schema& schema, const Value& value);

// Schema builders for registry tables.
[[nodiscard]] Schema boolSchema();
[[nodiscard]] Schema integerSchema(std::int64_t min, std::int64_t max);
[[nodiscard]] Schema numberSchema(double min, double max);
[[nodiscard]] Schema stringSchema(std::size_t maxLength = 4096, StringHint hint = StringHint::Plain,
                                  std::vector<EnumOption> suggestions = {});
[[nodiscard]] Schema enumSchema(std::vector<EnumOption> options);
[[nodiscard]] Schema stringListSchema(std::size_t maxItems = 1024, std::size_t maxLength = 4096,
                                      StringHint hint = StringHint::Plain);
[[nodiscard]] Schema recordListSchema(std::vector<FieldSpec> fields, std::size_t maxItems = 4096);

// Builds a Record from fields in any order, sorting them by name. Duplicate names are kept (and
// fail validateValue). O(n log n).
[[nodiscard]] Record makeRecord(std::vector<RecordField> fields);

// The value of field `name` of `record`, or nullptr. O(log n).
[[nodiscard]] const FieldValue* findField(const Record& record, std::string_view name) noexcept;

} // namespace frost::settings
