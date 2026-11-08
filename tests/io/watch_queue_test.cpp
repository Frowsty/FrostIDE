// SPDX-License-Identifier: Apache-2.0
//
/*
 * Tests for WatchQueue (src/io/src/watch_queue.hpp; ADR 0020) on a SimExecutor: the debounce and
 * maxDelay release rule in virtual time, cancellation (before waiting, while waiting, during the
 * debounce sleep) that loses nothing, close() with and without the pending changes and during the
 * debounce sleep, injected failures, scheduler shutdown, backpressure (maxPendingPaths), the single
 * consumer rule, the push filters and option validation.
 */

#include "frost/sched/sim_executor.hpp"

#include "support/crash_dialogs.hpp"
#include "watch_queue.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::ErrorCode;
using frost::io::FileChange;
using frost::io::FileChangeBatch;
using frost::io::FileChangeKind;
using frost::io::FileType;
using frost::io::WatchOptions;
using frost::io::detail::RawEvent;
using frost::io::detail::RawKind;
using frost::io::detail::WatchQueue;
using frost::io::detail::WatchQueueConfig;
using frost::sched::JoinHandle;
using frost::sched::Lane;
using frost::sched::SimExecutor;

RawEvent created(std::string path) { return RawEvent{RawKind::Created, std::move(path), FileType::Regular, false, 0}; }

struct QueueTest : ::testing::Test {
    SimExecutor sim;
    std::shared_ptr<WatchQueue> queue;

    void make(WatchOptions options = {}, bool foldCase = false) {
        queue = WatchQueue::create(
            sim, WatchQueueConfig{"/w", '/', std::move(options), foldCase, frost::io::WatchMode::Native});
    }
    JoinHandle<frost::Result<FileChangeBatch>> next(std::stop_token token = {}) {
        return sim.spawn(Lane::UserVisible, WatchQueue::next(queue, std::move(token)));
    }
    // Paths of a successful batch.
    static std::vector<std::string> paths(const frost::Result<FileChangeBatch>& batch) {
        std::vector<std::string> out;
        if (batch) {
            for (const FileChange& change : batch->changes) {
                out.push_back(change.path);
            }
        }
        return out;
    }
    ~QueueTest() override {
        if (queue) {
            queue->close(frost::Error(ErrorCode::Cancelled, "test end"), false);
        }
        sim.runUntilIdle();
    }
};

TEST_F(QueueTest, DebounceReleasesAfterTheQuietPeriod) {
    make();
    auto handle = next();
    sim.runFor(10ms);
    EXPECT_FALSE(handle.ready());
    static_cast<void>(queue->push(created("/w/a")));
    sim.runFor(49ms);
    EXPECT_FALSE(handle.ready());
    sim.runFor(1ms);
    ASSERT_TRUE(handle.ready());
    auto batch = handle.join();
    ASSERT_TRUE(batch) << batch.error().describe();
    EXPECT_EQ(batch->sequence, 1U);
    EXPECT_EQ(batch->mode, frost::io::WatchMode::Native);
    EXPECT_EQ(paths(batch), (std::vector<std::string>{"/w/a"}));
}

TEST_F(QueueTest, NewEventsRestartTheDebounce) {
    make();
    auto handle = next();
    static_cast<void>(queue->push(created("/w/a")));
    sim.runFor(40ms);
    static_cast<void>(queue->push(created("/w/b")));
    sim.runFor(40ms);
    EXPECT_FALSE(handle.ready());
    sim.runFor(10ms);
    ASSERT_TRUE(handle.ready());
    EXPECT_EQ(paths(handle.join()), (std::vector<std::string>{"/w/a", "/w/b"}));
}

TEST_F(QueueTest, MaxDelayCapsAStorm) {
    make();
    auto handle = next();
    const auto start = sim.steadyNow();
    int pushed = 0;
    while (!handle.ready()) {
        static_cast<void>(queue->push(created("/w/f" + std::to_string(pushed++))));
        sim.runFor(10ms);
    }
    EXPECT_LE(sim.steadyNow() - start, 510ms);
    EXPECT_GE(sim.steadyNow() - start, 500ms);
    EXPECT_TRUE(handle.join());
}

TEST_F(QueueTest, CancelledNextLosesNothing) {
    make();
    std::stop_source stop;
    auto handle = next(stop.get_token());
    static_cast<void>(queue->push(created("/w/a")));
    sim.runFor(20ms);
    stop.request_stop();
    sim.runUntilIdle();
    ASSERT_TRUE(handle.ready());
    auto cancelled = handle.join();
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code(), ErrorCode::Cancelled);
    auto again = next();
    sim.runFor(100ms);
    ASSERT_TRUE(again.ready());
    EXPECT_EQ(paths(again.join()), (std::vector<std::string>{"/w/a"}));
}

TEST_F(QueueTest, CancelWhileWaitingForEvents) {
    make();
    std::stop_source stop;
    auto handle = next(stop.get_token());
    sim.runFor(1s);
    EXPECT_FALSE(handle.ready());
    stop.request_stop();
    sim.runUntilIdle();
    ASSERT_TRUE(handle.ready());
    EXPECT_EQ(handle.join().error().code(), ErrorCode::Cancelled);
}

TEST_F(QueueTest, CloseDeliversPendingFirstThenTheError) {
    make();
    auto handle = next();
    static_cast<void>(queue->push(created("/w/a")));
    queue->close(frost::Error(ErrorCode::NotFound, "root gone"), /*deliverPending=*/true);
    sim.runUntilIdle();
    ASSERT_TRUE(handle.ready());
    EXPECT_EQ(paths(handle.join()), (std::vector<std::string>{"/w/a"}));
    auto after = next();
    sim.runUntilIdle();
    ASSERT_TRUE(after.ready());
    EXPECT_EQ(after.join().error().code(), ErrorCode::NotFound);
    EXPECT_EQ(queue->push(created("/w/b")), 0U); // Closed: ignored.
}

TEST_F(QueueTest, CloseWithoutPendingFailsAtOnce) {
    make();
    static_cast<void>(queue->push(created("/w/a")));
    auto handle = next();
    queue->close(frost::Error(ErrorCode::Cancelled, "destroyed"), /*deliverPending=*/false);
    sim.runUntilIdle();
    ASSERT_TRUE(handle.ready());
    EXPECT_EQ(handle.join().error().code(), ErrorCode::Cancelled);
}

TEST_F(QueueTest, InjectedFailureFiresOnceAndKeepsTheChanges) {
    make();
    static_cast<void>(queue->push(created("/w/a")));
    queue->injectFailure(frost::Error(ErrorCode::IoError, "injected"));
    auto first = next();
    sim.runUntilIdle();
    EXPECT_EQ(first.join().error().code(), ErrorCode::IoError);
    auto second = next();
    sim.runFor(100ms);
    EXPECT_EQ(paths(second.join()), (std::vector<std::string>{"/w/a"}));
}

TEST_F(QueueTest, SchedulerShutdownCancelsAWaitingNext) {
    make();
    auto handle = next();
    sim.runFor(10ms);
    sim.shutdown();
    ASSERT_TRUE(handle.ready());
    EXPECT_EQ(handle.join().error().code(), ErrorCode::Cancelled);
}

TEST_F(QueueTest, AnEntryThatCameAndWentIsReportedRemoved) {
    make();
    auto handle = next();
    static_cast<void>(queue->push(created("/w/a")));
    static_cast<void>(queue->push(RawEvent{RawKind::Removed, "/w/a", FileType::Regular, false, 0}));
    sim.runFor(100ms);
    ASSERT_TRUE(handle.ready());
    auto batch = handle.join();
    ASSERT_TRUE(batch);
    ASSERT_EQ(batch->changes.size(), 1U);
    EXPECT_EQ(batch->changes[0].kind, FileChangeKind::Removed);
    EXPECT_EQ(batch->sequence, 1U);
}

TEST_F(QueueTest, StatusCountsEventsBatchesAndRescans) {
    make();
    static_cast<void>(queue->push(created("/w/a")));
    queue->pushOverflow("/w/d");
    EXPECT_EQ(queue->status().pendingPaths, 2U);
    EXPECT_EQ(queue->status().rawEvents, 2U);
    auto handle = next();
    sim.runFor(100ms);
    ASSERT_TRUE(handle.join());
    const auto status = queue->status();
    EXPECT_EQ(status.batches, 1U);
    EXPECT_EQ(status.rescans, 1U);
    EXPECT_EQ(status.pendingPaths, 0U);
}

TEST_F(QueueTest, FiltersExclusionsDepthAndTempNames) {
    WatchOptions options;
    options.excluded = {"/w/build"};
    make(options);
    EXPECT_EQ(queue->push(created("/w/build")), 1U); // The excluded directory itself is reported.
    EXPECT_EQ(queue->push(created("/w/build/out.o")), 0U);
    EXPECT_EQ(queue->push(created("/w/buildx/a")), 1U);
    EXPECT_EQ(queue->push(created("/w/.f.frost-tmp-1-2")), 0U);
    EXPECT_EQ(queue->push(created("/other/a")), 0U);
}

// WatchOptions::excludedBelow (the project's directory roles, UX-23): the caller's rule filters
// like `excluded`, is asked only about paths strictly below the root, and leaves the folder itself.
TEST_F(QueueTest, TheCallersExclusionRuleFiltersLikeExcluded) {
    std::vector<std::string> asked;
    WatchOptions options;
    options.excludedBelow = [&asked](std::string_view path) {
        asked.emplace_back(path);
        return path.starts_with("/w/out/") || path.find("/.cache/") != std::string_view::npos;
    };
    make(options);
    EXPECT_EQ(queue->push(created("/w/out")), 1U);
    EXPECT_EQ(queue->push(created("/w/out/a.o")), 0U);
    EXPECT_EQ(queue->push(created("/w/src/.cache/x")), 0U);
    EXPECT_EQ(queue->push(created("/w/src/main.cpp")), 1U);
    EXPECT_EQ(queue->push(created("/other/out/a.o")), 0U);
    EXPECT_FALSE(queue->accepts("/w/out/b.o"));
    EXPECT_TRUE(std::ranges::none_of(
        asked, [](const std::string& path) { return path == "/w" || path.starts_with("/other"); }));
}

TEST_F(QueueTest, NonRecursiveWatchesSeeOnlyChildren) {
    WatchOptions options;
    options.recursive = false;
    make(options);
    EXPECT_EQ(queue->push(created("/w/a")), 1U);
    EXPECT_EQ(queue->push(created("/w/a/b")), 0U);
    queue->pushOverflow("/w/a/b"); // Becomes Rescan(root).
    auto handle = next();
    sim.runFor(100ms);
    auto batch = handle.join();
    ASSERT_TRUE(batch);
    ASSERT_EQ(batch->changes.size(), 1U);
    EXPECT_EQ(batch->changes[0].kind, FileChangeKind::Rescan);
    EXPECT_EQ(batch->changes[0].path, "/w");
}

TEST_F(QueueTest, FrostTempFilesCanBeReported) {
    WatchOptions options;
    options.reportFrostTempFiles = true;
    make(options);
    EXPECT_EQ(queue->push(created("/w/.f.frost-tmp-1-2")), 1U);
}

TEST_F(QueueTest, AStopRequestedBeforeTheCallIsCancelledAndLosesNothing) {
    make();
    static_cast<void>(queue->push(created("/w/a")));
    std::stop_source stop;
    stop.request_stop(); // Before next() ever runs, so before any callback is registered.
    auto handle = next(stop.get_token());
    sim.runUntilIdle();
    ASSERT_TRUE(handle.ready());
    EXPECT_EQ(handle.join().error().code(), ErrorCode::Cancelled);
    auto again = next(); // A fresh token: the per-call stop source starts clean.
    sim.runFor(100ms);
    ASSERT_TRUE(again.ready());
    EXPECT_EQ(paths(again.join()), (std::vector<std::string>{"/w/a"}));
}

TEST_F(QueueTest, AStopDuringTheDebounceSleepIsCancelledAndLosesNothing) {
    make();
    std::stop_source stop;
    auto handle = next(stop.get_token());
    static_cast<void>(queue->push(created("/w/a")));
    sim.runFor(10ms); // Now sleeping until the debounce deadline.
    EXPECT_FALSE(handle.ready());
    stop.request_stop();
    sim.runUntilIdle(); // No virtual time passes: the sleep ends early.
    ASSERT_TRUE(handle.ready());
    EXPECT_EQ(handle.join().error().code(), ErrorCode::Cancelled);
    auto again = next();
    sim.runFor(100ms);
    EXPECT_EQ(paths(again.join()), (std::vector<std::string>{"/w/a"}));
}

TEST_F(QueueTest, CloseDuringTheDebounceSleepResumesAtOnce) {
    make();
    auto handle = next();
    static_cast<void>(queue->push(created("/w/a")));
    sim.runFor(10ms);
    EXPECT_FALSE(handle.ready());
    const auto before = sim.steadyNow();
    queue->close(frost::Error(ErrorCode::NotFound, "root gone"), /*deliverPending=*/true);
    sim.runUntilIdle();
    ASSERT_TRUE(handle.ready());
    EXPECT_EQ(sim.steadyNow(), before); // Released without waiting for the debounce.
    EXPECT_EQ(paths(handle.join()), (std::vector<std::string>{"/w/a"}));
}

TEST_F(QueueTest, ANextCreatedBeforeTheWatchWentAwayIsCancelled) {
    make();
    static_cast<void>(queue->push(created("/w/a")));
    // The task owns its own reference: the queue outlives the test's pointer, as a watch's queue
    // outlives the destroyed watch (whose destructor closes it with Cancelled).
    auto task = WatchQueue::next(queue, {});
    queue->close(frost::Error(ErrorCode::Cancelled, "the watch was destroyed"), /*deliverPending=*/false);
    queue.reset();
    auto handle = sim.spawn(Lane::UserVisible, std::move(task));
    sim.runUntilIdle();
    ASSERT_TRUE(handle.ready());
    EXPECT_EQ(handle.join().error().code(), ErrorCode::Cancelled);
}

TEST_F(QueueTest, WithoutAConsumerThePendingSetCollapsesIntoRescanOfTheRoot) {
    WatchOptions options;
    options.maxPendingPaths = 8;
    make(options);
    for (int i = 0; i < 20; ++i) {
        static_cast<void>(queue->push(created("/w/f" + std::to_string(i))));
    }
    EXPECT_LE(queue->status().pendingPaths, 8U);
    auto handle = next();
    sim.runFor(100ms);
    auto batch = handle.join();
    ASSERT_TRUE(batch);
    ASSERT_EQ(batch->changes.size(), 1U);
    EXPECT_EQ(batch->changes[0].kind, FileChangeKind::Rescan);
    EXPECT_EQ(batch->changes[0].path, "/w");
}

TEST(WatchQueueDeathTest, ASecondConcurrentNextIsAContractViolation) {
    EXPECT_DEATH(
        {
            frost::test::disableCrashDialogs();
            SimExecutor sim;
            auto queue = WatchQueue::create(sim, WatchQueueConfig{"/w", '/', {}, false, frost::io::WatchMode::Native});
            auto first = sim.spawn(Lane::UserVisible, WatchQueue::next(queue, {}));
            sim.runUntilIdle(); // `first` now waits for events.
            auto second = sim.spawn(Lane::UserVisible, WatchQueue::next(queue, {}));
            sim.runUntilIdle();
        },
        "consumer");
}

TEST(WatchQueueHelpers, ValidateWatchOptions) {
    using frost::io::PathStyle;
    using frost::io::detail::validateWatchOptions;
    WatchOptions options;
    EXPECT_TRUE(validateWatchOptions("/w", options, PathStyle::Posix));
    options.debounce = -1ms;
    EXPECT_EQ(validateWatchOptions("/w", options, PathStyle::Posix).error().code(), ErrorCode::InvalidArgument);
    options = {};
    options.maxDelay = 10ms;
    EXPECT_FALSE(validateWatchOptions("/w", options, PathStyle::Posix));
    options = {};
    options.maxPendingPaths = 0;
    EXPECT_FALSE(validateWatchOptions("/w", options, PathStyle::Posix));
    options = {};
    options.pollInterval = 99ms;
    EXPECT_FALSE(validateWatchOptions("/w", options, PathStyle::Posix));
    options = {};
    options.excluded = {"relative"};
    EXPECT_FALSE(validateWatchOptions("/w", options, PathStyle::Posix));
    options.excluded = {"/elsewhere/x"};
    EXPECT_FALSE(validateWatchOptions("/w", options, PathStyle::Posix));
    options.excluded = {"/w"};
    EXPECT_FALSE(validateWatchOptions("/w", options, PathStyle::Posix)); // Must be strictly below.
    options.excluded = {"/w//./b/"}; // POSIX normalisation keeps "..", so only "." and repeats here.
    auto normalized = validateWatchOptions("/w", options, PathStyle::Posix);
    ASSERT_TRUE(normalized);
    EXPECT_EQ(normalized->excluded, (std::vector<std::string>{"/w/b"}));
    options.excluded = {"c:\\W\\Build"};
    auto folded = validateWatchOptions("C:\\w", options, PathStyle::Windows);
    ASSERT_TRUE(folded) << folded.error().describe();
}

TEST(WatchQueueHelpers, StartsWithFoldCase) {
    using frost::io::detail::startsWithFoldCase;
    EXPECT_TRUE(startsWithFoldCase("C:\\Src\\File", "c:\\src"));
    EXPECT_FALSE(startsWithFoldCase("C:\\Sr", "c:\\src"));
    EXPECT_TRUE(startsWithFoldCase("C:\\\xC3\x84rger\\x", "C:\\\xC3\x84rger")); // Non-ASCII, same case.
#ifdef _WIN32
    EXPECT_TRUE(startsWithFoldCase("C:\\\xC3\xA4rger\\x", "C:\\\xC3\x84rger")); // \u00e4 vs \u00c4.
#endif
}

TEST_F(QueueTest, ExclusionsFoldCaseWhenConfigured) {
    WatchOptions options;
    options.excluded = {"/w/Build"};
    make(options, /*foldCase=*/true);
    EXPECT_EQ(queue->push(created("/w/BUILD/out.o")), 0U);
}

} // namespace
