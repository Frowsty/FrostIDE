// SPDX-License-Identifier: Apache-2.0
// Real process optional-deadline contract: finite deadlines remain finite; cancellation-only runs
// collect real output/exit, and cancellation terminates the actual owned child (no skipped tool).
#include "process_env.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace frost::io {
namespace {
using namespace std::chrono_literals;
TEST(ProcessDeadline, UnboundedCollectsActualBothStreamsAndExit) {
    const std::string input = "actual stdout and stderr\n";
    auto result = runToCompletionUntil(realProcessLauncher(), test::childSpec({"tee"}), input, std::nullopt);
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_EQ(result->exitCode, 0);
    EXPECT_EQ(result->stdoutBytes, input);
    EXPECT_EQ(result->stderrBytes, input);
}
TEST(ProcessDeadline, ExplicitFiniteDeadlineStillTerminatesAndReportsTimeout) {
    auto process = realProcessLauncher().launch(test::childSpec({"sleep", "60000"}));
    ASSERT_TRUE(process) << process.error().message();
    test::ProcessWatch watch((*process)->pid());
    ASSERT_TRUE(watch.valid());
    auto result = (*process)->communicateUntil({}, 10ms);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout);
    EXPECT_TRUE(watch.exitsWithin(5s));
}
class ObservedProcessLauncher final : public IProcessLauncher {
public:
    std::mutex mutex;
    std::condition_variable changed;
    bool started = false;
    ProcessId pid = 0;
    Result<std::unique_ptr<IProcess>> launch(const ProcessSpec& spec) override {
        auto result = realProcessLauncher().launch(spec);
        {
            const std::lock_guard lock(mutex);
            if (result)
                pid = (*result)->pid();
            started = true;
        }
        changed.notify_all();
        return result;
    }
    Result<std::unique_ptr<IAsyncProcess>> launchAsync(const ProcessSpec&, const AsyncContext&) override {
        return fail(ErrorCode::Unsupported, "synchronous process observation");
    }
};
TEST(ProcessDeadline, CancellationOnlyRunStopsActualOwnedChildAndDiscardsOutput) {
    ObservedProcessLauncher launcher;
    std::stop_source stop;
    std::optional<Result<ProcessResult>> result;
    std::atomic<bool> done = false;
    std::jthread worker([&](std::stop_token lifetime) {
        std::stop_callback cancelOnExit(lifetime, [&] { stop.request_stop(); });
        result =
            runToCompletionUntil(launcher, test::childSpec({"sleep", "60000"}), {}, std::nullopt, stop.get_token());
        done = true;
    });
    ProcessId pid = 0;
    {
        std::unique_lock lock(launcher.mutex);
        ASSERT_TRUE(launcher.changed.wait_for(lock, 5s, [&] { return launcher.started; }));
        pid = launcher.pid;
    }
    ASSERT_NE(pid, 0u);
    test::ProcessWatch watch(pid);
    ASSERT_TRUE(watch.valid());
    stop.request_stop();
    const bool exited = watch.exitsWithin(5s);
    if (!exited)
        watch.kill();
    EXPECT_TRUE(exited);
    worker.join();
    ASSERT_TRUE(done);
    ASSERT_TRUE(result);
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().code(), ErrorCode::Cancelled);
}
} // namespace
} // namespace frost::io
