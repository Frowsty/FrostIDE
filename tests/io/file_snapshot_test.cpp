// SPDX-License-Identifier: Apache-2.0
// Actual stable bounded file snapshots: hard caps, concurrent growth, links and partial writers.
#include "frost/io/file_system.hpp"
#include "frost/io/mem_fs.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace frost::io {
namespace {
TEST(FileSnapshot, RealAndMemFsEnforceTheCapBeforeCopyingAndRejectNonRegularFiles) {
    test::ScratchDir scratch("bounded-file-snapshot");
    auto& fs = realFileSystem();
    ASSERT_TRUE(fs.writeFileAtomic(scratch.join("data"), "abcdef"));
    EXPECT_EQ(fs.readFileSnapshot(scratch.join("data"), 6).value_or(""), "abcdef");
    EXPECT_EQ(fs.readFileSnapshot(scratch.join("data"), 5).error().code(), ErrorCode::OutOfRange);
    EXPECT_FALSE(fs.readFileSnapshot(scratch.path(), 100));
    ManualClock clock;
    MemFs memory(clock);
    // The virtual filesystem starts with its own default root, independent of the host temp drive.
    constexpr auto memoryDirectory = kNativePathStyle == PathStyle::Windows ? "C:\\snapshot" : "/snapshot";
    const auto memoryFile = joinPath(memoryDirectory, "data");
    ASSERT_TRUE(memoryFile);
    ASSERT_TRUE(memory.createDirectories(memoryDirectory));
    ASSERT_TRUE(memory.writeFileAtomic(*memoryFile, "abcdef"));
    EXPECT_EQ(memory.readFileSnapshot(*memoryFile, 6).value_or(""), "abcdef");
    EXPECT_EQ(memory.readFileSnapshot(*memoryFile, 5).error().code(), ErrorCode::OutOfRange);
    EXPECT_FALSE(memory.readFileSnapshot(memoryDirectory, 100));
    // Explicitly select the base implementation: unsupported implementations never silently
    // fall back to an unbounded readFile(), even when that file is otherwise readable.
    EXPECT_EQ(memory.IFileSystem::readFileSnapshot(*memoryFile, 6).error().code(), ErrorCode::Unsupported);
}
TEST(FileSnapshot, RealHardLinksAndPosixSymbolicLinksAreRefused) {
    test::ScratchDir scratch("snapshot-links");
    auto& fs = realFileSystem();
    const auto file = scratch.join("file"), linked = scratch.join("hardlink");
    ASSERT_TRUE(fs.writeFileAtomic(file, "bounded"));
    std::error_code error;
    std::filesystem::create_hard_link(toStdPath(file).value(), toStdPath(linked).value(), error);
    ASSERT_FALSE(error) << error.message();
    EXPECT_EQ(fs.readFileSnapshot(linked, 100).error().code(), ErrorCode::PermissionDenied);
    ASSERT_TRUE(fs.removeFile(linked));
#ifndef _WIN32
    const auto symbolic = scratch.join("symlink");
    ASSERT_EQ(::symlink(file.c_str(), symbolic.c_str()), 0);
    EXPECT_EQ(fs.readFileSnapshot(symbolic, 100).error().code(), ErrorCode::PermissionDenied);
#endif
}
#ifdef _WIN32
TEST(FileSnapshot, WindowsRecorderExceptionIsLimitedToItsSystemImage) {
    wchar_t directory[MAX_PATH];
    const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
    ASSERT_GT(length, 0U);
    ASSERT_LT(length, MAX_PATH);
    auto system = fromStdPath(std::filesystem::path(std::wstring_view(directory, length)) / "wpr.exe");
    ASSERT_TRUE(system);
    auto original = readWindowsSystemRecorderSnapshot(*system, 16U * 1024U * 1024U);
    ASSERT_TRUE(original);
    const auto tooSmall = readWindowsSystemRecorderSnapshot(*system, original->size() - 1);
    ASSERT_FALSE(tooSmall);
    EXPECT_EQ(tooSmall.error().code(), ErrorCode::OutOfRange);

    const auto systemPath = toStdPath(*system);
    ASSERT_TRUE(systemPath);
    std::wstring linkName(32768, L'\0');
    DWORD linkLength = static_cast<DWORD>(linkName.size());
    HANDLE links = FindFirstFileNameW(systemPath->c_str(), 0, &linkLength, linkName.data());
    if (links != INVALID_HANDLE_VALUE) {
        do {
            auto alias = systemPath->root_name() / linkName.c_str();
            if (CompareStringOrdinal(alias.c_str(), -1, systemPath->c_str(), -1, TRUE) != CSTR_EQUAL) {
                auto aliasPath = fromStdPath(alias);
                EXPECT_TRUE(aliasPath);
                if (aliasPath) {
                    const auto refused = readWindowsSystemRecorderSnapshot(*aliasPath, original->size());
                    EXPECT_FALSE(refused);
                    if (!refused)
                        EXPECT_EQ(refused.error().code(), ErrorCode::PermissionDenied);
                }
            }
            linkLength = static_cast<DWORD>(linkName.size());
        } while (FindNextFileNameW(links, &linkLength, linkName.data()));
        EXPECT_EQ(GetLastError(), ERROR_HANDLE_EOF);
        FindClose(links);
    }

    test::ScratchDir scratch("snapshot-system-recorder-copy");
    auto& fs = realFileSystem();
    const auto copy = scratch.join("wpr.exe"), alias = scratch.join("alias.exe");
    ASSERT_TRUE(fs.writeFileAtomic(copy, *original));
    std::error_code error;
    std::filesystem::create_hard_link(toStdPath(copy).value(), toStdPath(alias).value(), error);
    ASSERT_FALSE(error) << error.message();
    EXPECT_EQ(fs.readFileSnapshot(copy, original->size()).error().code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(readWindowsSystemRecorderSnapshot(copy, original->size()).error().code(), ErrorCode::PermissionDenied);
    EXPECT_EQ(readWindowsSystemRecorderSnapshot(alias, original->size()).error().code(), ErrorCode::PermissionDenied);
}
#endif
TEST(FileSnapshot, ConcurrentGrowthCannotTurnTheBoundedReaderIntoAnUnboundedReadToEof) {
    test::ScratchDir scratch("snapshot-growth");
    auto& fs = realFileSystem();
    const auto file = scratch.join("data");
    ASSERT_TRUE(fs.writeFileAtomic(file, std::string(64 * 1024, 'a')));
    std::atomic<bool> opened{false}, written{false}, failed{false};
    std::jthread writer([&] {
        std::ofstream output(toStdPath(file).value(), std::ios::binary | std::ios::app);
        if (!output) {
            failed = true;
            opened = true;
            return;
        }
        opened = true;
        const std::string block(4096, 'x');
        for (unsigned i = 0; i < 1024; ++i) {
            output.write(block.data(), static_cast<std::streamsize>(block.size()));
            output.flush();
            if (!output) {
                failed = true;
                break;
            }
        }
        written = true;
    });
    while (!opened.load())
        std::this_thread::yield();
    for (unsigned i = 0; i < 128; ++i) {
        auto snapshot = fs.readFileSnapshot(file, 64 * 1024);
        if (snapshot)
            EXPECT_LE(snapshot->size(), 64U * 1024);
        else
            EXPECT_TRUE(snapshot.error().code() == ErrorCode::Busy ||
                        snapshot.error().code() == ErrorCode::OutOfRange ||
                        snapshot.error().code() == ErrorCode::PermissionDenied);
    }
    writer.join();
    EXPECT_FALSE(failed.load());
    EXPECT_TRUE(written.load());
    EXPECT_EQ(fs.readFileSnapshot(file, 64 * 1024).error().code(), ErrorCode::OutOfRange);
}
} // namespace
} // namespace frost::io
