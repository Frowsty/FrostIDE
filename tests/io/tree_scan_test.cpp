// SPDX-License-Identifier: Apache-2.0
//
/*
 * Unit tests for scanTree (frost/io/tree_scan.hpp) on MemFs: nested .gitignore files and the precedence
 * of the stack, ignored directories that are never descended, skipped names and atomic-save temp files,
 * reportIgnored, the issues (unlistable directories, unreadable .gitignore files, never-matching
 * patterns, names that are not UTF-8), recursion, the descend filter and statuses, cancellation (before
 * and during the walk) and argument errors, and determinism across SimExecutor seeds, parallelism and a
 * ThreadPool.
 */

#include "frost/core/clock.hpp"
#include "frost/io/ignore_rules.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/tree_scan.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/sched/thread_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::io::FileType;
using frost::io::FsOp;
using frost::io::IgnoreRules;
using frost::io::IgnoreStack;
using frost::io::MemFs;
using frost::io::PathStyle;
using frost::io::ScanIssueKind;
using frost::io::ScanOptions;
using frost::io::TreeScan;
using frost::sched::Lane;
using frost::sched::SimExecutor;

class TreeScanTest : public ::testing::Test {
protected:
    TreeScanTest() : fs(clock, PathStyle::Posix) {}

    void file(const std::string& path, std::string_view content = "x") {
        const std::size_t slash = path.rfind('/');
        ASSERT_TRUE(fs.createDirectories(path.substr(0, slash == 0 ? 1 : slash)).has_value()) << path;
        ASSERT_TRUE(fs.writeFileAtomic(path, content).has_value()) << path;
    }
    void directory(const std::string& path) { ASSERT_TRUE(fs.createDirectories(path).has_value()) << path; }

    frost::Result<TreeScan> scan(ScanOptions options = {}, std::string root = "/repo", std::uint64_t seed = 0,
                                 std::stop_token token = {}) {
        SimExecutor sim(frost::sched::SimOptions{.seed = seed});
        return sim.spawn(Lane::Background, frost::io::scanTree(fs, sim, std::move(root), std::move(options), token))
            .join();
    }

    static std::vector<std::string> relatives(const TreeScan& scan) {
        std::vector<std::string> out;
        for (const auto& entry : scan.entries) {
            out.push_back(entry.relative + (entry.type == FileType::Directory ? "/" : "") +
                          (entry.ignored ? " (ignored)" : ""));
        }
        std::ranges::sort(out);
        return out;
    }

    frost::ManualClock clock;
    MemFs fs;
};

TEST_F(TreeScanTest, AppliesNestedGitignoresAndNeverDescendsIgnoredDirectories) {
    file("/repo/.gitignore", "*.o\nbuild/\n/top.txt\n");
    file("/repo/top.txt");
    file("/repo/main.cpp");
    file("/repo/main.o");
    file("/repo/build/out.bin");
    file("/repo/src/.gitignore", "!keep.o\ngen/\n");
    file("/repo/src/keep.o");
    file("/repo/src/drop.o");
    file("/repo/src/top.txt"); // "/top.txt" is anchored to the root.
    file("/repo/src/gen/a.cpp");
    file("/repo/src/deep/x.o");
    file("/repo/.git/HEAD");
    const auto result = scan();
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(relatives(*result), (std::vector<std::string>{".gitignore", "main.cpp", "src/", "src/.gitignore",
                                                            "src/deep/", "src/keep.o", "src/top.txt"}));
    EXPECT_TRUE(result->issues.empty());
    // repo, src, src/deep: build/, src/gen/ and .git/ were never listed.
    EXPECT_EQ(result->directoriesListed, 3U);
    // Sorted by path, absolute in the file system's syntax.
    EXPECT_TRUE(std::ranges::is_sorted(result->entries, {}, &frost::io::ScanEntry::path));
    EXPECT_EQ(result->entries.front().path, "/repo/.gitignore");
}

TEST_F(TreeScanTest, ReportsIgnoredEntriesWhenAsked) {
    file("/repo/.gitignore", "*.log\nlogs/\n");
    file("/repo/a.log");
    file("/repo/logs/today.txt");
    file("/repo/b.txt");
    ScanOptions options;
    options.reportIgnored = true;
    const auto result = scan(options);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(relatives(*result),
              (std::vector<std::string>{".gitignore", "a.log (ignored)", "b.txt", "logs/ (ignored)"}));
}

TEST_F(TreeScanTest, RepositoryWideRulesSitBelowTheGitignoreFiles) {
    file("/repo/.gitignore", "!*.cache\n");
    file("/repo/a.cache");
    file("/repo/b.swp");
    file("/repo/sub/c.cache");
    ScanOptions options;
    options.ignore = IgnoreStack().pushed(IgnoreRules::parse("*.cache\n*.swp\n", {}, "info/exclude"));
    const auto result = scan(options);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(relatives(*result), (std::vector<std::string>{".gitignore", "a.cache", "sub/", "sub/c.cache"}));
}

// The project scan (ADR 0042): .ignore files above .gitignore, the default excludes below both, and the
// default skip names.
TEST_F(TreeScanTest, ProjectScanReadsIgnoreFilesAndTheDefaultExcludes) {
    file("/repo/.gitignore", "*.log\n!keep.log\n");
    file("/repo/.ignore", "secret/\n!build/\n");
    file("/repo/keep.log");
    file("/repo/drop.log");
    file("/repo/main.cpp");
    file("/repo/secret/key.txt");
    file("/repo/build/kept.txt"); // Re-included by .ignore over the default "build/".
    file("/repo/out/a.obj");
    file("/repo/cmake-build-debug/x.txt");
    file("/repo/src/Debug/b.obj");
    file("/repo/.git/HEAD");
    file("/repo/.vs/state.json");
    ScanOptions options;
    options.ignore = IgnoreStack().pushed(frost::io::defaultProjectExcludes());
    options.extraIgnoreFiles = {".ignore"};
    options.skipNames = frost::io::defaultProjectSkipNames();
    const auto result = scan(options);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(relatives(*result), (std::vector<std::string>{".gitignore", ".ignore", "build/", "build/kept.txt",
                                                            "keep.log", "main.cpp", "src/"}));
    EXPECT_TRUE(result->issues.empty());
}

TEST_F(TreeScanTest, ProblemsBecomeIssuesAndTheWalkGoesOn) {
    file("/repo/.gitignore", "[broken\nok.tmp\n");
    file("/repo/ok.tmp");
    file("/repo/locked/secret.txt");
    file("/repo/other/.gitignore", "*.txt\n");
    file("/repo/other/visible.txt");
    file("/repo/fine/a.txt");
    fs.injectFault(FsOp::List, "/repo/locked", ErrorCode::PermissionDenied);
    fs.injectFault(FsOp::Read, "/repo/other/.gitignore", ErrorCode::IoError);
    const auto result = scan();
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    // "other/.gitignore" could not be read, so other/visible.txt is not ignored.
    EXPECT_EQ(relatives(*result), (std::vector<std::string>{".gitignore", "fine/", "fine/a.txt", "locked/", "other/",
                                                            "other/.gitignore", "other/visible.txt"}));
    ASSERT_EQ(result->issues.size(), 3U);
    const auto has = [&](ScanIssueKind kind, std::string_view path, ErrorCode code) {
        return std::ranges::any_of(result->issues, [&](const frost::io::ScanIssue& issue) {
            return issue.kind == kind && issue.path == path && issue.error.code() == code;
        });
    };
    EXPECT_TRUE(has(ScanIssueKind::ListFailed, "/repo/locked", ErrorCode::PermissionDenied));
    EXPECT_TRUE(has(ScanIssueKind::IgnoreFileFailed, "/repo/other/.gitignore", ErrorCode::IoError));
    EXPECT_TRUE(has(ScanIssueKind::IgnoreRule, "/repo/.gitignore", ErrorCode::InvalidArgument));
}

TEST_F(TreeScanTest, NamesThatAreNotUtf8AreReported) {
    directory("/repo");
    if (!fs.writeFileAtomic("/repo/bad\xFF"
                            "name",
                            "x")
             .has_value()) {
        GTEST_SKIP() << "this MemFs refuses names that are not UTF-8";
    }
    const auto result = scan();
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    ASSERT_EQ(result->issues.size(), 1U);
    EXPECT_EQ(result->issues[0].kind, ScanIssueKind::NameSkipped);
    EXPECT_EQ(result->issues[0].path, "/repo");
    EXPECT_EQ(result->entries.size(), 1U); // Listed as raw bytes, as on POSIX.
}

TEST_F(TreeScanTest, AtomicSaveTempFilesAreSkippedUnlessAsked) {
    file("/repo/a.txt");
    file("/repo/.a.txt.frost-tmp-12-3");
    file("/repo/.d.frost-tmp-1-1/x.txt");
    auto result = scan();
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(relatives(*result), (std::vector<std::string>{"a.txt"}));
    ScanOptions all;
    all.skipFrostTempFiles = false;
    result = scan(all);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(relatives(*result), (std::vector<std::string>{".a.txt.frost-tmp-12-3", ".d.frost-tmp-1-1/",
                                                            ".d.frost-tmp-1-1/x.txt", "a.txt"}));
}

TEST_F(TreeScanTest, RecursionTheDescendFilterAndStatuses) {
    file("/repo/a.txt", "12345");
    file("/repo/sub/b.txt", "1");
    file("/repo/skip/c.txt");
    ScanOptions flat;
    flat.recursive = false;
    auto result = scan(flat);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(relatives(*result), (std::vector<std::string>{"a.txt", "skip/", "sub/"}));

    ScanOptions filtered;
    filtered.descend = [](std::string_view directory) { return directory != "/repo/skip"; };
    filtered.withStatus = true;
    result = scan(filtered);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(relatives(*result), (std::vector<std::string>{"a.txt", "skip/", "sub/", "sub/b.txt"}));
    for (const auto& entry : result->entries) {
        if (entry.relative == "a.txt") {
            EXPECT_EQ(entry.status.size, 5U);
            EXPECT_EQ(entry.status.type, FileType::Regular);
        }
        if (entry.relative == "skip") {
            EXPECT_EQ(entry.status.type, FileType::Directory); // Not descended: only the type is set.
        }
    }
}

TEST_F(TreeScanTest, ErrorsAndCancellation) {
    file("/repo/a/b/c.txt");
    // The root's own listing error ends the scan.
    EXPECT_EQ(scan({}, "/missing").error().code(), ErrorCode::NotFound);
    EXPECT_EQ(scan({}, "/repo/a/b/c.txt").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(scan({}, "relative/root").error().code(), ErrorCode::InvalidArgument);
    ScanOptions none;
    none.parallelism = 0;
    EXPECT_EQ(scan(none).error().code(), ErrorCode::InvalidArgument);
    std::stop_source stop;
    static_cast<void>(stop.request_stop());
    EXPECT_EQ(scan({}, "/repo", 0, stop.get_token()).error().code(), ErrorCode::Cancelled);
    // A stop during the walk, with directories still queued and none in flight.
    std::stop_source midway;
    ScanOptions stopping;
    stopping.parallelism = 1;
    stopping.descend = [&midway](std::string_view) {
        static_cast<void>(midway.request_stop());
        return true;
    };
    EXPECT_EQ(scan(stopping, "/repo", 0, midway.get_token()).error().code(), ErrorCode::Cancelled);
}

TEST_F(TreeScanTest, TheResultDoesNotDependOnScheduleOrParallelism) {
    // 40 directories x 3 levels with files and a .gitignore every few directories.
    for (int a = 0; a < 6; ++a) {
        for (int b = 0; b < 6; ++b) {
            const std::string dir = std::format("/repo/d{}/e{}", a, b);
            file(dir + "/keep.cpp");
            file(dir + "/drop.o");
            file(dir + std::format("/f{}/leaf.txt", (a + b) % 3));
            if ((a + b) % 4 == 0) {
                file(dir + "/.gitignore", "*.txt\n!f0/\n");
            }
        }
    }
    file("/repo/.gitignore", "*.o\n");
    const auto reference = scan();
    ASSERT_TRUE(reference.has_value()) << reference.error().describe();
    for (std::uint64_t seed = 1; seed <= 8; ++seed) {
        ScanOptions options;
        options.parallelism = 1 + static_cast<unsigned>(seed % 5);
        const auto other = scan(options, "/repo", seed);
        ASSERT_TRUE(other.has_value()) << other.error().describe();
        EXPECT_EQ(other->entries, reference->entries) << "seed " << seed;
        EXPECT_EQ(other->directoriesListed, reference->directoriesListed);
    }
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 4});
    ScanOptions wide;
    wide.parallelism = 16;
    wide.lane = Lane::UserVisible;
    const auto pooled = pool.spawn(Lane::UserVisible, frost::io::scanTree(fs, pool, "/repo", wide)).join();
    ASSERT_TRUE(pooled.has_value()) << pooled.error().describe();
    EXPECT_EQ(pooled->entries, reference->entries);
    pool.shutdown();
}

} // namespace
