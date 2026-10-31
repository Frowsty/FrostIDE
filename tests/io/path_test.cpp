// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost::io path rules: absolute-path detection, normalisation and
// the Windows extended-length form, for both styles on every platform.

#include "frost/io/path.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using frost::ErrorCode;
using frost::io::PathStyle;

constexpr PathStyle kWin = PathStyle::Windows;
constexpr PathStyle kPosix = PathStyle::Posix;

std::string normalized(std::string_view path, PathStyle style) {
    auto result = frost::io::normalizePath(path, style);
    return result ? *result : "<error: " + result.error().describe() + ">";
}

TEST(Path, WindowsAbsoluteForms) {
    for (const std::string_view path :
         {"C:\\", "C:\\dir", "c:/dir/file.txt", "Z:/", "\\\\server\\share", "\\\\server\\share\\dir",
          "//server/share/dir", "\\\\?\\C:\\dir", "\\\\?\\UNC\\server\\share\\x", "\\\\?\\Volume{01234567}\\x",
          "\\\\.\\pipe\\name", "\\\\.\\NUL"}) {
        EXPECT_TRUE(frost::io::isAbsolutePath(path, kWin)) << path;
    }
}

TEST(Path, WindowsNonAbsoluteForms) {
    // Relative, drive-relative ("C:dir" uses the drive's current directory), rooted on the
    // current drive ("\dir"), incomplete UNC, ambiguous device spellings and embedded NULs.
    for (const std::string_view path : std::initializer_list<std::string_view>{
             "", "dir", "dir\\file", ".\\x", "..\\x", "C:", "C:dir", "\\dir", "/dir", "\\\\server", "\\\\server\\",
             "\\\\\\share", "//?/C:/x", "//./pipe/x", "\\\\?/C:\\x", std::string_view("C:\\a\0b", 6)}) {
        EXPECT_FALSE(frost::io::isAbsolutePath(path, kWin)) << path;
    }
}

TEST(Path, PosixAbsoluteForms) {
    EXPECT_TRUE(frost::io::isAbsolutePath("/", kPosix));
    EXPECT_TRUE(frost::io::isAbsolutePath("/usr/bin/clang", kPosix));
    EXPECT_TRUE(frost::io::isAbsolutePath("//net/x", kPosix));
    for (const std::string_view path : std::initializer_list<std::string_view>{
             "", "usr/bin", "./x", "../x", "C:\\x", "C:/x", "\\\\server\\share", std::string_view("/a\0b", 4)}) {
        EXPECT_FALSE(frost::io::isAbsolutePath(path, kPosix)) << path;
    }
}

TEST(Path, NormalizesWindowsSeparatorsAndDots) {
    EXPECT_EQ(normalized("C:/a//b/./c/../d/", kWin), "C:\\a\\b\\d");
    EXPECT_EQ(normalized("C:\\", kWin), "C:\\");
    EXPECT_EQ(normalized("C:/", kWin), "C:\\");
    EXPECT_EQ(normalized("C:\\..\\..", kWin), "C:\\"); // ".." stops at the root, as in GetFullPathNameW.
    EXPECT_EQ(normalized("c:\\Mixed/Case\\", kWin), "c:\\Mixed\\Case");
    EXPECT_EQ(normalized("//server/share/a/../b", kWin), "\\\\server\\share\\b");
    EXPECT_EQ(normalized("\\\\server\\share", kWin), "\\\\server\\share\\");
    EXPECT_EQ(normalized("\\\\server\\share\\..", kWin), "\\\\server\\share\\");
    // Names are kept literally: trailing dots and spaces, reserved device names.
    EXPECT_EQ(normalized("C:\\dir.\\name \\NUL", kWin), "C:\\dir.\\name \\NUL");
}

TEST(Path, VerbatimAndDevicePathsAreNotTouched) {
    EXPECT_EQ(normalized("\\\\?\\C:\\a\\..\\b", kWin), "\\\\?\\C:\\a\\..\\b");
    EXPECT_EQ(normalized("\\\\?\\UNC\\srv\\sh\\.\\x", kWin), "\\\\?\\UNC\\srv\\sh\\.\\x");
    EXPECT_EQ(normalized("\\\\.\\pipe\\frost", kWin), "\\\\.\\pipe\\frost");
}

TEST(Path, NormalizesPosixButKeepsDotDot) {
    EXPECT_EQ(normalized("/", kPosix), "/");
    EXPECT_EQ(normalized("//a/./b//", kPosix), "/a/b");
    // Symlinks make "a/.." differ from "", so ".." is left to the kernel.
    EXPECT_EQ(normalized("/a/../b", kPosix), "/a/../b");
    EXPECT_EQ(normalized("/a\\b", kPosix), "/a\\b"); // Backslash is an ordinary character on POSIX.
}

TEST(Path, NormalizeRejectsRelativePathsAndNul) {
    for (const auto& [path, style] : {std::pair<std::string_view, PathStyle>{"rel", kWin},
                                      {"C:rel", kWin},
                                      {"\\rel", kWin},
                                      {"rel", kPosix},
                                      {"", kPosix},
                                      {std::string_view("/a\0", 3), kPosix}}) {
        const auto result = frost::io::normalizePath(path, style);
        ASSERT_FALSE(result.has_value()) << path;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(Path, ExtendedLengthForm) {
    EXPECT_EQ(frost::io::extendedLengthPath("C:\\a").value(), "\\\\?\\C:\\a");
    EXPECT_EQ(frost::io::extendedLengthPath("C:/a/./b/../c").value(), "\\\\?\\C:\\a\\c");
    EXPECT_EQ(frost::io::extendedLengthPath("C:\\").value(), "\\\\?\\C:\\"); // Keeps the root's backslash.
    EXPECT_EQ(frost::io::extendedLengthPath("\\\\srv\\share\\x").value(), "\\\\?\\UNC\\srv\\share\\x");
    EXPECT_EQ(frost::io::extendedLengthPath("//srv/share").value(), "\\\\?\\UNC\\srv\\share\\");
    EXPECT_EQ(frost::io::extendedLengthPath("\\\\?\\C:\\x").value(), "\\\\?\\C:\\x");
    EXPECT_EQ(frost::io::extendedLengthPath("\\\\.\\pipe\\p").value(), "\\\\.\\pipe\\p");
    const auto relative = frost::io::extendedLengthPath("dir\\x");
    ASSERT_FALSE(relative.has_value());
    EXPECT_EQ(relative.error().code(), ErrorCode::InvalidArgument);
}

TEST(Path, ExtendedLengthFormOfAPathBeyondMaxPath) {
    std::string path = "C:\\";
    while (path.size() < 600) {
        path += std::string(50, 'x') + "/";
    }
    const auto extended = frost::io::extendedLengthPath(path);
    ASSERT_TRUE(extended.has_value());
    EXPECT_TRUE(extended->starts_with("\\\\?\\C:\\xxx"));
    EXPECT_EQ(extended->find('/'), std::string::npos);
    EXPECT_FALSE(extended->ends_with("\\"));
}

TEST(Path, JoinPath) {
    EXPECT_EQ(frost::io::joinPath("C:\\a", "b", kWin).value(), "C:\\a\\b");
    EXPECT_EQ(frost::io::joinPath("C:\\", "b/c", kWin).value(), "C:\\b\\c");
    EXPECT_EQ(frost::io::joinPath("/a", "b", kPosix).value(), "/a/b");
    EXPECT_EQ(frost::io::joinPath("/", "b", kPosix).value(), "/b");
    for (const std::string_view bad : {"", "\\x", "/x", "D:x"}) {
        const auto result = frost::io::joinPath("C:\\a", bad, kWin);
        ASSERT_FALSE(result.has_value()) << bad;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    }
    EXPECT_FALSE(frost::io::joinPath("/a", "/b", kPosix).has_value());
}

TEST(Path, ParentAndFileName) {
    EXPECT_EQ(frost::io::parentPath("C:\\a\\b", kWin), "C:\\a");
    EXPECT_EQ(frost::io::parentPath("C:\\a", kWin), "C:\\");
    EXPECT_EQ(frost::io::parentPath("C:\\", kWin), "");
    EXPECT_EQ(frost::io::parentPath("\\\\s\\sh\\a", kWin), "\\\\s\\sh\\");
    EXPECT_EQ(frost::io::parentPath("\\\\s\\sh\\", kWin), "");
    EXPECT_EQ(frost::io::parentPath("\\\\?\\C:\\a\\b", kWin), "\\\\?\\C:\\a");
    EXPECT_EQ(frost::io::parentPath("\\\\?\\C:\\a", kWin), "\\\\?\\C:\\");
    EXPECT_EQ(frost::io::parentPath("/a/b", kPosix), "/a");
    EXPECT_EQ(frost::io::parentPath("/a", kPosix), "/");
    EXPECT_EQ(frost::io::parentPath("/", kPosix), "");
    EXPECT_EQ(frost::io::parentPath("relative/x", kPosix), "");

    EXPECT_EQ(frost::io::fileName("C:\\a\\b.txt", kWin), "b.txt");
    EXPECT_EQ(frost::io::fileName("C:\\", kWin), "");
    EXPECT_EQ(frost::io::fileName("\\\\s\\sh\\x", kWin), "x");
    EXPECT_EQ(frost::io::fileName("/a/b", kPosix), "b");
    EXPECT_EQ(frost::io::fileName("/", kPosix), "");
}

TEST(Path, StdPathRoundTrip) {
    const std::string utf8 = "/tmp/ünïcødé 测试 😀";
    const auto stdPath = frost::io::toStdPath(utf8);
    ASSERT_TRUE(stdPath.has_value());
    EXPECT_EQ(frost::io::fromStdPath(*stdPath).value(), utf8);
#ifdef _WIN32
    EXPECT_EQ(stdPath->native(), L"/tmp/ünïcødé 测试 😀");
    // A lone surrogate is a legal NTFS name but not representable in UTF-8.
    EXPECT_FALSE(frost::io::fromStdPath(std::filesystem::path(std::wstring(1, wchar_t{0xD800}))).has_value());
#endif
}

#ifdef _WIN32
TEST(Path, Win32PathIsWideExtendedForm) {
    EXPECT_EQ(frost::io::toWin32Path("C:/Users/ünï/x").value(), L"\\\\?\\C:\\Users\\ünï\\x");
    EXPECT_FALSE(frost::io::toWin32Path("relative").has_value());
}
#endif

} // namespace
