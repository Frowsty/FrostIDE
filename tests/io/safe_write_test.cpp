// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for io::IFileSystem::safeWrite on the real file system (ADR 0028): new and
// replaced files, read-only targets refused unless overwriteReadOnly, hard links kept by an
// in-place rewrite, symlinks kept by writing their target, and never a temp file left behind;
// on Windows a file another program holds open is refused with Busy and keeps its attributes and
// creation time across a save, on POSIX the file mode survives.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

using frost::ErrorCode;
using frost::io::SafeWriteMethod;
using frost::io::SafeWriteOptions;
using frost::test::ScratchDir;

frost::io::IFileSystem& fs() { return frost::io::realFileSystem(); }

std::string read(const std::string& file) {
    auto bytes = fs().readFile(file);
    return bytes ? std::move(*bytes) : std::string("<unreadable>");
}

std::filesystem::path native(const std::string& file) { return frost::io::toStdPath(file).value(); }

// Whether a writeFileAtomic or safeWrite temp file is left in `directory`.
bool tempLeft(const std::string& directory) {
    auto entries = fs().listDirectory(directory);
    EXPECT_TRUE(entries);
    for (const auto& entry : entries.value_or(std::vector<frost::io::DirEntry>{})) {
        if (entry.name.find(".frost-tmp-") != std::string::npos) {
            return true;
        }
    }
    return false;
}

void setReadOnly(const std::string& file, bool readOnly) {
    std::error_code error;
    std::filesystem::permissions(native(file),
                                 std::filesystem::perms::owner_write | std::filesystem::perms::group_write |
                                     std::filesystem::perms::others_write,
                                 readOnly ? std::filesystem::perm_options::remove : std::filesystem::perm_options::add,
                                 error);
    ASSERT_FALSE(error) << error.message();
}

TEST(SafeWrite, CreatesAndReplaces) {
    const ScratchDir dir("safe-write-basic");
    const std::string file = dir.join("a.txt");
    auto created = fs().safeWrite(file, "one\r\n", SafeWriteOptions{});
    ASSERT_TRUE(created) << created.error().describe();
    EXPECT_EQ(created->method, SafeWriteMethod::Created);
    auto replaced = fs().safeWrite(file, "two\n", SafeWriteOptions{});
    ASSERT_TRUE(replaced) << replaced.error().describe();
    EXPECT_EQ(replaced->method, SafeWriteMethod::Replaced);
    EXPECT_EQ(replaced->target, frost::io::normalizePath(file).value());
    EXPECT_EQ(read(file), "two\n");
    EXPECT_FALSE(tempLeft(dir.path()));

    // A directory is refused; a missing parent fails without creating anything.
    EXPECT_FALSE(fs().safeWrite(dir.path(), "x", SafeWriteOptions{}));
    auto orphan = fs().safeWrite(dir.join("missing/b.txt"), "x", SafeWriteOptions{});
    ASSERT_FALSE(orphan);
    EXPECT_EQ(orphan.error().code(), ErrorCode::NotFound) << orphan.error().describe();
}

TEST(SafeWrite, ReadOnlyTargetsNeedOverwrite) {
    const ScratchDir dir("safe-write-readonly");
    const std::string file = dir.join("ro.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "original"));
    setReadOnly(file, true);
    auto refused = fs().safeWrite(file, "new", SafeWriteOptions{});
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::PermissionDenied) << refused.error().describe();
    EXPECT_EQ(read(file), "original");
    EXPECT_FALSE(tempLeft(dir.path()));

    auto forced = fs().safeWrite(file, "new", SafeWriteOptions{.overwriteReadOnly = true});
    ASSERT_TRUE(forced) << forced.error().describe();
    EXPECT_EQ(read(file), "new");
    const auto perms = std::filesystem::status(native(file)).permissions();
    EXPECT_NE(perms & std::filesystem::perms::owner_write, std::filesystem::perms::none);
    setReadOnly(file, false);
}

TEST(SafeWrite, HardLinksStayLinked) {
    const ScratchDir dir("safe-write-hardlink");
    const std::string file = dir.join("a.txt");
    const std::string other = dir.join("b.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "shared content that is long enough\n"));
    std::error_code error;
    std::filesystem::create_hard_link(native(file), native(other), error);
    ASSERT_FALSE(error) << error.message();
    for (const std::string_view content : {std::string_view("short\n"), std::string_view("a longer second save\n")}) {
        auto written = fs().safeWrite(file, content, SafeWriteOptions{});
        ASSERT_TRUE(written) << written.error().describe();
        EXPECT_EQ(written->method, SafeWriteMethod::InPlace);
        EXPECT_EQ(read(file), content);
        EXPECT_EQ(read(other), content); // Still the same file.
    }
    EXPECT_EQ(std::filesystem::hard_link_count(native(file)), 2U);
    EXPECT_FALSE(tempLeft(dir.path()));
}

TEST(SafeWrite, SymlinksStayLinks) {
    const ScratchDir dir("safe-write-symlink");
    ASSERT_TRUE(fs().createDirectories(dir.join("real")));
    const std::string target = dir.join("real/target.txt");
    const std::string link = dir.join("link.txt");
    ASSERT_TRUE(fs().writeFileAtomic(target, "old"));
    std::error_code error;
    std::filesystem::create_symlink(native(target), native(link), error);
    if (error) {
#ifdef _WIN32
        GTEST_SKIP() << "creating a symlink needs Developer Mode or elevation here: " << error.message();
#else
        FAIL() << error.message();
#endif
    }
    auto written = fs().safeWrite(link, "new", SafeWriteOptions{});
    ASSERT_TRUE(written) << written.error().describe();
    EXPECT_EQ(written->target, frost::io::normalizePath(target).value());
    EXPECT_TRUE(std::filesystem::is_symlink(native(link)));
    EXPECT_EQ(read(target), "new");
    EXPECT_FALSE(tempLeft(dir.path()));
    EXPECT_FALSE(tempLeft(dir.join("real")));
}

#ifdef _WIN32
TEST(SafeWrite, LockedFilesAreBusyAndUnchanged) {
    const ScratchDir dir("safe-write-locked");
    const std::string file = dir.join("locked.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "original"));
    const std::wstring wide = frost::io::toWide(file).value();
    // Another program holds the file open without sharing (Visual Studio's build, an antivirus scan).
    HANDLE held = CreateFileW(wide.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(held, INVALID_HANDLE_VALUE) << GetLastError();
    auto refused = fs().safeWrite(file, "new", SafeWriteOptions{});
    CloseHandle(held);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code(), ErrorCode::Busy) << refused.error().describe();
    EXPECT_EQ(read(file), "original");
    EXPECT_FALSE(tempLeft(dir.path()));
    auto later = fs().safeWrite(file, "new", SafeWriteOptions{});
    ASSERT_TRUE(later) << later.error().describe();
    EXPECT_EQ(read(file), "new");
}

TEST(SafeWrite, KeepsAttributesAndCreationTime) {
    const ScratchDir dir("safe-write-attributes");
    const std::string file = dir.join("hidden.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "original"));
    const std::wstring wide = frost::io::toWide(file).value();
    ASSERT_NE(SetFileAttributesW(wide.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_ARCHIVE), FALSE);
    FILETIME created{};
    {
        // A creation time well in the past (2001-01-01), so a replacement's own time would differ.
        SYSTEMTIME old{2001, 1, 1, 1, 0, 0, 0, 0};
        ASSERT_NE(SystemTimeToFileTime(&old, &created), FALSE);
        HANDLE handle = CreateFileW(wide.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        ASSERT_NE(handle, INVALID_HANDLE_VALUE);
        EXPECT_NE(SetFileTime(handle, &created, nullptr, nullptr), FALSE);
        CloseHandle(handle);
    }
    auto written = fs().safeWrite(file, "replaced", SafeWriteOptions{});
    ASSERT_TRUE(written) << written.error().describe();
    EXPECT_EQ(written->method, SafeWriteMethod::Replaced);
    EXPECT_EQ(read(file), "replaced");
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    EXPECT_NE(attributes & FILE_ATTRIBUTE_HIDDEN, 0U);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    ASSERT_NE(GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data), FALSE);
    EXPECT_EQ(data.ftCreationTime.dwLowDateTime, created.dwLowDateTime);
    EXPECT_EQ(data.ftCreationTime.dwHighDateTime, created.dwHighDateTime);
    ASSERT_NE(SetFileAttributesW(wide.c_str(), FILE_ATTRIBUTE_NORMAL), FALSE);
}
#else
TEST(SafeWrite, KeepsTheFileMode) {
    const ScratchDir dir("safe-write-mode");
    const std::string file = dir.join("script.sh");
    ASSERT_TRUE(fs().writeFileAtomic(file, "#!/bin/sh\n"));
    ASSERT_EQ(chmod(file.c_str(), 0750), 0);
    auto written = fs().safeWrite(file, "#!/bin/sh\necho hi\n", SafeWriteOptions{});
    ASSERT_TRUE(written) << written.error().describe();
    struct stat info{};
    ASSERT_EQ(stat(file.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 07777, 0750U);
    EXPECT_EQ(read(file), "#!/bin/sh\necho hi\n");
}
#endif

} // namespace
