// SPDX-License-Identifier: Apache-2.0
//
/*
 * Integration tests for the polling backend of RealFileWatcher (src/io/src/polling_watcher.cpp; ADR 0020)
 * on the real file system, through WatchOptions::forcePolling: creations, removals, same-size rewrites
 * (mtimes are set explicitly, so timestamp granularity cannot make the test flaky), directory symlinks
 * (never followed) and a directory whose listing changed only between two scans.
 */

#include "frost/io/file_system.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/io/path.hpp"

#include "support/scratch_dir.hpp"
#include "support/watch_mirror.hpp"
#include "watch_env.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

using namespace std::chrono_literals;
using frost::io::FileChange;
using frost::io::FileChangeBatch;
using frost::io::FileChangeKind;
using frost::io::WatchMode;
using frost::io::WatchOptions;
using frost::test::nativeJoin;
using frost::test::WatchEnv;

[[nodiscard]] WatchOptions polling() {
    WatchOptions options;
    options.forcePolling = true;
    options.pollInterval = 100ms;
    return options;
}

[[nodiscard]] bool has(const FileChangeBatch& batch, FileChangeKind kind, const std::string& path) {
    return std::ranges::any_of(batch.changes, [&](const FileChange& c) { return c.kind == kind && c.path == path; });
}

void setMtime(const std::string& path, std::filesystem::file_time_type when) {
    std::error_code error;
    std::filesystem::last_write_time(frost::io::toStdPath(path).value(), when, error);
    ASSERT_FALSE(error) << error.message();
}

TEST(PollingWatcher, CreateRewriteAndDelete) {
    frost::test::ScratchDir scratch("poll");
    WatchEnv env;
    auto watch = env.watch(scratch.path(), polling());
    ASSERT_TRUE(watch) << watch.error().describe();
    EXPECT_EQ((*watch)->status().mode, WatchMode::Polling);
    const std::string file = nativeJoin(scratch.path(), "f");

    frost::test::writePlain(file, "aaaa");
    ASSERT_TRUE(
        env.until(**watch, nullptr, [&](const FileChangeBatch& b) { return has(b, FileChangeKind::Created, file); }));
    EXPECT_EQ(env.batches.back().mode, WatchMode::Polling);

    // The same size, an explicit new mtime: only the mtime tells.
    const auto before = std::filesystem::last_write_time(frost::io::toStdPath(file).value());
    frost::test::writePlain(file, "bbbb");
    setMtime(file, before + 10s);
    ASSERT_TRUE(
        env.until(**watch, nullptr, [&](const FileChangeBatch& b) { return has(b, FileChangeKind::Modified, file); }));

    frost::test::removeAll(file);
    ASSERT_TRUE(
        env.until(**watch, nullptr, [&](const FileChangeBatch& b) { return has(b, FileChangeKind::Removed, file); }));
    watch->reset();
}

TEST(PollingWatcher, ConvergesWithTheMirror) {
    frost::test::ScratchDir scratch("poll-mirror");
    WatchEnv env;
    auto watch = env.watch(scratch.path(), polling());
    ASSERT_TRUE(watch);
    frost::test::MirrorOptions options{.separator = frost::test::kNativeSeparator};
    frost::test::WatchMirror mirror(frost::io::realFileSystem(), (*watch)->root(), options);
    mirror.load();
    frost::test::makeDirectory(nativeJoin(scratch.path(), "d"));
    frost::test::writePlain(nativeJoin(nativeJoin(scratch.path(), "d"), "x"), "x");
    frost::test::writePlain(nativeJoin(scratch.path(), "y"), "y");
    frost::test::renamePath(nativeJoin(scratch.path(), "y"), nativeJoin(scratch.path(), "z"));
    env.settle(**watch, mirror, (*watch)->root(), options);
    watch->reset();
}

TEST(PollingWatcher, ADirectoryWhoseListingChangedIsModified) {
    // The directory's mtime moves when an entry comes and goes between two scans; the consumer re-lists it.
    frost::test::ScratchDir scratch("poll-dir");
    const std::string dir = nativeJoin(scratch.path(), "d");
    frost::test::makeDirectory(dir);
    WatchEnv env;
    auto watch = env.watch(scratch.path(), polling());
    ASSERT_TRUE(watch);
    const auto before = std::filesystem::last_write_time(frost::io::toStdPath(dir).value());
    frost::test::writePlain(nativeJoin(dir, "transient"), "t");
    frost::test::removeAll(nativeJoin(dir, "transient"));
    setMtime(dir, before + 10s);
    ASSERT_TRUE(
        env.until(**watch, nullptr, [&](const FileChangeBatch& b) { return has(b, FileChangeKind::Modified, dir); }));
    watch->reset();
}

TEST(PollingWatcher, DirectorySymlinksAreNotFollowed) {
    frost::test::ScratchDir outside("poll-outside");
    frost::test::ScratchDir scratch("poll-link");
    frost::test::writePlain(nativeJoin(outside.path(), "secret"), "s");
    std::error_code error;
    std::filesystem::create_directory_symlink(frost::io::toStdPath(outside.path()).value(),
                                              frost::io::toStdPath(nativeJoin(scratch.path(), "link")).value(), error);
    if (error) {
        GTEST_SKIP() << "cannot create a directory symlink here: " << error.message();
    }
    WatchEnv env;
    auto watch = env.watch(scratch.path(), polling());
    ASSERT_TRUE(watch);
    frost::test::writePlain(nativeJoin(outside.path(), "later"), "l");
    const std::string marker = nativeJoin(scratch.path(), "marker");
    frost::test::writePlain(marker, "m");
    ASSERT_TRUE(
        env.until(**watch, nullptr, [&](const FileChangeBatch& b) { return frost::test::mentions(b, marker); }));
    for (const FileChangeBatch& batch : env.batches) {
        for (const FileChange& change : batch.changes) {
            EXPECT_EQ(change.path.find("later"), std::string::npos) << change.path;
        }
    }
    watch->reset();
    std::filesystem::remove(frost::io::toStdPath(nativeJoin(scratch.path(), "link")).value(), error);
}

} // namespace
