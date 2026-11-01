// SPDX-License-Identifier: Apache-2.0
//
// Tests for the OS error mapping (errno and Win32 codes to frost::ErrorCode) and
// for the context that OS errors carry in their messages.

#include "frost/io/os_error.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <format>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using frost::ErrorCode;

TEST(OsError, ErrnoMapping) {
    const std::pair<int, ErrorCode> cases[] = {
        {ENOENT, ErrorCode::NotFound},
        {ENOTDIR, ErrorCode::NotFound},
        {EEXIST, ErrorCode::AlreadyExists},
        {EACCES, ErrorCode::PermissionDenied},
        {EPERM, ErrorCode::PermissionDenied},
        {EROFS, ErrorCode::PermissionDenied},
        {EBUSY, ErrorCode::Busy},
        {ETXTBSY, ErrorCode::Busy},
        {EAGAIN, ErrorCode::Busy},
        {EINVAL, ErrorCode::InvalidArgument},
        {EISDIR, ErrorCode::InvalidArgument},
        {ENAMETOOLONG, ErrorCode::InvalidArgument},
        {ENOSYS, ErrorCode::Unsupported},
        {EXDEV, ErrorCode::Unsupported},
        {ETIMEDOUT, ErrorCode::Timeout},
        {ECANCELED, ErrorCode::Cancelled},
        {EBADF, ErrorCode::Internal},
        {ENOSPC, ErrorCode::IoError},
        {ENOTEMPTY, ErrorCode::IoError},
        {EIO, ErrorCode::IoError},
        {EPIPE, ErrorCode::IoError},
        {0x7FFF, ErrorCode::IoError},
    };
    for (const auto& [value, code] : cases) {
        EXPECT_EQ(frost::io::errorCodeFromErrno(value), code) << "errno " << value;
    }
}

TEST(OsError, ErrnoErrorKeepsOperationPathAndNumber) {
    const frost::Error error = frost::io::errnoError(ENOENT, "readFile", "/no/such/file");
    EXPECT_EQ(error.code(), ErrorCode::NotFound);
    EXPECT_NE(error.message().find("readFile '/no/such/file': "), std::string::npos) << error.message();
    EXPECT_NE(error.message().find(std::format("(errno {})", ENOENT)), std::string::npos) << error.message();
    // Without a path the quotes are omitted.
    EXPECT_EQ(frost::io::errnoError(EIO, "poll").message().find('\''), std::string::npos);
}

#ifdef _WIN32

TEST(OsError, Win32Mapping) {
    const std::pair<DWORD, ErrorCode> cases[] = {
        {ERROR_FILE_NOT_FOUND, ErrorCode::NotFound},
        {ERROR_PATH_NOT_FOUND, ErrorCode::NotFound},
        {ERROR_INVALID_DRIVE, ErrorCode::NotFound},
        {ERROR_BAD_NETPATH, ErrorCode::NotFound},
        {ERROR_BAD_NET_NAME, ErrorCode::NotFound},
        {ERROR_FILE_EXISTS, ErrorCode::AlreadyExists},
        {ERROR_ALREADY_EXISTS, ErrorCode::AlreadyExists},
        {ERROR_ACCESS_DENIED, ErrorCode::PermissionDenied},
        {ERROR_WRITE_PROTECT, ErrorCode::PermissionDenied},
        {ERROR_PRIVILEGE_NOT_HELD, ErrorCode::PermissionDenied},
        {ERROR_SHARING_VIOLATION, ErrorCode::Busy},
        {ERROR_LOCK_VIOLATION, ErrorCode::Busy},
        {ERROR_USER_MAPPED_FILE, ErrorCode::Busy},
        {ERROR_INVALID_NAME, ErrorCode::InvalidArgument},
        {ERROR_FILENAME_EXCED_RANGE, ErrorCode::InvalidArgument},
        {ERROR_DIRECTORY, ErrorCode::InvalidArgument},
        {ERROR_BAD_EXE_FORMAT, ErrorCode::InvalidArgument},
        {ERROR_NOT_SUPPORTED, ErrorCode::Unsupported},
        {ERROR_NOT_SAME_DEVICE, ErrorCode::Unsupported},
        {WAIT_TIMEOUT, ErrorCode::Timeout},
        {ERROR_OPERATION_ABORTED, ErrorCode::Cancelled},
        {ERROR_INVALID_HANDLE, ErrorCode::Internal},
        {ERROR_DISK_FULL, ErrorCode::IoError},
        {ERROR_HANDLE_DISK_FULL, ErrorCode::IoError},
        {ERROR_DIR_NOT_EMPTY, ErrorCode::IoError},
        {ERROR_BROKEN_PIPE, ErrorCode::IoError},
        {0xDEADu, ErrorCode::IoError},
    };
    for (const auto& [value, code] : cases) {
        EXPECT_EQ(frost::io::errorCodeFromWin32(value), code) << "win32 error " << value;
    }
}

TEST(OsError, Win32ErrorKeepsOperationPathNumberAndSystemText) {
    const frost::Error error = frost::io::win32Error(ERROR_FILE_NOT_FOUND, "readFile", "C:\\no\\such\\file");
    EXPECT_EQ(error.code(), ErrorCode::NotFound);
    EXPECT_NE(error.message().find("readFile 'C:\\no\\such\\file': "), std::string::npos) << error.message();
    EXPECT_NE(error.message().find("(win32 error 2)"), std::string::npos) << error.message();
    // FormatMessageW supplied some text, with the trailing period and line break trimmed (the
    // text sits in the middle of the message, before the number).
    EXPECT_EQ(error.message().find_first_of("\r\n"), std::string::npos) << error.message();
    EXPECT_EQ(error.message().find(". (win32 error"), std::string::npos) << error.message();
    EXPECT_GT(error.message().size(), std::string("readFile 'C:\\no\\such\\file': (win32 error 2)").size());
}

TEST(OsError, LastWin32ErrorReadsGetLastError) {
    SetLastError(ERROR_SHARING_VIOLATION);
    EXPECT_EQ(frost::io::lastWin32Error("open").code(), ErrorCode::Busy);
}

#endif

} // namespace
