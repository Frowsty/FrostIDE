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
 * The OS message aims at the convention that Error messages are English log
 * text (frost/core/error.hpp). On Windows FormatMessageW is asked for en-US
 * first, then the user's language. On POSIX the text is the C library's
 * strerror text, which glibc translates for LC_MESSAGES; Qt sets that from the
 * environment, so it is English only in a process whose locale is "C" (the
 * unit tests, for example).
 *
 * Mapping, by intent rather than by the letter of each OS code (Win32 codes
 * first, then errno values):
 *   NotFound          file, path, drive, device, network name, module or process missing:
 *                     ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND, ERROR_INVALID_DRIVE,
 *                     ERROR_BAD_NETPATH, ERROR_BAD_NET_NAME, ERROR_DEV_NOT_EXIST,
 *                     ERROR_MOD_NOT_FOUND, ERROR_NOT_FOUND; ENOENT, ENOTDIR, ENXIO, ESRCH
 *   AlreadyExists     ERROR_FILE_EXISTS, ERROR_ALREADY_EXISTS; EEXIST
 *   PermissionDenied  access denied, write protected, privilege or elevation missing, file
 *                     quarantined by antivirus: ERROR_ACCESS_DENIED, ERROR_WRITE_PROTECT,
 *                     ERROR_NETWORK_ACCESS_DENIED, ERROR_PRIVILEGE_NOT_HELD,
 *                     ERROR_ELEVATION_REQUIRED, ERROR_VIRUS_INFECTED, ERROR_VIRUS_DELETED;
 *                     EACCES, EPERM, EROFS
 *   Busy              sharing/lock violations, busy resources: ERROR_SHARING_VIOLATION,
 *                     ERROR_LOCK_VIOLATION, ERROR_DRIVE_LOCKED, ERROR_BUSY, ERROR_PIPE_BUSY,
 *                     ERROR_USER_MAPPED_FILE; EBUSY, ETXTBSY, EAGAIN (EWOULDBLOCK)
 *   InvalidArgument   invalid names or parameters, name too long, "is a directory", bad
 *                     executable format: ERROR_INVALID_NAME, ERROR_BAD_PATHNAME,
 *                     ERROR_FILENAME_EXCED_RANGE, ERROR_DIRECTORY, ERROR_INVALID_PARAMETER,
 *                     ERROR_BAD_EXE_FORMAT; EINVAL, EISDIR, ENAMETOOLONG, ENOEXEC
 *   Unsupported       ERROR_NOT_SUPPORTED, ERROR_INVALID_FUNCTION, ERROR_CALL_NOT_IMPLEMENTED,
 *                     ERROR_NOT_SAME_DEVICE; ENOSYS, ENOTSUP (EOPNOTSUPP), EXDEV
 *                     (a rename across devices is Unsupported, not IoError)
 *   Timeout           WAIT_TIMEOUT, ERROR_TIMEOUT, ERROR_SEM_TIMEOUT; ETIMEDOUT
 *   Cancelled         ERROR_OPERATION_ABORTED, ERROR_CANCELLED; ECANCELED
 *   Internal          a bad handle, descriptor or pointer (a FrostIDE bug):
 *                     ERROR_INVALID_HANDLE; EBADF, EFAULT
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
