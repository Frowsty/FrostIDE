// SPDX-License-Identifier: Apache-2.0
// Real test-only directory aliases: an NTFS junction on Windows and a symlink on POSIX.
#pragma once

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/utf.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::test {

// Creates only the alias. Callers retain the real target and use ScratchDir's link-safe cleanup.
inline void makeDirectoryLink(const std::string& link, const std::string& target) {
#ifdef _WIN32
    std::array<wchar_t, MAX_PATH> system{};
    const UINT length = GetSystemDirectoryW(system.data(), MAX_PATH);
    ASSERT_GT(length, 0U);
    ASSERT_LT(length, system.size());
    const auto directory = io::fromWide(std::wstring_view(system.data(), length));
    ASSERT_TRUE(directory);
    io::ProcessSpec spec;
    spec.executable = *directory + "\\cmd.exe";
    spec.arguments = {"/d", "/c", "mklink", "/J", link, target};
    spec.stderrMode = io::Stdio::MergeIntoStdout;
    const auto made = io::runToCompletion(io::realProcessLauncher(), spec, "", std::chrono::seconds(30));
    ASSERT_TRUE(made) << made.error().describe();
    ASSERT_EQ(made->exitCode, 0) << made->stdoutBytes;
    const auto wide = io::toWide(link);
    ASSERT_TRUE(wide);
    const HANDLE handle =
        CreateFileW(wide->c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE) << GetLastError();
    FILE_ATTRIBUTE_TAG_INFO tag{};
    const BOOL queried = GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag));
    const DWORD queryError = GetLastError();
    const BOOL closed = CloseHandle(handle);
    ASSERT_NE(queried, FALSE) << queryError;
    ASSERT_NE(closed, FALSE);
    ASSERT_EQ(tag.ReparseTag, static_cast<DWORD>(IO_REPARSE_TAG_MOUNT_POINT));
#else
    std::error_code error;
    const auto from = io::toStdPath(target), to = io::toStdPath(link);
    ASSERT_TRUE(from && to);
    std::filesystem::create_directory_symlink(*from, *to, error);
    ASSERT_FALSE(error) << error.message();
#endif
    const auto linkPath = io::toStdPath(link), targetPath = io::toStdPath(target);
    ASSERT_TRUE(linkPath && targetPath);
    std::error_code linkError, targetError;
    const auto resolvedLink = std::filesystem::canonical(*linkPath, linkError);
    const auto resolvedTarget = std::filesystem::canonical(*targetPath, targetError);
    ASSERT_FALSE(linkError) << linkError.message();
    ASSERT_FALSE(targetError) << targetError.message();
    ASSERT_EQ(resolvedLink, resolvedTarget);
}

} // namespace frost::test
