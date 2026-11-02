// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the atomic-write temp files (file_system.hpp, ADR 0028): parseAtomicTempName on
// the names writeFileAtomic and safeWrite create, and removeStaleTempFiles on MemFs, which removes
// only other processes' temp files older than the cut-off, and IFileSystem::safeWrite's default.

#include "frost/core/clock.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/mem_fs.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using frost::io::AtomicTempName;
using frost::io::parseAtomicTempName;
using frost::io::PathStyle;

std::uint32_t ownPid() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

TEST(AtomicTempName, ParsesOnlyTempNames) {
    EXPECT_EQ(parseAtomicTempName(".main.cpp.frost-tmp-1234-7"), (AtomicTempName{1234, 7}));
    EXPECT_EQ(parseAtomicTempName(".a.frost-tmp-1-18446744073709551615"), (AtomicTempName{1, 18446744073709551615ULL}));
    EXPECT_EQ(parseAtomicTempName(".x.frost-tmp-2-3.frost-tmp-4-5"), (AtomicTempName{4, 5})); // The last marker.
    for (const char* name :
         {"main.cpp", ".main.cpp", "main.cpp.frost-tmp-1-2", ".frost-tmp-1-2", ".a.frost-tmp-1", ".a.frost-tmp--2",
          ".a.frost-tmp-1-", ".a.frost-tmp-x-2", ".a.frost-tmp-1-2x", ".a.frost-tmp-99999999999-2"}) {
        EXPECT_FALSE(parseAtomicTempName(name).has_value()) << name;
    }
}

TEST(RemoveStaleTempFiles, RemovesOldTempFilesOfOtherProcesses) {
    frost::ManualClock clock(frost::IClock::SteadyTime{}, frost::IClock::SystemTime{std::chrono::hours(1000)});
    frost::io::MemFs fs(clock, PathStyle::Posix);
    ASSERT_TRUE(fs.createDirectories("/d/sub"));
    const std::string own = std::format("/d/.c.txt.frost-tmp-{}-1", ownPid());
    const std::string otherPid = std::format("{}", ownPid() + 1);
    ASSERT_TRUE(fs.writeFileAtomic("/d/.a.txt.frost-tmp-" + otherPid + "-1", "old"));
    ASSERT_TRUE(fs.writeFileAtomic(own, "ours, in flight"));
    ASSERT_TRUE(fs.writeFileAtomic("/d/a.txt", "a file"));
    ASSERT_TRUE(fs.writeFileAtomic("/d/sub/.s.txt.frost-tmp-" + otherPid + "-2", "other directory"));
    clock.advance(2h);
    ASSERT_TRUE(fs.writeFileAtomic("/d/.b.txt.frost-tmp-" + otherPid + "-2", "young"));

    const auto cutoff = clock.systemNow() - 1h;
    auto removed = frost::io::removeStaleTempFiles(fs, "/d", cutoff, PathStyle::Posix);
    ASSERT_TRUE(removed) << removed.error().describe();
    EXPECT_EQ(*removed, 1U);
    EXPECT_FALSE(fs.exists("/d/.a.txt.frost-tmp-" + otherPid + "-1").value());
    EXPECT_TRUE(fs.exists("/d/.b.txt.frost-tmp-" + otherPid + "-2").value());
    EXPECT_TRUE(fs.exists(own).value());
    EXPECT_TRUE(fs.exists("/d/a.txt").value());
    EXPECT_TRUE(fs.exists("/d/sub/.s.txt.frost-tmp-" + otherPid + "-2").value()); // Not recursive.

    EXPECT_EQ(frost::io::removeStaleTempFiles(fs, "/missing", cutoff, PathStyle::Posix).error().code(),
              frost::ErrorCode::NotFound);
}

TEST(SafeWriteDefault, CreatesOrReplacesThroughWriteFileAtomic) {
    frost::ManualClock clock;
    frost::io::MemFs fs(clock, PathStyle::Posix);
    ASSERT_TRUE(fs.createDirectories("/d"));
    auto created = fs.safeWrite("/d/f.txt", "1", frost::io::SafeWriteOptions{});
    ASSERT_TRUE(created) << created.error().describe();
    EXPECT_EQ(created->method, frost::io::SafeWriteMethod::Created);
    EXPECT_EQ(created->target, "/d/f.txt");
    auto replaced = fs.safeWrite("/d/f.txt", "2", frost::io::SafeWriteOptions{});
    ASSERT_TRUE(replaced);
    EXPECT_EQ(replaced->method, frost::io::SafeWriteMethod::Replaced);
    EXPECT_EQ(fs.readFile("/d/f.txt").value(), "2");
}

} // namespace
