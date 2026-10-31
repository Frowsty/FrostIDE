// SPDX-License-Identifier: Apache-2.0
//
// frost::Error and frost::Result<T>: the error model for every fallible API
// (PLAN.md section 6.5). Functions that can fail return
// [[nodiscard]] Result<T> = std::expected<T, Error>; exceptions never cross
// module boundaries.
//
// An Error carries a machine-readable code, a human-readable message (English,
// for logs; user-facing text is produced by the UI from the code and context),
// the source location that created it and an optional chain of context notes
// added as the error propagates ("while loading foo.vcxproj").
#pragma once

#include <cstdint>
#include <expected>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace frost {

// Broad error categories; callers branch on these, never on message text.
enum class ErrorCode : std::uint16_t {
    Unknown = 0,
    InvalidArgument,  // The caller passed a value outside the documented domain.
    NotFound,         // A file, key, symbol or resource does not exist.
    AlreadyExists,    // Creation failed because the target exists.
    PermissionDenied, // The OS refused access.
    IoError,          // Read/write/OS failure not covered by a more specific code.
    Corrupt,          // Data failed validation (bad checksum, malformed file).
    Unsupported,      // The operation or format version is not supported.
    OutOfRange,       // An index, offset or size exceeded its bounds.
    Cancelled,        // The operation observed a stop request.
    Timeout,          // A deadline expired.
    Busy,             // The resource is temporarily unavailable; retry may succeed.
    Internal,         // A FrostIDE bug: an invariant did not hold.
};

// Returns the enumerator name ("NotFound"), or "ErrorCode(<n>)" for unknown values.
[[nodiscard]] std::string_view toString(ErrorCode code) noexcept;

class [[nodiscard]] Error {
public:
    // Creates an error; `where` defaults to the caller's location.
    explicit Error(ErrorCode code, std::string message = {},
                   std::source_location where = std::source_location::current());

    [[nodiscard]] ErrorCode code() const noexcept { return m_code; }
    [[nodiscard]] const std::string& message() const noexcept { return m_message; }
    [[nodiscard]] const std::source_location& where() const noexcept { return m_where; }

    // Context notes, innermost first, added by withContext() as the error propagates.
    [[nodiscard]] const std::vector<std::string>& context() const noexcept { return m_context; }

    // Appends a context note and returns the error, for `return std::unexpected(e.withContext(...))`.
    Error& withContext(std::string note) &;
    Error&& withContext(std::string note) &&;

    // One-line description for logs: "NotFound: no such file [while loading x] (error.cpp:12)".
    [[nodiscard]] std::string describe() const;

    friend bool operator==(const Error& a, const Error& b) noexcept {
        return a.m_code == b.m_code && a.m_message == b.m_message;
    }

private:
    ErrorCode m_code;
    std::string m_message;
    std::source_location m_where;
    std::vector<std::string> m_context;
};

// The result type of every fallible FrostIDE API.
template <class T>
using Result = std::expected<T, Error>;

// Shorthand for the error branch: `return frost::fail(ErrorCode::NotFound, "no project file");`
[[nodiscard]] inline std::unexpected<Error> fail(ErrorCode code, std::string message = {},
                                                 std::source_location where = std::source_location::current()) {
    return std::unexpected<Error>(Error(code, std::move(message), where));
}

} // namespace frost

// Formats an Error with describe(), so `std::format("{}", error)` works in logs.
template <>
struct std::formatter<frost::Error> : std::formatter<std::string_view> {
    auto format(const frost::Error& error, std::format_context& ctx) const {
        return std::formatter<std::string_view>::format(error.describe(), ctx);
    }
};
