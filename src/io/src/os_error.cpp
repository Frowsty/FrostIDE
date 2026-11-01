// SPDX-License-Identifier: Apache-2.0
//
// OS error mapping and messages (see frost/io/os_error.hpp).

#include "frost/io/os_error.hpp"

#include <cerrno>
#include <format>
#include <string>
#include <system_error>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#endif

namespace frost::io {
namespace {

// "<operation> '<path>': <detail>" or "<operation>: <detail>".
std::string formatMessage(std::string_view operation, std::string_view path, std::string_view detail) {
    if (path.empty()) {
        return std::format("{}: {}", operation, detail);
    }
    return std::format("{} '{}': {}", operation, path, detail);
}

} // namespace

ErrorCode errorCodeFromErrno(int errnoValue) noexcept {
    switch (errnoValue) {
    case ENOENT:
    case ENOTDIR:
    case ENXIO:
    case ESRCH: return ErrorCode::NotFound;
    case EEXIST: return ErrorCode::AlreadyExists;
    case EACCES:
    case EPERM:
    case EROFS: return ErrorCode::PermissionDenied;
    case EBUSY:
    case ETXTBSY:
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        return ErrorCode::Busy;
    case EINVAL:
    case EISDIR:
    case ENAMETOOLONG:
    case ENOEXEC: return ErrorCode::InvalidArgument;
    case ENOSYS:
    case ENOTSUP:
#if EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP:
#endif
    case EXDEV: return ErrorCode::Unsupported;
    case ETIMEDOUT: return ErrorCode::Timeout;
    case ECANCELED: return ErrorCode::Cancelled;
    case EBADF:
    case EFAULT: return ErrorCode::Internal; // A bad descriptor or pointer is a FrostIDE bug.
    default: return ErrorCode::IoError;
    }
}

Error errnoError(int errnoValue, std::string_view operation, std::string_view path, std::source_location where) {
    // generic_category().message is thread-safe (unlike strerror). On glibc it follows LC_MESSAGES, which Qt
    // sets from the environment, so it is English only in a "C"-locale process (see os_error.hpp).
    const std::string detail = std::format("{} (errno {})", std::generic_category().message(errnoValue), errnoValue);
    return Error(errorCodeFromErrno(errnoValue), formatMessage(operation, path, detail), where);
}

#ifdef _WIN32

ErrorCode errorCodeFromWin32(std::uint32_t win32Error) noexcept {
    switch (win32Error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_DEV_NOT_EXIST:
    case ERROR_MOD_NOT_FOUND:
    case ERROR_NOT_FOUND: return ErrorCode::NotFound;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return ErrorCode::AlreadyExists;
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
    case ERROR_NETWORK_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_ELEVATION_REQUIRED:
    case ERROR_VIRUS_INFECTED:
    case ERROR_VIRUS_DELETED: return ErrorCode::PermissionDenied;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_DRIVE_LOCKED:
    case ERROR_BUSY:
    case ERROR_PIPE_BUSY:
    case ERROR_USER_MAPPED_FILE: return ErrorCode::Busy;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_DIRECTORY:
    case ERROR_INVALID_PARAMETER:
    case ERROR_BAD_EXE_FORMAT: return ErrorCode::InvalidArgument;
    case ERROR_INVALID_FUNCTION:
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
    case ERROR_NOT_SAME_DEVICE: return ErrorCode::Unsupported;
    case WAIT_TIMEOUT:
    case ERROR_SEM_TIMEOUT:
    case ERROR_TIMEOUT: return ErrorCode::Timeout;
    case ERROR_OPERATION_ABORTED:
    case ERROR_CANCELLED: return ErrorCode::Cancelled;
    case ERROR_INVALID_HANDLE: return ErrorCode::Internal; // A bad handle is a FrostIDE bug.
    default: return ErrorCode::IoError;
    }
}

namespace {

// The system's text for `code` without the trailing period and line break, or "" if it has none.
std::string systemMessage(std::uint32_t code) {
    wchar_t buffer[512];
    const DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD length = FormatMessageW(flags, nullptr, code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), buffer,
                                  static_cast<DWORD>(std::size(buffer)), nullptr);
    if (length == 0) {
        // No English resources installed: fall back to the user's language.
        length = FormatMessageW(flags, nullptr, code, 0, buffer, static_cast<DWORD>(std::size(buffer)), nullptr);
    }
    while (length > 0 && (buffer[length - 1] == L'\r' || buffer[length - 1] == L'\n' || buffer[length - 1] == L'.' ||
                          buffer[length - 1] == L' ')) {
        --length;
    }
    auto text = fromWide(std::wstring_view(buffer, length));
    return text ? std::move(*text) : std::string();
}

} // namespace

Error win32Error(std::uint32_t code, std::string_view operation, std::string_view path, std::source_location where) {
    std::string text = systemMessage(code);
    const std::string detail =
        text.empty() ? std::format("win32 error {}", code) : std::format("{} (win32 error {})", text, code);
    return Error(errorCodeFromWin32(code), formatMessage(operation, path, detail), where);
}

Error lastWin32Error(std::string_view operation, std::string_view path, std::source_location where) {
    return win32Error(GetLastError(), operation, path, where);
}

#endif

} // namespace frost::io
