// SPDX-License-Identifier: Apache-2.0
//
/*
 * Tests for MemFsWatcher (frost/io/mem_fs_watcher.hpp; ADR 0020) and MemFs's observer events, rename and
 * writeInPlace: the inotify-like event sequences, batches in virtual time, removal of the root, the test
 * controls (overflow, failure, degradation), watch destruction while next() is pending, and an end-to-end
 * property test (random mutations, batches applied by the reference consumer with lag, determinism).
 */

#include "frost/io/mem_fs.hpp"
#include "frost/io/mem_fs_watcher.hpp"
#include "frost/sched/sim_executor.hpp"

#include "support/watch_mirror.hpp"
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::io::DegradeReason;
using frost::io::FileChange;
using frost::io::FileChangeBatch;
using frost::io::FileChangeKind;
using frost::io::FileType;
using frost::io::IDirectoryWatch;
using frost::io::MemFs;
using frost::io::MemFsEvent;
using frost::io::MemFsEventKind;
using frost::io::MemFsWatcher;
using frost::io::WatchMode;
using frost::io::WatchOptions;
using frost::sched::Lane;
using frost::sched::SimExecutor;

struct EventLog final : frost::io::IMemFsObserver {
    std::vector<MemFsEvent> events;
    void onMemFsEvents(std::span<const MemFsEvent> batch) noexcept override {
        events.insert(events.end(), batch.begin(), batch.end());
    }
};

TEST(MemFsEvents, SequencesAreInotifyLike) {
    frost::ManualClock clock;
    MemFs fs(clock, frost::io::PathStyle::Posix);
    ASSERT_TRUE(fs.createDirectories("/w/d"));
    EventLog log;
    fs.addObserver(log);
    ASSERT_TRUE(fs.writeFileAtomic("/w/f", "1"));
    ASSERT_EQ(log.events.size(), 4U);
    EXPECT_EQ(log.events[0].kind, MemFsEventKind::Created);
    EXPECT_NE(log.events[0].path.find(".frost-tmp-"), std::string::npos);
    EXPECT_EQ(log.events[1].kind, MemFsEventKind::Modified);
    EXPECT_EQ(log.events[2].kind, MemFsEventKind::MovedFrom);
    EXPECT_EQ(log.events[3], (MemFsEvent{MemFsEventKind::MovedTo, "/w/f", FileType::Regular, log.events[2].cookie}));
    EXPECT_NE(log.events[2].cookie, 0U);
    log.events.clear();

    ASSERT_TRUE(fs.writeInPlace("/w/f", "2"));
    EXPECT_EQ(log.events, (std::vector{MemFsEvent{MemFsEventKind::Modified, "/w/f", FileType::Regular, 0}}));
    EXPECT_EQ(fs.readFile("/w/f").value(), "2");
    log.events.clear();

    ASSERT_TRUE(fs.writeInPlace("/w/g", "new"));
    EXPECT_EQ(log.events, (std::vector{MemFsEvent{MemFsEventKind::Created, "/w/g", FileType::Regular, 0},
                                       MemFsEvent{MemFsEventKind::Modified, "/w/g", FileType::Regular, 0}}));
    log.events.clear();

    ASSERT_TRUE(fs.createDirectories("/w/x/y"));
    EXPECT_EQ(log.events, (std::vector{MemFsEvent{MemFsEventKind::Created, "/w/x", FileType::Directory, 0},
                                       MemFsEvent{MemFsEventKind::Created, "/w/x/y", FileType::Directory, 0}}));
    log.events.clear();

    ASSERT_TRUE(fs.rename("/w/x", "/w/z"));
    ASSERT_EQ(log.events.size(), 2U);
    EXPECT_EQ(log.events[0].kind, MemFsEventKind::MovedFrom);
    EXPECT_EQ(log.events[1], (MemFsEvent{MemFsEventKind::MovedTo, "/w/z", FileType::Directory, log.events[0].cookie}));
    EXPECT_TRUE(fs.exists("/w/z/y").value());
    EXPECT_FALSE(fs.exists("/w/x").value());
    log.events.clear();

    ASSERT_TRUE(fs.removeFile("/w/g"));
    ASSERT_TRUE(fs.removeDirectory("/w/z/y"));
    EXPECT_EQ(log.events, (std::vector{MemFsEvent{MemFsEventKind::Removed, "/w/g", FileType::Regular, 0},
                                       MemFsEvent{MemFsEventKind::Removed, "/w/z/y", FileType::Directory, 0}}));
    fs.removeObserver(log);
}

TEST(MemFsEvents, RenameRules) {
    frost::ManualClock clock;
    MemFs fs(clock, frost::io::PathStyle::Posix);
    ASSERT_TRUE(fs.createDirectories("/w/d/e"));
    ASSERT_TRUE(fs.writeFileAtomic("/w/f", "f"));
    ASSERT_TRUE(fs.writeFileAtomic("/w/g", "g"));
    EXPECT_EQ(fs.rename("/w/missing", "/w/x").error().code(), ErrorCode::NotFound);
    EXPECT_EQ(fs.rename("/w/d", "/w/d/e/inside").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(fs.rename("/w/f", "/w/d").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(fs.rename("/w/d/e", "/w/f").error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(fs.rename("/w/f", "/w/nope/x").error().code(), ErrorCode::NotFound);
    ASSERT_TRUE(fs.createDirectories("/w/empty"));
    ASSERT_TRUE(fs.rename("/w/d/e", "/w/empty")); // Onto an empty directory.
    ASSERT_TRUE(fs.createDirectories("/w/full"));
    ASSERT_TRUE(fs.writeFileAtomic("/w/full/x", "x"));
    EXPECT_EQ(fs.rename("/w/d", "/w/full").error().code(), ErrorCode::IoError); // Not empty.
    ASSERT_TRUE(fs.rename("/w/f", "/w/g"));                                     // Replaces a file.
    EXPECT_EQ(fs.readFile("/w/g").value(), "f");
    EXPECT_TRUE(fs.rename("/w/g", "/w/g")); // Onto itself: nothing happens.
    fs.injectFault(frost::io::FsOp::Rename, "/w/g", ErrorCode::Busy);
    EXPECT_EQ(fs.rename("/w/g", "/w/h").error().code(), ErrorCode::Busy);
}

struct WatcherTest : ::testing::Test {
    frost::ManualClock clock; // Unused by the watcher (MemFs mtimes only).
    MemFs fs{clock, frost::io::PathStyle::Posix};
    SimExecutor sim;
    MemFsWatcher watcher{fs, sim};
    std::unique_ptr<IDirectoryWatch> watch;

    void SetUp() override { ASSERT_TRUE(fs.createDirectories("/w/d")); }
    void TearDown() override {
        watch.reset();
        sim.runUntilIdle();
    }

    void start(WatchOptions options = {}, std::string root = "/w") {
        auto handle = sim.spawn(Lane::UserVisible, watcher.watch(std::move(root), std::move(options)));
        auto result = handle.join();
        ASSERT_TRUE(result) << result.error().describe();
        watch = std::move(*result);
    }
    frost::Result<FileChangeBatch> nextBatch() {
        auto handle = sim.spawn(Lane::UserVisible, watch->next());
        sim.runFor(1s);
        EXPECT_TRUE(handle.ready());
        if (!handle.ready()) {
            return frost::fail(ErrorCode::Timeout, "no batch");
        }
        return handle.join();
    }
};

TEST_F(WatcherTest, AtomicSaveArrivesAsModified) {
    start();
    ASSERT_TRUE(fs.writeFileAtomic("/w/d/f", "x"));
    auto batch = nextBatch();
    ASSERT_TRUE(batch) << batch.error().describe();
    EXPECT_EQ(batch->changes, (std::vector{FileChange{FileChangeKind::Modified, "/w/d/f", {}, FileType::Regular}}));
}

TEST_F(WatcherTest, RenamesAndDirectories) {
    ASSERT_TRUE(fs.writeFileAtomic("/w/d/f", "x"));
    start();
    ASSERT_TRUE(fs.rename("/w/d/f", "/w/d/g"));
    ASSERT_TRUE(fs.createDirectories("/w/n"));
    auto batch = nextBatch();
    ASSERT_TRUE(batch);
    EXPECT_EQ(batch->changes, (std::vector{FileChange{FileChangeKind::Renamed, "/w/d/g", "/w/d/f", FileType::Regular},
                                           FileChange{FileChangeKind::Created, "/w/n", {}, FileType::Directory}}));
}

TEST_F(WatcherTest, RemovingTheRootEndsTheWatch) {
    start({}, "/w/d");
    ASSERT_TRUE(fs.writeInPlace("/w/d/f", "x"));
    ASSERT_TRUE(fs.removeFile("/w/d/f"));
    ASSERT_TRUE(fs.removeDirectory("/w/d"));
    auto batch = nextBatch();
    ASSERT_TRUE(batch);
    EXPECT_EQ(batch->changes, (std::vector{FileChange{FileChangeKind::Removed, "/w/d", {}, FileType::Directory}}));
    auto after = nextBatch();
    ASSERT_FALSE(after);
    EXPECT_EQ(after.error().code(), ErrorCode::NotFound);
}

TEST_F(WatcherTest, MovingAnAncestorEndsTheWatch) {
    ASSERT_TRUE(fs.createDirectories("/w/d/sub"));
    start({}, "/w/d/sub");
    ASSERT_TRUE(fs.rename("/w/d", "/w/e"));
    auto batch = nextBatch();
    ASSERT_TRUE(batch);
    EXPECT_EQ(batch->changes.front().kind, FileChangeKind::Removed);
    EXPECT_EQ(nextBatch().error().code(), ErrorCode::NotFound);
}

TEST_F(WatcherTest, BadRoots) {
    const auto attempt = [&](std::string root) {
        auto handle = sim.spawn(Lane::UserVisible, watcher.watch(std::move(root), {}));
        auto result = handle.join();
        return result ? ErrorCode::Unknown : result.error().code();
    };
    ASSERT_TRUE(fs.writeFileAtomic("/w/file", "x"));
    EXPECT_EQ(attempt("/missing"), ErrorCode::NotFound);
    EXPECT_EQ(attempt("/w/file"), ErrorCode::InvalidArgument);
    EXPECT_EQ(attempt("relative"), ErrorCode::InvalidArgument);
    WatchOptions bad;
    bad.pollInterval = 1ms;
    auto handle = sim.spawn(Lane::UserVisible, watcher.watch("/w", bad));
    EXPECT_EQ(handle.join().error().code(), ErrorCode::InvalidArgument);
}

TEST_F(WatcherTest, ControlsOverflowFailureAndDegradation) {
    start();
    watcher.injectOverflow("/w/d");
    auto rescan = nextBatch();
    ASSERT_TRUE(rescan);
    EXPECT_EQ(rescan->changes, (std::vector{FileChange{FileChangeKind::Rescan, "/w/d", {}, std::nullopt}}));

    watcher.injectFailure(ErrorCode::IoError);
    EXPECT_EQ(nextBatch().error().code(), ErrorCode::IoError);

    watcher.setDegraded(DegradeReason::WatchLimit, 8192);
    auto degraded = nextBatch();
    ASSERT_TRUE(degraded);
    EXPECT_EQ(degraded->mode, WatchMode::Polling);
    EXPECT_EQ(degraded->changes, (std::vector{FileChange{FileChangeKind::Rescan, "/w", {}, std::nullopt}}));
    const auto status = watch->status();
    EXPECT_EQ(status.reason, DegradeReason::WatchLimit);
    EXPECT_EQ(status.reasonValue, 8192U);
    EXPECT_EQ(status.batches, 2U);
}

TEST_F(WatcherTest, ForcedPollingIsReported) {
    WatchOptions options;
    options.forcePolling = true;
    start(options);
    EXPECT_EQ(watch->status().mode, WatchMode::Polling);
    EXPECT_EQ(watch->status().reason, DegradeReason::Forced);
}

TEST_F(WatcherTest, DestroyingTheWatchCancelsAPendingNext) {
    start();
    auto handle = sim.spawn(Lane::UserVisible, watch->next());
    sim.runFor(10ms);
    EXPECT_FALSE(handle.ready());
    watch.reset();
    sim.runUntilIdle();
    ASSERT_TRUE(handle.ready());
    EXPECT_EQ(handle.join().error().code(), ErrorCode::Cancelled);
    ASSERT_TRUE(fs.writeInPlace("/w/f", "after")); // No observer left: must not crash.
}

TEST_F(WatcherTest, NextAfterDestructionStartsCancelled) {
    start();
    auto task = watch->next(); // Lazy: captures the queue, not the watch.
    watch.reset();
    auto handle = sim.spawn(Lane::UserVisible, std::move(task));
    sim.runUntilIdle();
    EXPECT_EQ(handle.join().error().code(), ErrorCode::Cancelled);
}

TEST_F(WatcherTest, ExclusionsHideEntriesBelow) {
    WatchOptions options;
    options.excluded = {"/w/d"};
    start(options);
    ASSERT_TRUE(fs.writeInPlace("/w/d/hidden", "x"));
    ASSERT_TRUE(fs.writeInPlace("/w/seen", "x"));
    auto batch = nextBatch();
    ASSERT_TRUE(batch);
    EXPECT_EQ(batch->changes, (std::vector{FileChange{FileChangeKind::Created, "/w/seen", {}, FileType::Regular}}));
}

// --- End-to-end property: random mutations in virtual time, batches applied with lag. ---

class Rng {
public:
    explicit Rng(std::uint64_t seed) : m_state(seed) {}
    std::uint64_t next() { // SplitMix64 (public domain, Vigna 2015).
        std::uint64_t z = (m_state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }

private:
    std::uint64_t m_state;
};

void mutate(MemFs& fs, Rng& rng) {
    static constexpr std::array<const char*, 4> names{"a", "b", "c", "d"};
    std::vector<std::string> directories{"/w"};
    std::vector<std::string> files;
    for (const std::string& path : fs.allPaths()) {
        if (path.starts_with("/w/")) {
            (fs.status(path).value().type == FileType::Directory ? directories : files).push_back(path);
        }
    }
    const std::string dir = directories[rng.below(directories.size())];
    const std::string name = dir + "/" + names[rng.below(names.size())];
    switch (rng.below(6)) {
    case 0: static_cast<void>(fs.writeFileAtomic(name, std::to_string(rng.below(3)))); break;
    case 1: static_cast<void>(fs.writeInPlace(name, std::to_string(rng.below(3)))); break;
    case 2: static_cast<void>(fs.createDirectories(name)); break;
    case 3:
        if (!files.empty()) {
            static_cast<void>(fs.removeFile(files[rng.below(files.size())]));
        }
        break;
    case 4:
        if (directories.size() > 1) {
            static_cast<void>(
                fs.removeDirectory(directories[1 + rng.below(directories.size() - 1)])); // Empty ones only.
        }
        break;
    default: {
        std::vector<std::string> all = files;
        all.insert(all.end(), directories.begin() + 1, directories.end());
        if (!all.empty()) {
            static_cast<void>(fs.rename(all[rng.below(all.size())], name));
        }
        break;
    }
    }
}

std::uint64_t runSeed(std::uint64_t seed, bool& converged) {
    frost::ManualClock clock;
    MemFs fs(clock, frost::io::PathStyle::Posix);
    EXPECT_TRUE(fs.createDirectories("/w"));
    Rng rng(seed);
    for (int i = 0; i < 5; ++i) {
        mutate(fs, rng);
    }
    SimExecutor sim(frost::sched::SimOptions{.seed = seed});
    MemFsWatcher watcher(fs, sim);
    frost::test::WatchMirror mirror(fs, "/w");
    std::unique_ptr<IDirectoryWatch> watch;
    {
        auto started = sim.spawn(Lane::UserVisible, watcher.watch("/w", {}));
        auto result = started.join();
        EXPECT_TRUE(result);
        watch = std::move(*result);
    }
    mirror.load();
    auto pending = sim.spawn(Lane::UserVisible, watch->next());
    for (int step = 0; step < 40; ++step) {
        mutate(fs, rng);
        sim.runFor(std::chrono::milliseconds(rng.below(80)));
        if (pending.ready()) {
            auto batch = pending.join();
            EXPECT_TRUE(batch);
            if (rng.below(3) == 0) {
                mutate(fs, rng); // Lag: the tree moves on before the batch is applied.
            }
            if (batch) {
                mirror.apply(*batch);
            }
            pending = sim.spawn(Lane::UserVisible, watch->next());
        }
    }
    // Quiet: everything pending is released.
    sim.runFor(1s);
    while (pending.ready()) {
        auto batch = pending.join();
        EXPECT_TRUE(batch);
        if (batch) {
            mirror.apply(*batch);
        }
        pending = sim.spawn(Lane::UserVisible, watch->next());
        sim.runFor(1s);
    }
    converged = mirror.tree() == mirror.snapshot();
    watch.reset();
    sim.runUntilIdle();
    return sim.traceHash();
}

TEST(MemFsWatcherProperty, ConsumersConvergeAndRunsAreDeterministic) {
    for (std::uint64_t seed = 1; seed <= 500; ++seed) {
        bool converged = false;
        const std::uint64_t first = runSeed(seed, converged);
        ASSERT_TRUE(converged) << "seed " << seed;
        if (seed % 50 == 0) {
            bool again = false;
            EXPECT_EQ(runSeed(seed, again), first) << "seed " << seed;
        }
    }
}

} // namespace
