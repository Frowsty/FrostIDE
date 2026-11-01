// SPDX-License-Identifier: Apache-2.0
//
/*
 * Tests for Reactor (frost/io/reactor.hpp; ADR 0020): option checks, thread start and the idempotent
 * drain shutdown, onReactorThread, the lifecycle of bound states (binding after shutdown, the
 * boundObjects count, states destroyed concurrently with shutdown, the destructor's check), SIGPIPE on
 * POSIX reactor threads, the ReactorStall test hook, and RealFileWatcher::create's refusals (a
 * caller-driven scheduler, a shut-down reactor). Watch behaviour is covered by the integration tests.
 */

#include "frost/io/file_watcher.hpp"
#include "frost/io/reactor.hpp"
#include "frost/sched/sim_executor.hpp"
#include "frost/sched/thread_pool.hpp"

#include "support/crash_dialogs.hpp"
#include "watcher_testing.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>

#ifndef _WIN32
#include <csignal>

#include <pthread.h>
#endif

namespace {

using frost::ErrorCode;
using frost::io::Reactor;
using frost::io::ReactorOptions;
using frost::io::testing::BoundProbe;

TEST(Reactor, RefusesThreadCountsOtherThanOneOrTwo) {
    for (const unsigned threads : {0U, 3U, 16U}) {
        auto reactor = Reactor::create(ReactorOptions{threads, "frost-test"});
        ASSERT_FALSE(reactor);
        EXPECT_EQ(reactor.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(Reactor, StartsAndShutsDownIdempotently) {
    for (const unsigned threads : {1U, 2U}) {
        auto reactor = Reactor::create(ReactorOptions{threads, "frost-test"});
        ASSERT_TRUE(reactor) << reactor.error().describe();
        EXPECT_FALSE((*reactor)->onReactorThread());
        EXPECT_EQ((*reactor)->stats().boundObjects, 0);
        EXPECT_FALSE((*reactor)->stats().shutDown);
        (*reactor)->shutdown();
        EXPECT_TRUE((*reactor)->stats().shutDown);
        (*reactor)->shutdown(); // Idempotent.
    }
}

TEST(Reactor, StallBlocksEveryThreadUntilReleased) {
    auto reactor = Reactor::create(ReactorOptions{2, "frost-test"});
    ASSERT_TRUE(reactor);
    {
        frost::io::testing::ReactorStall stall(**reactor); // Returns once both threads are blocked.
        stall.release();
        stall.release(); // Idempotent.
    }
    frost::io::testing::ReactorStall second(**reactor); // Released by its destructor.
}

TEST(Reactor, DestructionWithoutShutdownDrains) {
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor);
    reactor->reset(); // ~Reactor shuts down first.
}

TEST(Reactor, OnReactorThreadIsTrueOnlyOnItsOwnThreads) {
    auto reactor = Reactor::create(ReactorOptions{2, "frost-test"});
    auto other = Reactor::create();
    ASSERT_TRUE(reactor && other);
    bool onOwn = false;
    bool onOther = true;
    frost::io::testing::runOnReactorThread(**reactor, [&] {
        onOwn = (*reactor)->onReactorThread();
        onOther = (*other)->onReactorThread();
    });
    EXPECT_TRUE(onOwn);
    EXPECT_FALSE(onOther);
    EXPECT_FALSE((*reactor)->onReactorThread());
}

TEST(Reactor, BoundStatesAreCountedUntilTheirLastReferenceDrops) {
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor);
    auto first = BoundProbe::bind(**reactor);
    auto second = BoundProbe::bind(**reactor);
    ASSERT_TRUE(first && second);
    EXPECT_EQ((*reactor)->stats().boundObjects, 2);
    std::shared_ptr<BoundProbe> extra = *first;
    first->reset();
    EXPECT_EQ((*reactor)->stats().boundObjects, 2); // `extra` keeps it alive.
    extra.reset();
    second->reset();
    EXPECT_EQ((*reactor)->stats().boundObjects, 0);
}

TEST(Reactor, ShutdownRunsEveryLiveStatesShutdownAndRefusesLaterBindsAndPosts) {
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor);
    auto probe = BoundProbe::bind(**reactor);
    ASSERT_TRUE(probe);
    std::atomic<bool> posted{false};
    // A named reference: frost-lint reads `(*probe)->post(` as a trailing return type plus a contract.
    BoundProbe& state = **probe;
    ASSERT_TRUE(state.post([&] { posted.store(true, std::memory_order_relaxed); }));
    (*reactor)->shutdown();
    EXPECT_TRUE(posted.load(std::memory_order_relaxed)); // Posts made before shutdown still run (the drain).
    EXPECT_TRUE(state.shutdownRan());
    EXPECT_FALSE(state.post([] {}));
    auto late = BoundProbe::bind(**reactor);
    ASSERT_FALSE(late);
    EXPECT_EQ(late.error().code(), ErrorCode::Cancelled);
    probe->reset(); // Destroyed after shutdown: inline, no reactor thread involved.
    EXPECT_EQ((*reactor)->stats().boundObjects, 0);
}

// A state dropped on another thread while the owner shuts the reactor down: every post that was
// accepted runs, and the count returns to zero (also run under TSan in WSL).
TEST(Reactor, StatesDestroyedConcurrentlyWithShutdownLeaveNothingBehind) {
    for (int iteration = 0; iteration < 1000; ++iteration) {
        auto reactor = Reactor::create(ReactorOptions{iteration % 2 == 0 ? 1U : 2U, "frost-test"});
        ASSERT_TRUE(reactor);
        auto probe = BoundProbe::bind(**reactor);
        ASSERT_TRUE(probe);
        std::atomic<int> accepted{0};
        std::atomic<int> ran{0};
        std::thread dropper([&, state = std::move(*probe)]() mutable {
            for (int i = 0; i < 4; ++i) {
                if (state->post([&] { ran.fetch_add(1, std::memory_order_relaxed); })) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                }
            }
            state.reset();
        });
        (*reactor)->shutdown();
        dropper.join();
        ASSERT_EQ(ran.load(), accepted.load()) << "iteration " << iteration;
        ASSERT_EQ((*reactor)->stats().boundObjects, 0) << "iteration " << iteration;
    }
}

TEST(ReactorDeathTest, DestroyingTheReactorWithABoundStateIsAContractViolation) {
    EXPECT_DEATH(
        {
            frost::test::disableCrashDialogs();
            auto reactor = Reactor::create();
            auto probe = BoundProbe::bind(**reactor);
            reactor->reset(); // `probe` is still alive.
        },
        "boundObjects");
}

#ifndef _WIN32
TEST(Reactor, ReactorThreadsBlockSigpipe) {
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor);
    int blocked = -1;
    frost::io::testing::runOnReactorThread(**reactor, [&] {
        sigset_t current;
        sigemptyset(&current);
        pthread_sigmask(SIG_BLOCK, nullptr, &current);
        blocked = sigismember(&current, SIGPIPE);
    });
    EXPECT_EQ(blocked, 1);
}
#endif

TEST(RealFileWatcherCreate, RefusesCallerDrivenSchedulers) {
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor);
    frost::sched::SimExecutor sim;
    auto watcher = frost::io::RealFileWatcher::create(**reactor, sim);
    ASSERT_FALSE(watcher);
    EXPECT_EQ(watcher.error().code(), ErrorCode::InvalidArgument);
}

TEST(RealFileWatcherCreate, RefusesAShutDownReactor) {
    auto reactor = Reactor::create();
    ASSERT_TRUE(reactor);
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    (*reactor)->shutdown();
    auto watcher = frost::io::RealFileWatcher::create(**reactor, pool);
    ASSERT_FALSE(watcher);
    EXPECT_EQ(watcher.error().code(), ErrorCode::Cancelled);
    pool.shutdown();
}

} // namespace
