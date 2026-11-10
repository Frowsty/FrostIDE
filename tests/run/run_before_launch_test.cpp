// SPDX-License-Identifier: Apache-2.0
//
// RUN-5 before-launch tasks with real processes (label integration): io_test_child as the tasks and
// as the program, started by the RunManager through the real IProcessLauncher. What ran, in which
// order and where comes from the children's own output (an independent oracle,
// tests/io/io_test_child.cpp); that nothing is left running after Stop comes from the OS process
// table (process_table.hpp). Covers: tasks run in order before the program and stream their output,
// a failing task or one that cannot start keeps the program from starting, Stop during a task ends
// its tree and starts nothing, Prepare (Debug) runs the tasks too, and compound and attach
// configurations are refused by the manager (they start no program of their own).

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_manager.hpp"

#include "io/process_env.hpp"
#include "run/process_table.hpp"
#include "run/run_collector.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace frost::run {
namespace {

using namespace std::chrono_literals;

constexpr auto kWait = 60s; // Generous: the machine may be building in parallel.

std::string hex(std::string_view bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        out += kDigits[c >> 4];
        out += kDigits[c & 15];
    }
    return out;
}

BeforeLaunchTask task(std::vector<std::string> arguments, std::string workingDirectory = {}) {
    return BeforeLaunchTask{.program = test::childPath(),
                            .arguments = std::move(arguments),
                            .workingDirectory = std::move(workingDirectory)};
}

RunConfiguration program(std::string id, std::vector<std::string> arguments, std::vector<BeforeLaunchTask> tasks) {
    RunConfiguration configuration;
    configuration.id = std::move(id);
    configuration.name = "program " + configuration.id;
    configuration.type = RunType::Native;
    configuration.program = test::childPath();
    configuration.arguments = std::move(arguments);
    configuration.beforeLaunchTasks = std::move(tasks);
    return configuration;
}

io::ProcessId selfPid() {
#ifdef _WIN32
    return static_cast<io::ProcessId>(GetCurrentProcessId());
#else
    return static_cast<io::ProcessId>(getpid());
#endif
}

// The live descendants of this test process whose image is io_test_child.
std::vector<io::ProcessId> childrenLeft() {
    const auto table = test::processTable();
    const std::string image = test::lowerAscii(io::fileName(test::childPath()));
    std::vector<io::ProcessId> left;
    for (const io::ProcessId pid : test::descendantsOf(selfPid(), table)) {
        const auto entry = std::ranges::find(table, pid, &test::OsProcess::pid);
        if (entry != table.end() && entry->image == image) {
            left.push_back(pid);
        }
    }
    return left;
}

TEST(RunBeforeLaunch, TasksRunInOrderBeforeTheProgramAndStreamTheirOutput) {
    frost::test::ScratchDir scratch("run-tasks");
    std::vector<std::string> taskLines;
    RunManagerOptions options;
    options.listener.onPhaseChanged = [&](const RunStatus& status) {
        if (status.phase == RunPhase::BeforeLaunch) {
            taskLines.push_back(status.beforeLaunchTask);
        }
    };
    auto manager = RunManager::create(io::realProcessLauncher(), std::move(options));
    test::RunCollector collector;
    // Task 1 prints; task 2 reports the working directory it was given; then the program prints.
    const RunConfiguration configuration = program("ordered", {"print", "[program]", ""},
                                                   {task({"print", "[task one]", ""}), task({"cwd"}, scratch.path())});
    ASSERT_TRUE(manager->start(configuration, collector.handlers()).has_value());
    const auto finished = collector.waitFinished(kWait);
    ASSERT_TRUE(finished.result.has_value());
    ASSERT_TRUE(finished.result->ran()) << (finished.result->error ? finished.result->error->describe() : "");
    EXPECT_EQ(*finished.result->exitCode, 0);
    EXPECT_FALSE(finished.result->beforeLaunchFailed);
    // The children's own output, in run order: task one, task two's directory, the program.
    EXPECT_EQ(finished.out, "[task one]cwd:" + hex(scratch.path()) + "\n[program]");
    ASSERT_TRUE(manager->waitForIdle(kWait));
    ASSERT_EQ(taskLines.size(), 2U);
    EXPECT_NE(taskLines[0].find("[task one]"), std::string::npos) << taskLines[0];
    EXPECT_NE(taskLines[1].find("cwd"), std::string::npos) << taskLines[1];
    // Before-launch phases (one per task) come before Running.
    const auto running = std::ranges::find(finished.phases, RunPhase::Running);
    ASSERT_NE(running, finished.phases.end());
    EXPECT_EQ(std::count(finished.phases.begin(), running, RunPhase::BeforeLaunch), 2);
}

TEST(RunBeforeLaunch, AFailingTaskKeepsTheProgramFromStarting) {
    auto manager = RunManager::create(io::realProcessLauncher());
    test::RunCollector collector;
    ASSERT_TRUE(
        manager
            ->start(program("failing", {"print", "[program]", ""},
                            {task({"print", "[before]", ""}), task({"exit", "3"}), task({"print", "[never]", ""})}),
                    collector.handlers())
            .has_value());
    const auto finished = collector.waitFinished(kWait);
    ASSERT_TRUE(finished.result.has_value());
    EXPECT_FALSE(finished.result->ran());
    EXPECT_TRUE(finished.result->beforeLaunchFailed);
    EXPECT_FALSE(finished.result->stopped);
    ASSERT_TRUE(finished.result->error.has_value());
    EXPECT_NE(finished.result->error->message().find("exited with code 3"), std::string::npos)
        << finished.result->error->message();
    EXPECT_EQ(finished.out, "[before]") << "only the tasks before the failing one ran";

    // A task program that does not exist: the launcher's error, and nothing else runs.
    frost::test::ScratchDir scratch("run-tasks-missing");
    RunConfiguration missing = program("missing", {"print", "[program]", ""}, {});
    missing.beforeLaunchTasks.push_back(
        BeforeLaunchTask{.program = scratch.join("no-such-tool.exe"), .arguments = {}, .workingDirectory = {}});
    test::RunCollector second;
    ASSERT_TRUE(manager->start(missing, second.handlers()).has_value());
    const auto refused = second.waitFinished(kWait);
    ASSERT_TRUE(refused.result.has_value());
    EXPECT_FALSE(refused.result->ran());
    EXPECT_TRUE(refused.result->beforeLaunchFailed);
    ASSERT_TRUE(refused.result->error.has_value());
    EXPECT_NE(refused.result->error->describe().find("before-launch task"), std::string::npos)
        << refused.result->error->describe();
    EXPECT_TRUE(refused.out.empty()) << refused.out;
}

TEST(RunBeforeLaunch, StopDuringATaskEndsItsTreeAndStartsNothing) {
    auto manager = RunManager::create(io::realProcessLauncher());
    test::RunCollector collector;
    std::atomic<bool> taskRunning{false};
    RunHandlers handlers = collector.handlers();
    auto record = handlers.onPhaseChanged;
    handlers.onPhaseChanged = [&, record](const RunStatus& status) {
        if (status.phase == RunPhase::BeforeLaunch && !status.beforeLaunchTask.empty()) {
            taskRunning = true;
        }
        record(status);
    };
    const auto begun = std::chrono::steady_clock::now();
    auto id = manager->start(program("stopped", {"print", "[program]", ""}, {task({"sleep", "120000"})}),
                             std::move(handlers));
    ASSERT_TRUE(id.has_value()) << id.error().describe();
    ASSERT_TRUE(collector.waitUntil([&](const auto&) { return taskRunning.load(); }, kWait).events.size() > 0);
    ASSERT_TRUE(taskRunning.load());
    EXPECT_FALSE(childrenLeft().empty()) << "the sleeping task is not in the process table";
    manager->stop(*id);
    const auto finished = collector.waitFinished(kWait);
    ASSERT_TRUE(finished.result.has_value());
    EXPECT_TRUE(finished.result->stopped);
    EXPECT_FALSE(finished.result->ran());
    EXPECT_FALSE(finished.result->beforeLaunchFailed);
    EXPECT_LT(std::chrono::steady_clock::now() - begun, 100s) << "Stop waited for the task instead of ending it";
    EXPECT_TRUE(finished.out.empty()) << finished.out;
    ASSERT_TRUE(manager->waitForIdle(kWait));
    EXPECT_TRUE(childrenLeft().empty()) << "Stop left the task's process running";
}

TEST(RunBeforeLaunch, PrepareRunsTheTasksAndStartsNoProgram) {
    auto manager = RunManager::create(io::realProcessLauncher());
    test::RunCollector collector;
    ASSERT_TRUE(manager
                    ->start(program("prepared", {"print", "[program]", ""}, {task({"print", "[task]", ""})}),
                            collector.handlers(), RunMode::Prepare)
                    .has_value());
    const auto finished = collector.waitFinished(kWait);
    ASSERT_TRUE(finished.result.has_value());
    EXPECT_FALSE(finished.result->ran());
    EXPECT_FALSE(finished.result->error.has_value())
        << (finished.result->error ? finished.result->error->describe() : "");
    ASSERT_TRUE(finished.result->plan.has_value());
    EXPECT_EQ(finished.result->plan->program, test::childPath());
    EXPECT_EQ(finished.out, "[task]");

    // A failing task fails the preparation: the debugger gets the error, not a program.
    test::RunCollector failing;
    ASSERT_TRUE(manager
                    ->start(program("prepared-failing", {"print", "[program]", ""}, {task({"exit", "5"})}),
                            failing.handlers(), RunMode::Prepare)
                    .has_value());
    const auto failed = failing.waitFinished(kWait);
    ASSERT_TRUE(failed.result.has_value());
    EXPECT_TRUE(failed.result->beforeLaunchFailed);
    ASSERT_TRUE(failed.result->error.has_value());
    EXPECT_NE(failed.result->error->message().find("exited with code 5"), std::string::npos);
}

TEST(RunBeforeLaunch, CompoundAndAttachConfigurationsStartNoProgramHere) {
    auto manager = RunManager::create(io::realProcessLauncher());
    RunConfiguration compound;
    compound.id = "both";
    compound.name = "Both";
    compound.type = RunType::Compound;
    compound.configurations = {"a", "b"};
    auto refused = manager->start(compound);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
    RunConfiguration attach;
    attach.id = "attach";
    attach.name = "Attach";
    attach.type = RunType::Attach;
    attach.target = "io_test_child.exe";
    EXPECT_EQ(manager->start(attach).error().code(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(manager->activeRuns().empty());
}

} // namespace
} // namespace frost::run
