// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for the real IFileSystem against the disk: byte-exact
// round trips, atomic replacement (no temp files left, old content kept on
// failure), listings, error-code mapping, non-ASCII names, and on Windows
// paths longer than MAX_PATH, which must work even with LongPathsEnabled = 0.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <vector>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include <windows.h>
#else
#include <sys/stat.h>

#include <unistd.h>
#endif

namespace {

using namespace std::string_literals;
using frost::ErrorCode;
using frost::io::DirEntry;
using frost::io::FileType;
using frost::test::ScratchDir;

frost::io::IFileSystem& fs() { return frost::io::realFileSystem(); }

// True when no writeFileAtomic temp file is left in `directory`.
bool noTempFiles(const std::string& directory) {
    const auto entries = fs().listDirectory(directory);
    if (!entries) {
        return false;
    }
    for (const DirEntry& entry : *entries) {
        if (entry.name.find(".frost-tmp-") != std::string::npos) {
            return false;
        }
    }
    return true;
}

#ifndef _WIN32
// Permission checks do not apply to root (e.g. tests run in a container as root).
bool runningAsRoot() { return geteuid() == 0; }
#endif

TEST(RealFileSystem, WriteAndReadAreByteExact) {
    const ScratchDir dir("fs-bytes");
    std::string data = "line one\r\nline two\n\0\xff\xfe binary"s; // The s suffix keeps the NUL.
    data += std::string(200'000, 'x'); // Larger than one I/O chunk on any buffer size we use.
    const std::string file = dir.join("data.bin");
    ASSERT_TRUE(fs().writeFileAtomic(file, data).has_value());
    EXPECT_EQ(fs().readFile(file).value(), data);
    ASSERT_TRUE(fs().writeFileAtomic(file, "").has_value());
    EXPECT_EQ(fs().readFile(file).value(), "");
}

TEST(RealFileSystem, AtomicWriteReplacesAndLeavesNoTempFiles) {
    const ScratchDir dir("fs-atomic");
    const std::string file = dir.join("settings.json");
    ASSERT_TRUE(fs().writeFileAtomic(file, "version 1, rather long content").has_value());
    ASSERT_TRUE(fs().writeFileAtomic(file, "v2").has_value());
    EXPECT_EQ(fs().readFile(file).value(), "v2");
    EXPECT_EQ(fs().listDirectory(dir.path()).value(), (std::vector<DirEntry>{{"settings.json", FileType::Regular}}));
    EXPECT_TRUE(noTempFiles(dir.path()));
}

TEST(RealFileSystem, AtomicWriteIntoMissingDirectoryFails) {
    const ScratchDir dir("fs-missing");
    const auto result = fs().writeFileAtomic(dir.join("no/such/dir/file"), "x");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
    EXPECT_TRUE(fs().listDirectory(dir.path()).value().empty());
}

TEST(RealFileSystem, AtomicWriteOverADirectoryFailsAndLeavesItAlone) {
    const ScratchDir dir("fs-overdir");
    const std::string target = dir.join("sub");
    ASSERT_TRUE(fs().createDirectories(target).has_value());
    ASSERT_TRUE(fs().writeFileAtomic(dir.join("sub/keep"), "k").has_value());
    const auto result = fs().writeFileAtomic(target, "x");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << result.error().describe();
    EXPECT_EQ(fs().readFile(dir.join("sub/keep")).value(), "k");
    EXPECT_TRUE(noTempFiles(dir.path()));
}

#ifdef _WIN32
TEST(RealFileSystem, FailedReplaceKeepsTheOldContent) {
    // Another process holding the target open without FILE_SHARE_DELETE blocks the rename,
    // a real-world failure (virus scanners, editors) injected without any mock.
    const ScratchDir dir("fs-locked");
    const std::string file = dir.join("locked.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "old content").has_value());
    const std::wstring wide = frost::io::toWin32Path(file).value();
    const HANDLE holder =
        CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(holder, INVALID_HANDLE_VALUE);
    const auto result = fs().writeFileAtomic(file, "new content");
    CloseHandle(holder);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().code() == ErrorCode::PermissionDenied || result.error().code() == ErrorCode::Busy)
        << result.error().describe();
    EXPECT_EQ(fs().readFile(file).value(), "old content");
    EXPECT_TRUE(noTempFiles(dir.path()));
}

TEST(RealFileSystem, SharingViolationMapsToBusy) {
    const ScratchDir dir("fs-busy");
    const std::string file = dir.join("exclusive.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "x").has_value());
    const std::wstring wide = frost::io::toWin32Path(file).value();
    const HANDLE holder = CreateFileW(wide.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(holder, INVALID_HANDLE_VALUE);
    const auto result = fs().readFile(file);
    CloseHandle(holder);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Busy);
    EXPECT_NE(result.error().message().find("win32 error 32"), std::string::npos) << result.error().message();
}

TEST(RealFileSystem, ForwardSlashesAndDotSegmentsAreNormalised) {
    const ScratchDir dir("fs-slashes");
    std::string mixed = dir.path();
    std::ranges::replace(mixed, '\\', '/');
    ASSERT_TRUE(fs().writeFileAtomic(mixed + "/./x/../file.txt", "slashes").has_value());
    EXPECT_EQ(fs().readFile(dir.join("file.txt")).value(), "slashes");
}
#else
TEST(RealFileSystem, FailedWriteKeepsTheOldContent) {
    if (runningAsRoot()) {
        GTEST_SKIP() << "root ignores directory permissions";
    }
    // A read-only directory refuses the temp file, so the replace never starts.
    const ScratchDir dir("fs-readonly");
    const std::string sub = dir.join("ro");
    ASSERT_TRUE(fs().createDirectories(sub).has_value());
    const std::string file = sub + "/file.txt";
    ASSERT_TRUE(fs().writeFileAtomic(file, "old content").has_value());
    ASSERT_EQ(chmod(sub.c_str(), 0555), 0);
    const auto result = fs().writeFileAtomic(file, "new content");
    chmod(sub.c_str(), 0755);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied) << result.error().describe();
    EXPECT_EQ(fs().readFile(file).value(), "old content");
    EXPECT_TRUE(noTempFiles(sub));
}

TEST(RealFileSystem, AtomicWriteKeepsPermissionBits) {
    const ScratchDir dir("fs-mode");
    const std::string file = dir.join("script.sh");
    ASSERT_TRUE(fs().writeFileAtomic(file, "#!/bin/sh\n").has_value());
    ASSERT_EQ(chmod(file.c_str(), 0751), 0);
    ASSERT_TRUE(fs().writeFileAtomic(file, "#!/bin/sh\necho hi\n").has_value());
    struct stat info{};
    ASSERT_EQ(stat(file.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 07777, 0751u);
}

TEST(RealFileSystem, UnreadableFileMapsToPermissionDenied) {
    if (runningAsRoot()) {
        GTEST_SKIP() << "root ignores file permissions";
    }
    const ScratchDir dir("fs-eacces");
    const std::string file = dir.join("secret");
    ASSERT_TRUE(fs().writeFileAtomic(file, "x").has_value());
    ASSERT_EQ(chmod(file.c_str(), 0), 0);
    const auto result = fs().readFile(file);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::PermissionDenied);
    EXPECT_NE(result.error().message().find("errno"), std::string::npos);
}
#endif

TEST(RealFileSystem, StatusReportsTypeSizeAndTime) {
    const ScratchDir dir("fs-status");
    const std::string file = dir.join("f.txt");
    ASSERT_TRUE(fs().writeFileAtomic(file, "12345").has_value());
    const auto fileStatus = fs().status(file);
    ASSERT_TRUE(fileStatus.has_value());
    EXPECT_EQ(fileStatus->type, FileType::Regular);
    EXPECT_EQ(fileStatus->size, 5u);
    const auto age = std::chrono::system_clock::now() - fileStatus->mtime;
    EXPECT_LT(age, std::chrono::hours(1));
    EXPECT_GT(age, -std::chrono::hours(1));
    const auto dirStatus = fs().status(dir.path());
    ASSERT_TRUE(dirStatus.has_value());
    EXPECT_EQ(dirStatus->type, FileType::Directory);
    EXPECT_EQ(dirStatus->size, 0u);
}

TEST(RealFileSystem, ListingIsSortedAndTyped) {
    const ScratchDir dir("fs-list");
    ASSERT_TRUE(fs().writeFileAtomic(dir.join("b.txt"), "").has_value());
    ASSERT_TRUE(fs().writeFileAtomic(dir.join("a.txt"), "").has_value());
    ASSERT_TRUE(fs().createDirectories(dir.join("c/d")).has_value());
    const std::vector<DirEntry> expected{
        {"a.txt", FileType::Regular}, {"b.txt", FileType::Regular}, {"c", FileType::Directory}};
    EXPECT_EQ(fs().listDirectory(dir.path()).value(), expected);
    EXPECT_TRUE(fs().listDirectory(dir.join("c/d")).value().empty());
}

TEST(RealFileSystem, CreateDirectoriesIsIdempotent) {
    const ScratchDir dir("fs-mkdir");
    ASSERT_TRUE(fs().createDirectories(dir.join("a/b/c")).has_value());
    ASSERT_TRUE(fs().createDirectories(dir.join("a/b/c")).has_value());
    ASSERT_TRUE(fs().createDirectories(dir.path()).has_value());
    EXPECT_EQ(fs().status(dir.join("a/b/c"))->type, FileType::Directory);
}

TEST(RealFileSystem, ErrorCodes) {
    const ScratchDir dir("fs-errors");
    const std::string file = dir.join("file");
    const std::string sub = dir.join("sub");
    ASSERT_TRUE(fs().writeFileAtomic(file, "x").has_value());
    ASSERT_TRUE(fs().createDirectories(sub + "/inner").has_value());
    const std::string missing = dir.join("missing");

    EXPECT_EQ(fs().readFile(missing).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs().readFile(sub).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(fs().status(missing).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs().status(dir.join("file/below")).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs().listDirectory(missing).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs().listDirectory(file).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(fs().createDirectories(file).error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(fs().createDirectories(dir.join("file/below")).error().code(), ErrorCode::AlreadyExists);
    EXPECT_EQ(fs().removeFile(missing).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs().removeFile(sub).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(fs().removeDirectory(sub).error().code(), ErrorCode::IoError); // Not empty.
    EXPECT_EQ(fs().removeDirectory(file).error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(fs().removeDirectory(missing).error().code(), ErrorCode::NotFound);
    EXPECT_FALSE(fs().exists(missing).value());
    EXPECT_TRUE(fs().exists(file).value());
    EXPECT_TRUE(fs().exists(sub).value());

    // The message names the operation and the path, and keeps the OS error number.
    const auto notFound = fs().readFile(missing);
    EXPECT_NE(notFound.error().message().find("readFile '" + missing + "'"), std::string::npos)
        << notFound.error().message();
#ifdef _WIN32
    EXPECT_NE(notFound.error().message().find("win32 error 2"), std::string::npos) << notFound.error().message();
#else
    EXPECT_NE(notFound.error().message().find("errno 2"), std::string::npos) << notFound.error().message();
#endif

    // Relative paths are never resolved against the current directory.
    for (const char* relative : {"file", "./file", "sub/inner"}) {
        EXPECT_EQ(fs().readFile(relative).error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(fs().writeFileAtomic(relative, "x").error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(fs().status(relative).error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(fs().listDirectory(relative).error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(fs().createDirectories(relative).error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(fs().removeFile(relative).error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(fs().removeDirectory(relative).error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(fs().exists(relative).error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(RealFileSystem, NonAsciiNames) {
    const ScratchDir dir("fs-ünïcødé");
    const std::string sub = dir.join("ünïcødé 测试 😀");
    const std::string file = frost::io::joinPath(sub, "файл ✓.txt").value();
    ASSERT_TRUE(fs().createDirectories(sub).has_value());
    ASSERT_TRUE(fs().writeFileAtomic(file, "содержимое").has_value());
    EXPECT_EQ(fs().readFile(file).value(), "содержимое");
    EXPECT_EQ(fs().listDirectory(dir.path()).value(),
              (std::vector<DirEntry>{{"ünïcødé 测试 😀", FileType::Directory}}));
    EXPECT_EQ(fs().listDirectory(sub).value(), (std::vector<DirEntry>{{"файл ✓.txt", FileType::Regular}}));
    ASSERT_TRUE(fs().removeFile(file).has_value());
    ASSERT_TRUE(fs().removeDirectory(sub).has_value());
}

TEST(RealFileSystem, PathsBeyondMaxPath) {
    // Windows' MAX_PATH is 260 UTF-16 units. frost::io uses \\?\ paths internally, so this
    // must pass even where LongPathsEnabled is 0 (as on the reference machine).
    const ScratchDir dir("fs-long");
    std::string deep = dir.path();
    int depth = 0;
    while (deep.size() < 400) {
        auto next = frost::io::joinPath(deep, std::format("segment-{:02}-{}", depth++, std::string(40, 'x')));
        ASSERT_TRUE(next.has_value());
        deep = std::move(*next);
    }
    ASSERT_GT(deep.size(), 260u);
    ASSERT_TRUE(fs().createDirectories(deep).has_value());
    const std::string file = frost::io::joinPath(deep, "long-path file ✓.txt").value();
    ASSERT_GT(file.size(), 300u);
    ASSERT_TRUE(fs().writeFileAtomic(file, "deep content").has_value());
    EXPECT_EQ(fs().readFile(file).value(), "deep content");
    const auto status = fs().status(file);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->size, 12u);
    EXPECT_EQ(fs().listDirectory(deep).value(), (std::vector<DirEntry>{{"long-path file ✓.txt", FileType::Regular}}));
    ASSERT_TRUE(fs().writeFileAtomic(file, "replaced").has_value()); // The atomic replace works there too.
    EXPECT_EQ(fs().readFile(file).value(), "replaced");
    EXPECT_TRUE(noTempFiles(deep));
    ASSERT_TRUE(fs().removeFile(file).has_value());
    EXPECT_FALSE(fs().exists(file).value());
    // ScratchDir removes the deep tree through the same API.
}

} // namespace
