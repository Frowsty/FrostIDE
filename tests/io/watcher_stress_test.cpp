// SPDX-License-Identifier: Apache-2.0
//
/*
 * Stress tests for the file watchers (ADR 0020; label "stress", run in Release and the sanitizer trees):
 * many files created, renamed and deleted; rename storms; overflow rounds; watches created and destroyed
 * while writers run; and MemFsWatcher under a ThreadPool with eight mutating threads. Each scenario ends
 * in the mirror comparison of watch_env.hpp and prints its counts (raw events per second, rescans, peak
 * pending paths). FROST_STRESS_FULL=1 selects the full scale (200k files instead of 50k).
 */

#include "frost/core/clock.hpp"
#include "frost/io/file_watcher.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/mem_fs_watcher.hpp"
#include "frost/io/reactor.hpp"
#include "frost/sched/thread_pool.hpp"

#include "support/scratch_dir.hpp"
#include "support/test_env.hpp"
#include "support/watch_mirror.hpp"
#include "watch_env.hpp"
#include "watcher_testing.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::io::IDirectoryWatch;
using frost::io::WatchMode;
using frost::io::WatchOptions;
using frost::test::MirrorOptions;
using frost::test::nativeJoin;
using frost::test::WatchEnv;
using frost::test::WatchMirror;

constexpr char kSep = frost::test::kNativeSeparator;

// Samples status().pendingPaths while a phase runs.
class PeakSampler {
public:
    explicit PeakSampler(IDirectoryWatch& watch)
        : m_thread([this, &watch] {
              while (!m_stop.load(std::memory_order_relaxed)) {
                  const std::size_t now = watch.status().pendingPaths;
                  std::size_t peak = m_peak.load(std::memory_order_relaxed);
                  while (now > peak && !m_peak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
                  }
                  std::this_thread::sleep_for(1ms);
              }
          }) {}
    ~PeakSampler() {
        m_stop.store(true, std::memory_order_relaxed);
        m_thread.join();
    }
    PeakSampler(const PeakSampler&) = delete ("owns a thread");
    PeakSampler& operator=(const PeakSampler&) = delete ("owns a thread");
    [[nodiscard]] std::size_t peak() const noexcept { return m_peak.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> m_stop{false};
    std::atomic<std::size_t> m_peak{0};
    std::thread m_thread;
};

struct Session {
    WatchEnv env;
    frost::test::ScratchDir scratch{"watch-stress"};
    MirrorOptions mirrorOptions{.separator = kSep};
    std::unique_ptr<IDirectoryWatch> watch;
    std::unique_ptr<WatchMirror> mirror;

    explicit Session(WatchOptions options = {}) {
        auto started = env.watch(scratch.path(), std::move(options));
        if (!started) {
            ADD_FAILURE() << started.error().describe();
            return;
        }
        watch = std::move(*started);
        mirror = std::make_unique<WatchMirror>(frost::io::realFileSystem(), watch->root(), mirrorOptions);
        mirror->load();
    }
    ~Session() { watch.reset(); }
    Session(const Session&) = delete ("owns the watch");
    Session& operator=(const Session&) = delete ("owns the watch");

    [[nodiscard]] std::string at(std::string_view name) const { return nativeJoin(watch->root(), name); }
    // Applies everything up to quiescence; overflow is allowed under stress.
    void settle() { env.settle(*watch, *mirror, watch->root(), mirrorOptions, /*rescansExpected=*/true); }
    void report(const char* phase, std::chrono::steady_clock::duration took, std::size_t peak) {
        const auto status = watch->status();
        const double seconds = std::chrono::duration<double>(took).count();
        std::printf("[stress] %s: %.2f s, %llu raw events (%.0f/s), %llu rescans, peak pending %zu, mode %s\n", phase,
                    seconds, static_cast<unsigned long long>(status.rawEvents),
                    seconds > 0 ? static_cast<double>(status.rawEvents) / seconds : 0.0,
                    static_cast<unsigned long long>(status.rescans), peak,
                    status.mode == WatchMode::Native ? "native" : "polling");
    }
};

TEST(WatcherStress, ManyFilesCreatedRenamedAndDeleted) {
    const std::size_t files = frost::test::stressFull() ? 200'000 : 50'000;
    const std::size_t directories = files / 100;
    Session s;
    ASSERT_TRUE(s.watch);
    {
        PeakSampler sampler(*s.watch);
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t d = 0; d < directories; ++d) {
            const std::string dir = s.at(std::format("d{:04}", d));
            frost::test::makeDirectory(dir);
            for (std::size_t f = 0; f < files / directories; ++f) {
                frost::test::writePlain(nativeJoin(dir, std::format("f{:03}", f)), "x");
            }
        }
        s.settle();
        s.report("create", std::chrono::steady_clock::now() - start, sampler.peak());
    }
    {
        PeakSampler sampler(*s.watch);
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t d = 0; d < directories; ++d) {
            const std::string dir = s.at(std::format("d{:04}", d));
            for (std::size_t f = 0; f < files / directories; ++f) {
                frost::test::renamePath(nativeJoin(dir, std::format("f{:03}", f)),
                                        nativeJoin(dir, std::format("g{:03}", f)));
            }
        }
        s.settle();
        s.report("rename", std::chrono::steady_clock::now() - start, sampler.peak());
    }
    {
        PeakSampler sampler(*s.watch);
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t d = 0; d < directories; ++d) {
            frost::test::removeAll(s.at(std::format("d{:04}", d)));
        }
        s.settle();
        s.report("delete", std::chrono::steady_clock::now() - start, sampler.peak());
    }
}

#ifdef _WIN32
// ADR 0020 records how often a burst overflows the RDCW buffer at 64 KiB and at 256 KiB.
TEST(WatcherStress, ReadBufferOverflowRates) {
    for (const std::size_t bytes : {std::size_t{64} * 1024, std::size_t{256} * 1024}) {
        frost::io::testing::setWindowsReadBufferBytesForTesting(bytes);
        Session s;
        ASSERT_TRUE(s.watch);
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 10'000; ++i) {
            frost::test::writePlain(s.at(std::format("burst{:05}", i)), "b");
        }
        s.settle();
        std::printf("[stress] %zu KiB buffer: 10000 creates, %llu rescans in %.2f s\n", bytes / 1024,
                    static_cast<unsigned long long>(s.watch->status().rescans),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    }
    frost::io::testing::setWindowsReadBufferBytesForTesting(0);
}
#endif

TEST(WatcherStress, RenameStorms) {
    Session s;
    ASSERT_TRUE(s.watch);
    frost::test::writePlain(s.at("a"), "ping");
    for (int i = 0; i < 10'000; ++i) {
        if (i % 2 == 0) {
            frost::test::renamePath(s.at("a"), s.at("b"));
        } else {
            frost::test::renamePath(s.at("b"), s.at("a"));
        }
    }
    s.settle();
    for (int chain = 0; chain < 1000; ++chain) {
        const std::string first = s.at(std::format("c{}-0", chain));
        frost::test::writePlain(first, "chain");
        std::string current = first;
        for (int link = 1; link <= 3; ++link) {
            const std::string next = s.at(std::format("c{}-{}", chain, link));
            frost::test::renamePath(current, next);
            current = next;
        }
    }
    s.settle();
    // Directory renames while files are written elsewhere.
    frost::test::makeDirectory(s.at("left"));
    frost::test::writePlain(nativeJoin(s.at("left"), "inside"), "i");
    frost::test::makeDirectory(s.at("busy"));
    for (int i = 0; i < 200; ++i) {
        frost::test::writePlain(nativeJoin(s.at("busy"), std::format("w{}", i)), "w");
        if (i % 2 == 0) {
            frost::test::renamePath(s.at("left"), s.at("right"));
        } else {
            frost::test::renamePath(s.at("right"), s.at("left"));
        }
    }
    s.settle();
}

TEST(WatcherStress, OverflowRounds) {
#ifdef _WIN32
    frost::io::testing::setWindowsReadBufferBytesForTesting(4096);
    const std::size_t burst = 2000;
#else
    std::size_t burst = 8292; // Half of the default max_queued_events, plus room: each create is 3 events.
    if (std::FILE* file = std::fopen("/proc/sys/fs/inotify/max_queued_events", "r"); file != nullptr) {
        unsigned long long value = 0;
        if (std::fscanf(file, "%llu", &value) == 1 && value < 200'000) {
            burst = static_cast<std::size_t>(value / 2 + 100);
        }
        std::fclose(file);
    }
#endif
    Session s;
    ASSERT_TRUE(s.watch);
    for (int round = 0; round < 20; ++round) {
        const std::string dir = s.at(std::format("round{}", round));
        frost::test::makeDirectory(dir);
        {
            frost::io::testing::ReactorStall stall(s.env.reactor());
            for (std::size_t i = 0; i < burst; ++i) {
                frost::test::writePlain(nativeJoin(dir, std::format("o{}", i)), "o");
            }
        }
        s.settle();
        frost::test::removeAll(dir);
    }
    s.settle();
    std::printf("[stress] 20 overflow rounds of %zu files: %llu rescans\n", burst,
                static_cast<unsigned long long>(s.watch->status().rescans));
#ifdef _WIN32
    frost::io::testing::setWindowsReadBufferBytesForTesting(0);
#endif
}

TEST(WatcherStress, WatchesComeAndGoWhileWritersRun) {
    frost::test::ScratchDir scratch("watch-churn");
    for (int d = 0; d < 8; ++d) {
        frost::test::makeDirectory(nativeJoin(scratch.path(), std::format("w{}", d)));
    }
    auto reactor = frost::io::Reactor::create(frost::io::ReactorOptions{2, "frost-test-churn"});
    ASSERT_TRUE(reactor);
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 2});
    auto watcher = frost::io::RealFileWatcher::create(**reactor, pool);
    ASSERT_TRUE(watcher);
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    for (int t = 0; t < 8; ++t) {
        writers.emplace_back([&, t] {
            const std::string dir = nativeJoin(scratch.path(), std::format("w{}", t));
            for (unsigned i = 0; !stop.load(std::memory_order_relaxed); ++i) {
                const std::string file = nativeJoin(dir, std::format("f{}", i % 50));
                static_cast<void>(frost::io::realFileSystem().writeFileAtomic(file, std::to_string(i)));
            }
        });
    }
    std::mt19937 random(42);
    for (int i = 0; i < 200; ++i) {
        const std::string root =
            random() % 3 == 0 ? scratch.path() : nativeJoin(scratch.path(), std::format("w{}", random() % 8));
        auto watch = frost::sched::syncWait((*watcher)->watch(root, {}));
        ASSERT_TRUE(watch) << watch.error().describe();
#ifndef _WIN32
        EXPECT_EQ((*watch)->status().mode, WatchMode::Native); // One shared inotify instance.
#endif
        if (i % 4 == 0) {
            std::stop_source cancel;
            auto handle = pool.spawn(frost::sched::Lane::UserVisible, (*watch)->next(cancel.get_token()));
            std::this_thread::sleep_for(std::chrono::milliseconds(random() % 30));
            static_cast<void>(cancel.request_stop());
            static_cast<void>(handle.join());
        }
        watch->reset();
    }
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& writer : writers) {
        writer.join();
    }
    (*reactor)->shutdown();
    watcher->reset();
    EXPECT_EQ((*reactor)->stats().boundObjects, 0);
    pool.shutdown();
}

TEST(WatcherStress, MemFsWatcherUnderAThreadPool) {
    frost::ManualClock clock;
    frost::io::MemFs fs(clock, frost::io::PathStyle::Posix);
    ASSERT_TRUE(fs.createDirectories("/w"));
    for (int t = 0; t < 8; ++t) {
        ASSERT_TRUE(fs.createDirectories(std::format("/w/t{}", t)));
    }
    frost::sched::ThreadPool pool(frost::sched::ThreadPoolOptions{.workerCount = 4});
    {
        frost::io::MemFsWatcher watcher(fs, pool);
        auto watch = frost::sched::syncWait(watcher.watch("/w", {}));
        ASSERT_TRUE(watch);
        WatchMirror mirror(fs, "/w");
        mirror.load();
        std::atomic<bool> stop{false};
        std::atomic<bool> sawFence{false};
        std::thread consumer([&] { // The only thread that touches `mirror` until it is joined.
            while (!stop.load(std::memory_order_acquire)) {
                std::stop_source cancel;
                auto handle = pool.spawn(frost::sched::Lane::UserVisible, (*watch)->next(cancel.get_token()));
                const auto deadline = std::chrono::steady_clock::now() + 200ms;
                while (!handle.ready() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(1ms);
                }
                static_cast<void>(cancel.request_stop());
                auto batch = handle.join();
                if (batch) {
                    mirror.apply(*batch);
                    if (mirror.tree().contains("/w/fence")) {
                        sawFence.store(true, std::memory_order_release);
                    }
                }
            }
        });
        std::vector<std::thread> mutators;
        for (int t = 0; t < 8; ++t) {
            mutators.emplace_back([&fs, t] {
                std::mt19937 random(static_cast<unsigned>(t));
                const std::string dir = std::format("/w/t{}", t);
                for (int i = 0; i < 2000; ++i) {
                    const std::string a = std::format("{}/{}", dir, random() % 20);
                    const std::string b = std::format("{}/{}", dir, random() % 20);
                    switch (random() % 5) {
                    case 0: static_cast<void>(fs.writeFileAtomic(a, std::to_string(i))); break;
                    case 1: static_cast<void>(fs.writeInPlace(a, std::to_string(i))); break;
                    case 2: static_cast<void>(fs.removeFile(a)); break;
                    case 3: static_cast<void>(fs.rename(a, b)); break;
                    default: static_cast<void>(fs.createDirectories(a + "d")); break;
                    }
                }
            });
        }
        for (std::thread& mutator : mutators) {
            mutator.join();
        }
        // Fence: everything before it has been delivered once a batch names it.
        ASSERT_TRUE(fs.writeInPlace("/w/fence", "f"));
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (std::chrono::steady_clock::now() < deadline && !sawFence.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(5ms);
        }
        stop.store(true, std::memory_order_release);
        consumer.join();
        EXPECT_TRUE(sawFence.load());
        EXPECT_EQ(mirror.tree(), mirror.snapshot());
        watch->reset();
    }
    pool.shutdown();
}

} // namespace
