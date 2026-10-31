// SPDX-License-Identifier: Apache-2.0
//
// Tests for Task<T>: lazy start, values, void and move-only results,
// composition, symmetric transfer (deep chains in constant stack), and the
// exception policy at the task boundary (Result tasks get ErrorCode::Internal;
// other tasks abort through the contract-violation handler), and awaiting a
// completed task being a contract violation.

#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"

#include "support/crash_dialogs.hpp"
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::Result;
using frost::sched::syncWait;
using frost::sched::Task;

Task<int> answer(bool& started) {
    started = true;
    co_return 42;
}

TEST(Task, IsLazyUntilAwaited) {
    bool started = false;
    Task<int> task = answer(started);
    EXPECT_TRUE(task.valid());
    EXPECT_FALSE(started);
    EXPECT_EQ(syncWait(std::move(task)), 42);
    EXPECT_TRUE(started);
}

TEST(Task, DestroyingAnUnstartedTaskNeverRunsIt) {
    bool started = false;
    {
        Task<int> task = answer(started);
    }
    EXPECT_FALSE(started);
    Task<int> empty;
    EXPECT_FALSE(empty.valid());
}

Task<void> appendTo(std::vector<int>& out, int value) {
    out.push_back(value);
    co_return;
}

TEST(Task, VoidTasksRunToCompletion) {
    std::vector<int> out;
    syncWait(appendTo(out, 1));
    syncWait(appendTo(out, 2));
    EXPECT_EQ(out, (std::vector<int>{1, 2}));
}

Task<std::unique_ptr<std::string>> makeOwned(std::string text) {
    co_return std::make_unique<std::string>(std::move(text));
}

Task<std::size_t> consumeOwned() {
    std::unique_ptr<std::string> owned = co_await makeOwned("move-only");
    co_return owned->size();
}

TEST(Task, MoveOnlyResultsAreMovedOut) {
    std::unique_ptr<std::string> owned = syncWait(makeOwned("hello"));
    ASSERT_NE(owned, nullptr);
    EXPECT_EQ(*owned, "hello");
    EXPECT_EQ(syncWait(consumeOwned()), 9U);
}

Task<int> add(int a, int b) { co_return a + b; }

Task<int> sumOfSquares(int n) {
    int total = 0;
    for (int i = 1; i <= n; ++i) {
        total += co_await add(i * i, 0);
    }
    co_return total;
}

Task<int> composed() {
    Task<int> first = sumOfSquares(3); // 14
    Task<int> second = add(1, 2);      // 3
    const int a = co_await std::move(first);
    const int b = co_await second; // lvalue await
    co_return a* b;
}

TEST(Task, TasksAwaitTasks) {
    EXPECT_EQ(syncWait(sumOfSquares(4)), 30);
    EXPECT_EQ(syncWait(composed()), 42);
}

Task<int> one() { co_return 1; }

// Each await completes synchronously; without symmetric transfer this would
// nest a million stack frames and overflow.
Task<long long> longChain(int count) {
    long long total = 0;
    for (int i = 0; i < count; ++i) {
        total += co_await one();
    }
    co_return total;
}

TEST(Task, SymmetricTransferKeepsDeepChainsInConstantStack) { EXPECT_EQ(syncWait(longChain(1'000'000)), 1'000'000); }

Task<Result<int>> parse(std::string text) {
    if (text.empty()) {
        co_return frost::fail(ErrorCode::InvalidArgument, "empty");
    }
    co_return std::stoi(text); // Throws std::invalid_argument on garbage.
}

TEST(Task, ResultTasksCarryValuesAndErrors) {
    const Result<int> ok = syncWait(parse("17"));
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, 17);
    const Result<int> bad = syncWait(parse(""));
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code(), ErrorCode::InvalidArgument);
}

TEST(Task, ExceptionInResultTaskBecomesInternalError) {
    const Result<int> result = syncWait(parse("not a number"));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Internal);
    EXPECT_NE(result.error().message().find("exception escaped a task"), std::string::npos);
    EXPECT_NE(result.error().message().find("stoi"), std::string::npos) << result.error().message();
}

Task<Result<int>> throwsCustom() {
    throw 5; // Not derived from std::exception.
    co_return 0;
}

Task<Result<int>> parent() {
    Result<int> child = co_await throwsCustom();
    if (!child) {
        co_return std::unexpected(std::move(child).error().withContext("in parent"));
    }
    co_return *child;
}

TEST(Task, ExceptionErrorsPropagateThroughAwaiters) {
    const Result<int> result = syncWait(parent());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Internal);
    EXPECT_NE(result.error().message().find("not derived from std::exception"), std::string::npos);
    ASSERT_EQ(result.error().context().size(), 1U);
}

Task<int> throwsInPlainTask() {
    throw std::runtime_error("boom");
    co_return 0;
}

TEST(TaskDeathTest, ExceptionInNonResultTaskAborts) {
    EXPECT_DEATH(
        {
            frost::test::disableCrashDialogs();
            (void)syncWait(throwsInPlainTask());
        },
        "no exception escapes a Task body \\(caught: boom\\)");
}

Task<int> fortyTwo() { co_return 42; }

Task<int> awaitTwice(Task<int>& inner) {
    const int first = co_await inner;
    const int second = co_await inner; // The task has completed: a contract violation.
    co_return first + second;
}

TEST(TaskDeathTest, AwaitingACompletedTaskIsAContractViolation) {
    EXPECT_DEATH(
        {
            frost::test::disableCrashDialogs();
            Task<int> inner = fortyTwo();
            (void)syncWait(awaitTwice(inner));
        },
        "precondition violated: valid\\(\\) && !m_handle.done\\(\\)");
}

} // namespace
