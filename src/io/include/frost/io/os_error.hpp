// SPDX-License-Identifier: Apache-2.0
//
// Mapping of OS error numbers (Win32 GetLastError() values and POSIX errno) to
// frost::ErrorCode, and construction of frost::Error values that keep the OS
// context for logs.
//
/*
 * Callers branch on the ErrorCode; the message keeps the detail a human needs:
 *
 *   "<operation> '<path>': <OS message> (win32 error 2)"
 *   "<operation> '<path>': <OS message> (errno 2)"
 *
 * The OS message is English where the system provides it (FormatMessageW is
 * asked for en-US first, then the user's language), matching the convention
 * that Error messages are English log text (frost/core/error.hpp).
 *
 * Mapping, by intent rather than by the letter of each OS code:
 *   NotFound          file/path/drive/network name missing; ENOENT, ENOTDIR
 *   AlreadyExists     ERROR_FILE_EXISTS, ERROR_ALREADY_EXISTS; EEXIST
 *   PermissionDenied  access denied, write protected, privilege missing; EACCES, EPERM, EROFS
 *   Busy              sharing/lock violations, busy resources; EBUSY, ETXTBSY, EAGAIN
 *   InvalidArgument   invalid names, name too long, "is a directory"; EINVAL, EISDIR, ENAMETOOLONG
 *   Unsupported       ERROR_NOT_SUPPORTED, ERROR_INVALID_FUNCTION; ENOSYS, ENOTSUP
 *   Timeout           WAIT_TIMEOUT, ERROR_TIMEOUT; ETIMEDOUT
 *   Cancelled         ERROR_OPERATION_ABORTED, ERROR_CANCELLED; ECANCELED
 *   IoError           everything else (disk full, broken pipe, directory not empty, ...)
 *
 * ENOTDIR maps to NotFound because it almost always means "a component of
 * the path prefix is not a directory". Operations where it means "the target
 * itself is not a directory" (listing a file) refine it to InvalidArgument.
 *
 * All functions are thread-safe.
 */
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <source_location>
#include <string_view>

namespace frost::io {

// Maps a POSIX errno value (as defined by <cerrno> on this platform). Never fails.
[[nodiscard]] ErrorCode errorCodeFromErrno(int errnoValue) noexcept;

// An Error for a failed POSIX call: code from errorCodeFromErrno, message as above.
// `path` may be empty when the operation has none.
[[nodiscard]] Error errnoError(int errnoValue, std::string_view operation, std::string_view path = {},
                               std::source_location where = std::source_location::current());

#ifdef _WIN32
// Maps a Win32 error code (GetLastError()). Never fails.
[[nodiscard]] ErrorCode errorCodeFromWin32(std::uint32_t win32Error) noexcept;

// An Error for a failed Win32 call: code from errorCodeFromWin32, message as above.
[[nodiscard]] Error win32Error(std::uint32_t win32Error, std::string_view operation, std::string_view path = {},
                               std::source_location where = std::source_location::current());

// win32Error(GetLastError(), ...): call it immediately after the failing API.
[[nodiscard]] Error lastWin32Error(std::string_view operation, std::string_view path = {},
                                   std::source_location where = std::source_location::current());
#endif

} // namespace frost::io
