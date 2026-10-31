// SPDX-License-Identifier: Apache-2.0
//
// Tests for ChaseLevDeque: owner LIFO, thieves FIFO, growth, empty behaviour,
// the last-element race, and a concurrent owner+thieves run in which every
// element must be taken exactly once.

#include "frost/sched/chase_lev_deque.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

using frost::sched::ChaseLevDeque;
using Status = ChaseLevDeque<int>::StealStatus;

// Backing storage so the deque can hold stable int pointers.
std::vector<int> iota(int count) {
    std::vector<int> values(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        values[static_cast<std::size_t>(i)] = i;
    }
    return values;
}

TEST(ChaseLevDeque, OwnerPopsLifo) {
    std::vector<int> values = iota(5);
    ChaseLevDeque<int> deque;
    for (int& value : values) {
        deque.push(&value);
    }
    for (int expected = 4; expected >= 0; --expected) {
        int* item = deque.pop();
        ASSERT_NE(item, nullptr);
        EXPECT_EQ(*item, expected);
    }
    EXPECT_EQ(deque.pop(), nullptr);
}

TEST(ChaseLevDeque, ThievesStealFifo) {
    std::vector<int> values = iota(5);
    ChaseLevDeque<int> deque;
    for (int& value : values) {
        deque.push(&value);
    }
    for (int expected = 0; expected < 5; ++expected) {
        const auto result = deque.steal();
        ASSERT_EQ(result.status, Status::Success);
        EXPECT_EQ(*result.item, expected);
    }
    EXPECT_EQ(deque.steal().status, Status::Empty);
}

TEST(ChaseLevDeque, MixedEndsMeetInTheMiddle) {
    std::vector<int> values = iota(6);
    ChaseLevDeque<int> deque;
    for (int& value : values) {
        deque.push(&value);
    }
    EXPECT_EQ(*deque.steal().item, 0);
    EXPECT_EQ(*deque.pop(), 5);
    EXPECT_EQ(*deque.steal().item, 1);
    EXPECT_EQ(*deque.pop(), 4);
    EXPECT_EQ(deque.sizeApprox(), 2U);
    EXPECT_EQ(*deque.pop(), 3);
    EXPECT_EQ(*deque.steal().item, 2);
    EXPECT_EQ(deque.pop(), nullptr);
    EXPECT_EQ(deque.steal().status, Status::Empty);
}

TEST(ChaseLevDeque, EmptyPopKeepsDequeUsable) {
    ChaseLevDeque<int> deque;
    EXPECT_EQ(deque.pop(), nullptr);
    EXPECT_EQ(deque.pop(), nullptr);
    EXPECT_EQ(deque.steal().status, Status::Empty);
    int value = 7;
    deque.push(&value);
    EXPECT_EQ(deque.sizeApprox(), 1U);
    EXPECT_EQ(deque.pop(), &value);
    EXPECT_EQ(deque.sizeApprox(), 0U);
}

TEST(ChaseLevDeque, GrowsAndKeepsOrder) {
    std::vector<int> values = iota(10'000);
    ChaseLevDeque<int> deque(4);
    EXPECT_EQ(deque.capacity(), 4U);
    // Interleave steals so growth happens with top != 0 and the ring wraps.
    std::size_t stolen = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        deque.push(&values[i]);
        if (i % 3 == 0) {
            const auto result = deque.steal();
            ASSERT_EQ(result.status, Status::Success);
            EXPECT_EQ(*result.item, static_cast<int>(stolen));
            ++stolen;
        }
    }
    EXPECT_GE(deque.capacity(), values.size() - stolen);
    // The rest comes out newest first.
    for (int expected = 9'999; expected >= static_cast<int>(stolen); --expected) {
        int* item = deque.pop();
        ASSERT_NE(item, nullptr);
        ASSERT_EQ(*item, expected);
    }
    EXPECT_EQ(deque.pop(), nullptr);
}

TEST(ChaseLevDeque, InitialCapacityRoundsUpToPowerOfTwo) {
    EXPECT_EQ(ChaseLevDeque<int>(0).capacity(), 2U);
    EXPECT_EQ(ChaseLevDeque<int>(5).capacity(), 8U);
    EXPECT_EQ(ChaseLevDeque<int>(64).capacity(), 64U);
}

// The owner and one thief race for a single element many times; exactly one may win each round.
TEST(ChaseLevDeque, LastElementGoesToExactlyOneTaker) {
    ChaseLevDeque<int> deque;
    int value = 1;
    constexpr int kRounds = 20'000;
    std::atomic<int> round{-1};
    std::atomic<int> thiefWins{0};
    std::atomic<int> thiefDone{-1};
    std::thread thief([&] {
        for (int r = 0; r < kRounds; ++r) {
            while (round.load(std::memory_order_acquire) < r) {
                std::this_thread::yield();
            }
            if (deque.steal().status == Status::Success) {
                thiefWins.fetch_add(1, std::memory_order_relaxed);
            }
            thiefDone.store(r, std::memory_order_release);
        }
    });
    int ownerWins = 0;
    for (int r = 0; r < kRounds; ++r) {
        deque.push(&value);
        round.store(r, std::memory_order_release);
        if (deque.pop() != nullptr) {
            ++ownerWins;
        }
        while (thiefDone.load(std::memory_order_acquire) < r) {
            std::this_thread::yield();
        }
        // Whatever neither side took is still queued; drain it for the next round.
        while (deque.pop() != nullptr) {
            ++ownerWins;
        }
    }
    thief.join();
    EXPECT_EQ(ownerWins + thiefWins.load(), kRounds);
}

// The owner writes each element's payload (plain, non-atomic memory) just
// before pushing it; a thief that steals it must see the whole payload. With
// no other synchronisation between the threads, this checks push()/steal()
// publication on its own (under TSan, the reason for FROST_SCHED_TSAN).
TEST(ChaseLevDeque, PushPublishesElementContentsToThieves) {
    struct Payload {
        int id = 0;
        int square = 0;
        int check = 0;
    };
    constexpr int kItems = 20'000;
    std::vector<Payload> payloads(kItems);
    ChaseLevDeque<Payload> deque(4);
    std::atomic<bool> ownerDone{false};
    std::atomic<int> bad{0};
    std::atomic<int> stolen{0};
    std::thread thief([&] {
        for (;;) {
            const auto result = deque.steal();
            if (result.status == ChaseLevDeque<Payload>::StealStatus::Success) {
                const Payload& payload = *result.item;
                bad.fetch_add(payload.square == payload.id * payload.id && payload.check == ~payload.id ? 0 : 1,
                              std::memory_order_relaxed);
                stolen.fetch_add(1, std::memory_order_relaxed);
            } else if (result.status == ChaseLevDeque<Payload>::StealStatus::Empty &&
                       ownerDone.load(std::memory_order_acquire)) {
                return;
            }
        }
    });
    int popped = 0;
    for (int i = 0; i < kItems; ++i) {
        Payload& payload = payloads[static_cast<std::size_t>(i)];
        payload.id = i;
        payload.square = i * i;
        payload.check = ~i;
        deque.push(&payload);
        if (i % 8 == 0 && deque.pop() != nullptr) {
            ++popped;
        }
    }
    while (deque.pop() != nullptr) {
        ++popped;
    }
    ownerDone.store(true, std::memory_order_release);
    thief.join();
    EXPECT_EQ(bad.load(), 0);
    EXPECT_EQ(popped + stolen.load(), kItems);
}

// One owner pushes and pops while several thieves steal; every element is taken exactly once.
TEST(ChaseLevDeque, ConcurrentOwnerAndThievesTakeEachElementOnce) {
    constexpr int kItems = 200'000;
    constexpr int kThieves = 3;
    std::vector<int> values = iota(kItems);
    std::unique_ptr<std::atomic<int>[]> taken = std::make_unique<std::atomic<int>[]>(kItems);
    ChaseLevDeque<int> deque(8); // Small, to force growth under contention.
    std::atomic<bool> ownerDone{false};
    std::atomic<int> total{0};

    auto record = [&](int* item) {
        taken[static_cast<std::size_t>(*item)].fetch_add(1, std::memory_order_relaxed);
        total.fetch_add(1, std::memory_order_relaxed);
    };
    std::vector<std::thread> thieves;
    for (int t = 0; t < kThieves; ++t) {
        thieves.emplace_back([&] {
            for (;;) {
                const auto result = deque.steal();
                if (result.status == Status::Success) {
                    record(result.item);
                } else if (result.status == Status::Empty && ownerDone.load(std::memory_order_acquire)) {
                    return;
                }
            }
        });
    }
    for (int i = 0; i < kItems; ++i) {
        deque.push(&values[static_cast<std::size_t>(i)]);
        if (i % 4 == 0) {
            if (int* item = deque.pop()) {
                record(item);
            }
        }
    }
    while (int* item = deque.pop()) {
        record(item);
    }
    ownerDone.store(true, std::memory_order_release);
    for (std::thread& thief : thieves) {
        thief.join();
    }
    EXPECT_EQ(total.load(), kItems);
    int wrong = 0;
    for (int i = 0; i < kItems; ++i) {
        wrong += taken[static_cast<std::size_t>(i)].load() == 1 ? 0 : 1;
    }
    EXPECT_EQ(wrong, 0);
}

} // namespace
