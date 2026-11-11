// SPDX-License-Identifier: Apache-2.0
//
// frost::rpc::Json: the owning JSON value the protocol clients speak in (LSP requests, results and
// notifications; DAP requests, responses and events), with a parser over yyjson and a compact
// writer. yyjson stays private to frost::rpc (ADR 0036, moved here by ADR 0050), so layers above
// see only this type. frost::lang re-exports it as frost::lang::Json.
/*
 * Model. A value is null, a boolean, an integer (int64 or uint64), a double, a UTF-8 string, an array or an
 * object. Objects keep their members in insertion (or document) order; set() replaces an existing
 * member in place, so keys are always unique. A parsed object with a duplicate key keeps the
 * first member's position and the last member's value.
 *
 * Numbers. Every integer that fits int64 or uint64 is Integer and round-trips exactly: values above
 * INT64_MAX (FrostEngine's 64-bit asset handles and entity ids, FEC-1) are kept as unsigned
 * integers, never as doubles, which hold only 53 bits. asInteger() returns int64 values only;
 * asUnsigned() returns every non-negative integer. Numbers with a fraction or an exponent, and
 * integers beyond uint64, are Double. asInteger() and asUnsigned() also accept a Double that holds
 * an exact integer in their range.
 *
 * Thread safety: a value type; distinct objects may be used concurrently, one object is not
 * synchronized. Copying is O(size of the tree).
 */
#pragma once

#include "frost/core/error.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace frost::rpc {

class Json {
public:
    enum class Kind : std::uint8_t { Null, Bool, Integer, Double, String, Array, Object };

    // Null.
    Json() noexcept = default;
    // NOLINTNEXTLINE(google-explicit-constructor): Scalar conversion is the Json set()/array() value contract.
    Json(std::nullptr_t) noexcept {}
    // NOLINTNEXTLINE(google-explicit-constructor): Scalar conversion is the Json set()/array() value contract.
    Json(bool value) noexcept : m_kind(Kind::Bool), m_bool(value) {}
    // Any integer type; unsigned values above INT64_MAX stay unsigned (asUnsigned()).
    template<std::integral I>
        requires(!std::same_as<I, bool>)
    // NOLINTNEXTLINE(google-explicit-constructor): Scalar conversion is the Json set()/array() value contract.
    Json(I value) noexcept : m_kind(Kind::Integer), m_integer(static_cast<std::int64_t>(value)) {
        if constexpr (std::is_unsigned_v<I>) {
            m_aboveInt64 = static_cast<std::uint64_t>(value) > static_cast<std::uint64_t>(INT64_MAX);
        }
    }
    // NOLINTNEXTLINE(google-explicit-constructor): Scalar conversion is the Json set()/array() value contract.
    Json(double value) noexcept : m_kind(Kind::Double), m_double(value) {}
    // NOLINTNEXTLINE(google-explicit-constructor): Scalar conversion is the Json set()/array() value contract.
    Json(std::string value) : m_kind(Kind::String), m_string(std::move(value)) {}
    // NOLINTNEXTLINE(google-explicit-constructor): Scalar conversion is the Json set()/array() value contract.
    Json(std::string_view value) : m_kind(Kind::String), m_string(value) {}
    // NOLINTNEXTLINE(google-explicit-constructor): Scalar conversion is the Json set()/array() value contract.
    Json(const char* value) : m_kind(Kind::String), m_string(value) {}

    // An empty array, or one holding `items`.
    [[nodiscard]] static Json array(std::vector<Json> items = {});
    // An empty object.
    [[nodiscard]] static Json object();

    [[nodiscard]] Kind kind() const noexcept { return m_kind; }
    [[nodiscard]] bool isNull() const noexcept { return m_kind == Kind::Null; }
    [[nodiscard]] bool isBool() const noexcept { return m_kind == Kind::Bool; }
    [[nodiscard]] bool isNumber() const noexcept { return m_kind == Kind::Integer || m_kind == Kind::Double; }
    [[nodiscard]] bool isString() const noexcept { return m_kind == Kind::String; }
    [[nodiscard]] bool isArray() const noexcept { return m_kind == Kind::Array; }
    [[nodiscard]] bool isObject() const noexcept { return m_kind == Kind::Object; }

    // Scalars; nullopt (nullptr) when the value has another kind. O(1).
    [[nodiscard]] std::optional<bool> asBool() const noexcept;
    // An Integer within int64, or a Double holding an exact int64 value.
    [[nodiscard]] std::optional<std::int64_t> asInteger() const noexcept;
    // A non-negative Integer (all of uint64), or a Double holding an exact value in [0, 2^64).
    [[nodiscard]] std::optional<std::uint64_t> asUnsigned() const noexcept;
    // Any number.
    [[nodiscard]] std::optional<double> asDouble() const noexcept;
    [[nodiscard]] const std::string* asString() const noexcept;

    // Array items or object values, in order (empty for scalars). O(1).
    [[nodiscard]] const std::vector<Json>& items() const noexcept { return m_items; }
    // Object keys, parallel to items() (empty for other kinds). O(1).
    [[nodiscard]] const std::vector<std::string>& keys() const noexcept { return m_keys; }
    // items().size().
    [[nodiscard]] std::size_t size() const noexcept { return m_items.size(); }

    // Appends to an array; a Null value becomes an empty array first. Precondition (FROST_PRE):
    // isArray() or isNull(). Amortised O(1).
    Json& push(Json value) &;
    Json&& push(Json value) &&;

    // The first member named `key`, or nullptr (also for non-objects). O(members).
    [[nodiscard]] const Json* find(std::string_view key) const noexcept;
    [[nodiscard]] Json* find(std::string_view key) noexcept;
    // Sets member `key` (replacing an existing one in place); a Null value becomes an empty object
    // first. Precondition (FROST_PRE): isObject() or isNull(). O(members).
    Json& set(std::string key, Json value) &;
    Json&& set(std::string key, Json value) &&;

    // Member helpers that tolerate missing members and wrong kinds.
    [[nodiscard]] std::string_view stringAt(std::string_view key, std::string_view fallback = {}) const noexcept;
    [[nodiscard]] std::optional<std::int64_t> integerAt(std::string_view key) const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> unsignedAt(std::string_view key) const noexcept;
    [[nodiscard]] std::optional<bool> boolAt(std::string_view key) const noexcept;

    // Compact JSON text (no spaces). Strings are written as UTF-8 with the mandatory escapes;
    // NaN and infinities are written as null (JSON has no spelling for them). O(size).
    [[nodiscard]] std::string dump() const;

    // Deep equality; an Integer equals a Double of the same value (compared as doubles), two
    // Integers compare exactly. Object member order matters.
    friend bool operator==(const Json& a, const Json& b) noexcept;

private:
    void dumpTo(std::string& out) const;

    Kind m_kind = Kind::Null;
    bool m_bool = false;
    // An Integer above INT64_MAX: m_integer holds the uint64's bits.
    bool m_aboveInt64 = false;
    std::int64_t m_integer = 0;
    double m_double = 0.0;
    std::string m_string;
    std::vector<Json> m_items;
    std::vector<std::string> m_keys;
};

// Parses one JSON text (RFC 8259; surrounding whitespace allowed, nothing else after the value).
// Nesting deeper than 256 levels is refused. Errors: Corrupt, with yyjson's reason and the byte
// position. O(size).
[[nodiscard]] Result<Json> parseJson(std::string_view text);

} // namespace frost::rpc
