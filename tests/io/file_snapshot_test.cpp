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

#ifndef _WIN32
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
    ASSERT_TRUE(memory.createDirectories(scratch.path()));
    ASSERT_TRUE(memory.writeFileAtomic(scratch.join("data"), "abcdef"));
    EXPECT_EQ(memory.readFileSnapshot(scratch.join("data"), 6).value_or(""), "abcdef");
    EXPECT_EQ(memory.readFileSnapshot(scratch.join("data"), 5).error().code(), ErrorCode::OutOfRange);
    // Explicitly select the base implementation: unsupported implementations never silently
    // fall back to an unbounded readFile(), even when that file is otherwise readable.
    EXPECT_EQ(memory.IFileSystem::readFileSnapshot(scratch.join("data"), 6).error().code(), ErrorCode::Unsupported);
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
