// SPDX-License-Identifier: Apache-2.0
//
// Implementation of frost::Error (see frost/core/error.hpp).

#include "frost/core/error.hpp"

#include <format>
#include <string_view>
#include <utility>

namespace frost {

std::string_view toString(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Unknown: return "Unknown";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::AlreadyExists: return "AlreadyExists";
    case ErrorCode::PermissionDenied: return "PermissionDenied";
    case ErrorCode::IoError: return "IoError";
    case ErrorCode::Corrupt: return "Corrupt";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::OutOfRange: return "OutOfRange";
    case ErrorCode::Cancelled: return "Cancelled";
    case ErrorCode::Timeout: return "Timeout";
    case ErrorCode::Busy: return "Busy";
    case ErrorCode::Internal: return "Internal";
    }
    // Values outside the enumeration can arrive through IPC; they must still print.
    return "ErrorCode(?)";
}

Error::Error(ErrorCode code, std::string message, std::source_location where)
    : m_code(code), m_message(std::move(message)), m_where(where) {}

Error& Error::withContext(std::string note) & {
    m_context.push_back(std::move(note));
    return *this;
}

Error&& Error::withContext(std::string note) && {
    m_context.push_back(std::move(note));
    return std::move(*this);
}

std::string Error::describe() const {
    std::string out(toString(m_code));
    if (!m_message.empty()) {
        out += ": ";
        out += m_message;
    }
    for (const auto& note : m_context) {
        out += " [";
        out += note;
        out += ']';
    }
    // Only the file name, not the full build path, keeps logs readable and machine-independent.
    std::string_view file = m_where.file_name();
    if (const auto slash = file.find_last_of("/\\"); slash != std::string_view::npos) {
        file.remove_prefix(slash + 1);
    }
    out += std::format(" ({}:{})", file, m_where.line());
    return out;
}

} // namespace frost
