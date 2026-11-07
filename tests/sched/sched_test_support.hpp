// SPDX-License-Identifier: Apache-2.0
//
// Shared helpers for the frost::sched tests: the build-type flags, environment() and the
// FROST_STRESS_FULL switch (re-exported from tests/support/test_env.hpp, which every module's
// tests share) and bounded waiting, so a scheduler bug fails a test instead of hanging it.
#pragma once

#include "support/test_env.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace frost::sched::test {

using frost::test::environment;
using frost::test::kAssertBudgets;
using frost::test::kOptimized;
using frost::test::kSanitized;
using frost::test::stressFull;

// Polls `predicate` until it is true or `timeout` passes; returns the evaluation that ended the
// wait (each check of the deadline follows one evaluation, never a second one after it).
template<class Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(20)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (predicate()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
}

} // namespace frost::sched::test
