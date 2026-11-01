// SPDX-License-Identifier: Apache-2.0
//
/*
 * Integration tests for RealFileWatcher (frost/io/file_watcher.hpp; ADR 0020) on the real file system:
 * a one-thread Reactor, a two-worker ThreadPool and a scratch directory. Every scenario ends with the
 * fence quiescence of watch_env.hpp and a comparison of the consumer's mirror with a std::filesystem walk.
 * Windows adds long roots, renamed roots, the fallback chain and overflow through the test hooks and the
 * Winsock check; Linux adds the inotify limits, IN_Q_OVERFLOW, moved roots and network file systems.
 */

#include "frost/io/file_system.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/io/path.hpp"
#include "frost/io/reactor.hpp"
#include "frost/sched/sim_executor.hpp"

#include "support/scratch_dir.hpp"
#include "support/watch_mirror.hpp"
#include "watch_env.hpp"
#include "watcher_testing.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/vfs.h>

#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::io::DegradeReason;
using frost::io::FileChange;
using frost::io::FileChangeBatch;
using frost::io::FileChangeKind;
using frost::io::IDirectoryWatch;
using frost::io::WatchMode;
using frost::io::WatchOptions;
using frost::test::MirrorOptions;
using frost::test::nativeJoin;
using frost::test::WatchEnv;
using frost::test::WatchMirror;

constexpr char kSep = frost::test::kNativeSeparator;

[[nodiscard]] bool has(const FileChangeBatch& batch, FileChangeKind kind, const std::string& path) {
    return std::ranges::any_of(batch.changes, [&](const FileChange& c) { return c.kind == kind && c.path == path; });
}
[[nodiscard]] bool anyHas(const std::vector<FileChangeBatch>& batches, FileChangeKind kind, const std::string& path) {
    return std::ranges::any_of(batches, [&](const FileChangeBatch& b) { return has(b, kind, path); });
}

struct Watched {
    WatchEnv env;
    frost::test::ScratchDir scratch{"watch"};
    MirrorOptions mirrorOptions{.separator = kSep};
    std::unique_ptr<IDirectoryWatch> watch;
    std::unique_ptr<WatchMirror> mirror;

    explicit Watched(WatchOptions options = {}, std::string root = {}) {
        if (root.empty()) {
            root = scratch.path();
        }
        mirrorOptions.recursive = options.recursive;
        mirrorOptions.excluded = options.excluded;
        auto started = env.watch(root, std::move(options));
        if (!started) {
            ADD_FAILURE() << started.error().describe();
            return;
        }
        watch = std::move(*started);
        mirror = std::make_unique<WatchMirror>(frost::io::realFileSystem(), watch->root(), mirrorOptions);
        mirror->load();
    }
    ~Watched() { watch.reset(); }
    Watched(const Watched&) = delete ("owns the watch");
    Watched& operator=(const Watched&) = delete ("owns the watch");

    [[nodiscard]] std::string at(std::string_view name) const { return nativeJoin(watch->root(), name); }
    void settle(bool rescansExpected = false) {
        env.settle(*watch, *mirror, watch->root(), mirrorOptions, rescansExpected);
    }
    bool until(const std::function<bool(const FileChangeBatch&)>& done) {
        return env.until(*watch, mirror.get(), done);
    }
};

TEST(FileWatcher, CreateModifyAndDeleteAFile) {
    Watched w;
    ASSERT_TRUE(w.watch);
    const std::string file = w.at("a.txt");
    frost::test::writePlain(file, "one");
    ASSERT_TRUE(w.until([&](const FileChangeBatch& b) { return frost::test::mentions(b, file); }));
    frost::test::writePlain(file, "two, longer");
    ASSERT_TRUE(w.until([&](const FileChangeBatch& b) {
        return has(b, FileChangeKind::Modified, file) || has(b, FileChangeKind::Created, file);
    }));
    frost::test::removeAll(file);
    ASSERT_TRUE(w.until([&](const FileChangeBatch& b) { return has(b, FileChangeKind::Removed, file); }));
    w.settle();
    EXPECT_EQ(w.watch->status().mode, WatchMode::Native);
}

TEST(FileWatcher, NewDirectoryAndItsFilesAreSeen) {
    // On Linux the files race the watch the new directory needs; the setup listing closes the race.
    Watched w;
    ASSERT_TRUE(w.watch);
    const std::string dir = w.at("d");
    frost::test::makeDirectory(dir);
    for (int i = 0; i < 20; ++i) {
        frost::test::writePlain(nativeJoin(dir, std::format("f{}", i)), std::to_string(i));
    }
    frost::test::makeDirectory(nativeJoin(dir, "sub"));
    frost::test::writePlain(nativeJoin(nativeJoin(dir, "sub"), "deep"), "deep");
    w.settle();
}

TEST(FileWatcher, RenameWithinADirectoryIsRenamed) {
    Watched w;
    ASSERT_TRUE(w.watch);
    frost::test::writePlain(w.at("old"), "x");
    w.settle();
    frost::test::renamePath(w.at("old"), w.at("new"));
    ASSERT_TRUE(w.until([&](const FileChangeBatch& b) { return frost::test::mentions(b, w.at("new")); }));
    EXPECT_TRUE(anyHas(w.env.batches, FileChangeKind::Renamed, w.at("new")));
    w.settle();
}

TEST(FileWatcher, MoveBetweenDirectories) {
    Watched w;
    ASSERT_TRUE(w.watch);
    frost::test::makeDirectory(w.at("a"));
    frost::test::makeDirectory(w.at("b"));
    frost::test::writePlain(nativeJoin(w.at("a"), "f"), "x");
    w.settle();
    const std::string to = nativeJoin(w.at("b"), "f");
    frost::test::renamePath(nativeJoin(w.at("a"), "f"), to);
    ASSERT_TRUE(w.until([&](const FileChangeBatch& b) { return frost::test::mentions(b, to); }));
#ifdef _WIN32
    // ReadDirectoryChangesW reports a move between directories as a removal plus an addition.
    EXPECT_TRUE(
        anyHas(w.env.batches, FileChangeKind::Renamed, to) ||
        (anyHas(w.env.batches, FileChangeKind::Removed, nativeJoin(w.at("a"), "f")) &&
         (anyHas(w.env.batches, FileChangeKind::Created, to) || anyHas(w.env.batches, FileChangeKind::Modified, to))));
#else
    EXPECT_TRUE(anyHas(w.env.batches, FileChangeKind::Renamed, to)); // inotify pairs by cookie.
#endif
    w.settle();
}

TEST(FileWatcher, AtomicSaveArrivesAsModifiedWithoutTempFiles) {
    WatchOptions options;
    options.debounce = 200ms;
    Watched w(options);
    ASSERT_TRUE(w.watch);
    const std::string file = w.at("doc.txt");
    frost::test::writePlain(file, "v1");
    w.settle();
    w.env.batches.clear();
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(file, "version two"));
    ASSERT_TRUE(w.until([&](const FileChangeBatch& b) { return frost::test::mentions(b, file); }));
    for (const FileChangeBatch& batch : w.env.batches) {
        for (const FileChange& change : batch.changes) {
            EXPECT_EQ(change.path.find(".frost-tmp-"), std::string::npos) << change.path;
        }
    }
    EXPECT_TRUE(anyHas(w.env.batches, FileChangeKind::Modified, file) ||
                anyHas(w.env.batches, FileChangeKind::Rescan, file));
    w.settle();
}

TEST(FileWatcher, NonAsciiNames) {
    Watched w;
    ASSERT_TRUE(w.watch);
    const std::string dir = w.at("\xC3\xA4\xC3\xB6\xC3\xBC"); // äöü
    frost::test::makeDirectory(dir);
    frost::test::writePlain(nativeJoin(dir, "\xE6\x97\xA5\xE6\x9C\xAC.txt"), "nihon"); // 日本.txt
    frost::test::writePlain(w.at("\xF0\x9F\x98\x80"), "emoji");                        // U+1F600
    w.settle();
}

TEST(FileWatcher, ExcludedSubtreesAreNotReported) {
    frost::test::ScratchDir scratch("watch-excluded");
    const std::string excluded = nativeJoin(scratch.path(), "build");
    frost::test::makeDirectory(excluded);
    WatchOptions options;
    options.excluded = {excluded};
    Watched w(options, scratch.path());
    ASSERT_TRUE(w.watch);
    for (int i = 0; i < 10; ++i) {
        frost::test::writePlain(nativeJoin(excluded, std::format("out{}", i)), "o");
    }
    frost::test::writePlain(w.at("src.cpp"), "s");
    w.settle();
    for (const FileChangeBatch& batch : w.env.batches) {
        for (const FileChange& change : batch.changes) {
            EXPECT_FALSE(change.path.starts_with(excluded + kSep)) << change.path;
        }
    }
#ifndef _WIN32
    EXPECT_EQ(w.watch->status().osWatches, 1U); // The root only: the excluded directory is never watched.
#endif
}

TEST(FileWatcher, NonRecursiveWatchesSeeOnlyChildren) {
    WatchOptions options;
    options.recursive = false;
    frost::test::ScratchDir scratch("watch-flat");
    frost::test::makeDirectory(nativeJoin(scratch.path(), "sub"));
    Watched w(options, scratch.path());
    ASSERT_TRUE(w.watch);
    frost::test::writePlain(nativeJoin(w.at("sub"), "deep"), "d");
    frost::test::writePlain(w.at("top"), "t");
    w.settle();
}

TEST(FileWatcher, OverlappingWatchesBothConverge) {
    frost::test::ScratchDir scratch("watch-overlap");
    const std::string sub = nativeJoin(scratch.path(), "sub");
    frost::test::makeDirectory(sub);
    WatchEnv env;
    MirrorOptions mirrorOptions{.separator = kSep};
    auto outer = env.watch(scratch.path());
    auto inner = env.watch(sub);
    ASSERT_TRUE(outer && inner);
    WatchMirror outerMirror(frost::io::realFileSystem(), (*outer)->root(), mirrorOptions);
    WatchMirror innerMirror(frost::io::realFileSystem(), (*inner)->root(), mirrorOptions);
    outerMirror.load();
    innerMirror.load();
    frost::test::writePlain(nativeJoin(sub, "shared"), "s");
    frost::test::writePlain(nativeJoin(scratch.path(), "outer-only"), "o");
    env.settle(**outer, outerMirror, (*outer)->root(), mirrorOptions);
    env.settle(**inner, innerMirror, (*inner)->root(), mirrorOptions);
    inner->reset();
    outer->reset();
}

TEST(FileWatcher, BadRoots) {
    WatchEnv env;
    frost::test::ScratchDir scratch("watch-bad");
    const std::string file = nativeJoin(scratch.path(), "file");
    frost::test::writePlain(file, "x");
    auto onFile = env.watch(file);
    ASSERT_FALSE(onFile);
    EXPECT_EQ(onFile.error().code(), ErrorCode::InvalidArgument);
    auto missing = env.watch(nativeJoin(scratch.path(), "missing"));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    auto relative = env.watch("relative");
    ASSERT_FALSE(relative);
    EXPECT_EQ(relative.error().code(), ErrorCode::InvalidArgument);
}

TEST(FileWatcher, DeletingTheRootEndsTheWatchAndFreesThePath) {
    frost::test::ScratchDir scratch("watch-root");
    const std::string root = nativeJoin(scratch.path(), "root");
    frost::test::makeDirectory(root);
    frost::test::writePlain(nativeJoin(root, "f"), "x");
    WatchEnv env;
    auto watch = env.watch(root);
    ASSERT_TRUE(watch);
    frost::test::removeAll(root);
    ASSERT_TRUE(
        env.until(**watch, nullptr, [&](const FileChangeBatch& b) { return has(b, FileChangeKind::Removed, root); }));
    auto after = env.next(**watch, 10s);
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().code(), ErrorCode::NotFound);
    // The watch no longer holds the directory: the path can be created again at once.
    std::error_code error;
    EXPECT_TRUE(std::filesystem::create_directory(frost::io::toStdPath(root).value(), error)) << error.message();
    watch->reset();
}

TEST(FileWatcher, DestroyingTheWatchCancelsAPendingNext) {
    WatchEnv env;
    frost::test::ScratchDir scratch("watch-destroy");
    auto watch = env.watch(scratch.path());
    ASSERT_TRUE(watch);
    auto handle = env.pool().spawn(frost::sched::Lane::UserVisible, (*watch)->next());
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(handle.ready());
    watch->reset();
    auto result = handle.join();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
}

TEST(FileWatcher, ReactorShutdownCancelsPendingNext) {
    WatchEnv env;
    frost::test::ScratchDir scratch("watch-shutdown");
    auto watch = env.watch(scratch.path());
    ASSERT_TRUE(watch);
    auto handle = env.pool().spawn(frost::sched::Lane::UserVisible, (*watch)->next());
    env.reactor().shutdown();
    auto result = handle.join();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Cancelled);
    EXPECT_EQ(env.reactor().stats().shutDown, true);
    watch->reset();
}

TEST(FileWatcher, ForcedPollingIsReportedAndWorks) {
    WatchOptions options;
    options.forcePolling = true;
    options.pollInterval = 100ms;
    Watched w(options);
    ASSERT_TRUE(w.watch);
    EXPECT_EQ(w.watch->status().mode, WatchMode::Polling);
    EXPECT_EQ(w.watch->status().reason, DegradeReason::Forced);
    frost::test::writePlain(w.at("p"), "x");
    w.settle();
}

#ifdef _WIN32

TEST(FileWatcherWindows, WinsockIsNeverLoaded) {
    {
        Watched w;
        ASSERT_TRUE(w.watch);
        frost::test::writePlain(w.at("x"), "x");
        w.settle();
    }
    EXPECT_EQ(GetModuleHandleW(L"ws2_32.dll"), nullptr);
}

TEST(FileWatcherWindows, RootsBeyondMaxPath) {
    frost::test::ScratchDir scratch("watch-long");
    std::string root = scratch.path();
    while (root.size() < 300) {
        root = nativeJoin(root, std::string(40, 'l'));
    }
    frost::test::makeDirectory(root);
    Watched w({}, root);
    ASSERT_TRUE(w.watch);
    frost::test::writePlain(w.at("deep.txt"), "deep");
    w.settle();
}

TEST(FileWatcherWindows, RenamingTheRootEndsTheWatch) {
    frost::test::ScratchDir scratch("watch-rename-root");
    const std::string root = nativeJoin(scratch.path(), "root");
    frost::test::makeDirectory(root);
    WatchEnv env;
    auto watch = env.watch(root);
    ASSERT_TRUE(watch);
    // While watched, the directory handle (FILE_SHARE_DELETE) lets the root itself be renamed.
    std::error_code error;
    std::filesystem::rename(frost::io::toStdPath(root).value(),
                            frost::io::toStdPath(nativeJoin(scratch.path(), "moved")).value(), error);
    if (error) {
        GTEST_SKIP() << "renaming a watched directory failed here: " << error.message();
    }
    ASSERT_TRUE(
        env.until(**watch, nullptr, [&](const FileChangeBatch& b) { return has(b, FileChangeKind::Removed, root); }));
    for (const FileChangeBatch& batch : env.batches) {
        for (const FileChange& change : batch.changes) {
            EXPECT_TRUE(change.path == root) << "an event under the old prefix: " << change.path;
        }
    }
    auto after = env.next(**watch, 10s);
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().code(), ErrorCode::NotFound);
    watch->reset();
}

TEST(FileWatcherWindows, RenamingAnAncestorOfTheRoot) {
    // Recorded behaviour: Windows refuses to rename a directory with an open handle below it.
    frost::test::ScratchDir scratch("watch-ancestor");
    const std::string parent = nativeJoin(scratch.path(), "parent");
    const std::string root = nativeJoin(parent, "root");
    frost::test::makeDirectory(root);
    WatchEnv env;
    auto watch = env.watch(root);
    ASSERT_TRUE(watch);
    std::error_code error;
    std::filesystem::rename(frost::io::toStdPath(parent).value(),
                            frost::io::toStdPath(nativeJoin(scratch.path(), "moved")).value(), error);
    RecordProperty("ancestor_rename_error", error ? error.message() : std::string("none"));
    if (!error) {
        ASSERT_TRUE(env.until(**watch, nullptr,
                              [&](const FileChangeBatch& b) { return has(b, FileChangeKind::Removed, root); }));
    }
    watch->reset();
}

struct FaultReset {
    ~FaultReset() {
        using frost::io::testing::WindowsWatchFault;
        for (const WindowsWatchFault fault : {WindowsWatchFault::NotifyEnumDir, WindowsWatchFault::ExRefused,
                                              WindowsWatchFault::PlainRefused, WindowsWatchFault::ReadError}) {
            frost::io::testing::injectWindowsWatchFaultForTesting(fault, 0);
        }
        frost::io::testing::setWindowsReadBufferBytesForTesting(0);
    }
};

TEST(FileWatcherWindows, NotifyEnumDirBecomesRescan) {
    FaultReset reset;
    Watched w;
    ASSERT_TRUE(w.watch);
    frost::io::testing::injectWindowsWatchFaultForTesting(frost::io::testing::WindowsWatchFault::NotifyEnumDir, 1);
    frost::test::writePlain(w.at("trigger"), "x");
    ASSERT_TRUE(w.until([&](const FileChangeBatch& b) { return has(b, FileChangeKind::Rescan, w.watch->root()); }));
    EXPECT_EQ(w.watch->status().mode, WatchMode::Native);
    w.settle(/*rescansExpected=*/true);
}

TEST(FileWatcherWindows, ExRefusedFallsBackToPlainReads) {
    FaultReset reset;
    frost::io::testing::injectWindowsWatchFaultForTesting(frost::io::testing::WindowsWatchFault::ExRefused, 1);
    Watched w;
    ASSERT_TRUE(w.watch);
    EXPECT_EQ(w.watch->status().mode, WatchMode::Native);
    EXPECT_NE(w.watch->status().detail.find("ReadDirectoryChangesW"), std::string::npos) << w.watch->status().detail;
    frost::test::writePlain(w.at("a"), "x");
    frost::test::renamePath(w.at("a"), w.at("b"));
    w.settle();
}

TEST(FileWatcherWindows, PlainRefusedFallsBackToPolling) {
    FaultReset reset;
    frost::io::testing::injectWindowsWatchFaultForTesting(frost::io::testing::WindowsWatchFault::ExRefused, 1);
    frost::io::testing::injectWindowsWatchFaultForTesting(frost::io::testing::WindowsWatchFault::PlainRefused, 1);
    WatchOptions options;
    options.pollInterval = 100ms;
    Watched w(options);
    ASSERT_TRUE(w.watch);
    EXPECT_EQ(w.watch->status().mode, WatchMode::Polling);
    EXPECT_EQ(w.watch->status().reason, DegradeReason::Unsupported);
    frost::test::writePlain(w.at("a"), "x");
    w.settle();
}

TEST(FileWatcherWindows, RepeatedReadErrorsFallBackToPolling) {
    FaultReset reset;
    WatchOptions options;
    options.pollInterval = 100ms;
    Watched w(options);
    ASSERT_TRUE(w.watch);
    frost::io::testing::injectWindowsWatchFaultForTesting(frost::io::testing::WindowsWatchFault::ReadError, 3);
    // Each failing completion needs a change to complete the read it fails.
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    for (int i = 0; w.watch->status().mode != WatchMode::Polling && std::chrono::steady_clock::now() < deadline; ++i) {
        frost::test::writePlain(w.at(std::format("trigger{}", i)), "x");
        std::this_thread::sleep_for(150ms); // Longer than the 100 ms retry delay.
    }
    ASSERT_TRUE(w.until([&](const FileChangeBatch& b) { return b.mode == WatchMode::Polling; }));
    EXPECT_EQ(w.watch->status().reason, DegradeReason::RepeatedErrors);
    frost::test::writePlain(w.at("after"), "y");
    w.settle(/*rescansExpected=*/true); // The switch to polling hands out Rescan(root).
}

TEST(FileWatcherWindows, RealOverflowBecomesRescan) {
    FaultReset reset;
    frost::io::testing::setWindowsReadBufferBytesForTesting(4096);
    Watched w;
    ASSERT_TRUE(w.watch);
    {
        frost::io::testing::ReactorStall stall(w.env.reactor());
        for (int i = 0; i < 2000; ++i) {
            frost::test::writePlain(w.at(std::format("burst-{:04}", i)), "b");
        }
    }
    ASSERT_TRUE(w.until([&](const FileChangeBatch& b) { return has(b, FileChangeKind::Rescan, w.watch->root()); }));
    w.settle(/*rescansExpected=*/true);
}

TEST(FileWatcherWindows, ShortNamesAreExpandedAndTildesKept) {
    Watched w;
    ASSERT_TRUE(w.watch);
    frost::test::writePlain(w.at("file.txt~"), "backup");
    frost::test::writePlain(w.at("~$x.docx"), "lock");
    // A long name created through its 8.3 spelling (when the volume makes 8.3 names).
    const std::string longName = w.at("a long directory name");
    frost::test::makeDirectory(longName);
    std::wstring shortName(1024, L'\0');
    const auto wide = frost::io::toWin32Path(longName).value();
    const DWORD length = GetShortPathNameW(wide.c_str(), shortName.data(), static_cast<DWORD>(shortName.size()));
    w.settle();
    for (const FileChangeBatch& batch : w.env.batches) {
        for (const FileChange& change : batch.changes) {
            EXPECT_NE(change.kind, FileChangeKind::Rescan) << change.path;
        }
    }
    if (length == 0 || length >= shortName.size() || shortName.find(L'~') == std::wstring::npos) {
        GTEST_SKIP() << "this volume makes no 8.3 names";
    }
    shortName.resize(length);
    const std::filesystem::path viaShort = std::filesystem::path(shortName) / L"inner.txt";
    {
        std::FILE* file = nullptr;
        ASSERT_EQ(_wfopen_s(&file, viaShort.c_str(), L"wb"), 0);
        std::fclose(file);
    }
    const std::string expanded = nativeJoin(longName, "inner.txt");
    ASSERT_TRUE(w.until([&](const FileChangeBatch& b) { return frost::test::mentions(b, expanded); }));
    w.settle();
}

#else // Linux

TEST(FileWatcherLinux, WatchLimitFallsBackToPolling) {
    frost::test::ScratchDir scratch("watch-limit");
    for (int i = 0; i < 20; ++i) {
        frost::test::makeDirectory(nativeJoin(scratch.path(), std::format("d{}", i)));
    }
    frost::io::testing::setInotifyWatchLimitForTesting(5);
    WatchOptions options;
    options.pollInterval = 200ms;
    Watched w(options, scratch.path());
    frost::io::testing::setInotifyWatchLimitForTesting(0);
    ASSERT_TRUE(w.watch);
    EXPECT_EQ(w.watch->status().mode, WatchMode::Polling);
    EXPECT_EQ(w.watch->status().reason, DegradeReason::WatchLimit);
    frost::test::writePlain(nativeJoin(w.at("d7"), "found"), "x");
    w.settle();
}

TEST(FileWatcherLinux, InstanceLimitFallsBackToPolling) {
    frost::io::testing::setInotifyInstanceLimitForTesting(true);
    WatchOptions options;
    options.pollInterval = 200ms;
    Watched w(options);
    frost::io::testing::setInotifyInstanceLimitForTesting(false);
    ASSERT_TRUE(w.watch);
    EXPECT_EQ(w.watch->status().mode, WatchMode::Polling);
    EXPECT_EQ(w.watch->status().reason, DegradeReason::InstanceLimit);
    frost::test::writePlain(w.at("x"), "x");
    w.settle();
}

TEST(FileWatcherLinux, MovingTheRootAwayEndsTheWatch) {
    frost::test::ScratchDir scratch("watch-move-root");
    const std::string root = nativeJoin(scratch.path(), "root");
    frost::test::makeDirectory(root);
    WatchEnv env;
    auto watch = env.watch(root);
    ASSERT_TRUE(watch);
    frost::test::renamePath(root, nativeJoin(scratch.path(), "moved"));
    ASSERT_TRUE(
        env.until(**watch, nullptr, [&](const FileChangeBatch& b) { return has(b, FileChangeKind::Removed, root); }));
    auto after = env.next(**watch, 10s);
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().code(), ErrorCode::NotFound);
    watch->reset();
}

TEST(FileWatcherLinux, UnreadableDirectoriesAreCounted) {
    if (geteuid() == 0) {
        GTEST_SKIP() << "root reads every directory";
    }
    frost::test::ScratchDir scratch("watch-unreadable");
    const std::string locked = nativeJoin(scratch.path(), "locked");
    frost::test::makeDirectory(locked);
    ASSERT_EQ(chmod(locked.c_str(), 0), 0);
    Watched w({}, scratch.path());
    ASSERT_TRUE(w.watch);
    EXPECT_EQ(w.watch->status().mode, WatchMode::Native);
    EXPECT_GE(w.watch->status().unreadableDirectories, 1U);
    frost::test::writePlain(w.at("x"), "x");
    ASSERT_TRUE(w.until([&](const FileChangeBatch& b) { return frost::test::mentions(b, w.at("x")); }));
    ASSERT_EQ(chmod(locked.c_str(), 0700), 0);
}

TEST(FileWatcherLinux, ADirectoryMovedInFromAnExcludedPlaceIsWatched) {
    frost::test::ScratchDir scratch("watch-move-in");
    const std::string excluded = nativeJoin(scratch.path(), "out");
    frost::test::makeDirectory(nativeJoin(excluded, "pkg"));
    WatchOptions options;
    options.excluded = {excluded};
    Watched w(options, scratch.path());
    ASSERT_TRUE(w.watch);
    // Its MovedFrom happened in an unwatched directory: an unpaired directory MovedTo is Rescan(pkg).
    frost::test::renamePath(nativeJoin(excluded, "pkg"), w.at("pkg"));
    w.settle(/*rescansExpected=*/true);
    frost::test::writePlain(nativeJoin(w.at("pkg"), "later"), "l");
    ASSERT_TRUE(
        w.until([&](const FileChangeBatch& b) { return frost::test::mentions(b, nativeJoin(w.at("pkg"), "later")); }));
    w.settle(/*rescansExpected=*/true);
}

TEST(FileWatcherLinux, RealQueueOverflowBecomesRescan) {
    std::uint64_t limit = 16384;
    if (std::FILE* file = std::fopen("/proc/sys/fs/inotify/max_queued_events", "r"); file != nullptr) {
        unsigned long long value = 0;
        if (std::fscanf(file, "%llu", &value) == 1) {
            limit = value;
        }
        std::fclose(file);
    }
    if (limit > 200'000) {
        GTEST_SKIP() << "max_queued_events is " << limit << ", too many files for a test";
    }
    Watched w;
    ASSERT_TRUE(w.watch);
    {
        frost::io::testing::ReactorStall stall(w.env.reactor());
        for (std::uint64_t i = 0; i < limit / 2 + 100; ++i) {
            frost::test::writePlain(w.at(std::format("q{}", i)), "q"); // IN_CREATE + IN_MODIFY + IN_CLOSE_WRITE.
        }
    }
    ASSERT_TRUE(w.until([&](const FileChangeBatch& b) { return has(b, FileChangeKind::Rescan, w.watch->root()); }));
    w.settle(/*rescansExpected=*/true);
}

TEST(FileWatcherLinux, NetworkFileSystemsArePolled) {
    struct statfs info{};
    if (statfs("/mnt/c", &info) != 0 || static_cast<std::uint64_t>(info.f_type) != 0x01021997ULL) {
        GTEST_SKIP() << "no 9p /mnt/c here";
    }
    WatchEnv env;
    WatchOptions options;
    options.recursive = false;
    auto watch = env.watch("/mnt/c", options);
    ASSERT_TRUE(watch) << watch.error().describe();
    EXPECT_EQ((*watch)->status().mode, WatchMode::Polling);
    EXPECT_EQ((*watch)->status().reason, DegradeReason::NetworkFileSystem);
    EXPECT_EQ((*watch)->status().reasonValue, 0x01021997ULL);
    watch->reset();
}

#endif

TEST(FileWatcher, CreateRefusesSimExecutor) {
    auto reactor = frost::io::Reactor::create();
    ASSERT_TRUE(reactor);
    frost::sched::SimExecutor sim;
    auto watcher = frost::io::RealFileWatcher::create(**reactor, sim);
    ASSERT_FALSE(watcher);
    EXPECT_EQ(watcher.error().code(), ErrorCode::InvalidArgument);
}

} // namespace
