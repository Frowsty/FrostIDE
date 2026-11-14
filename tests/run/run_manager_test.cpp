// SPDX-License-Identifier: Apache-2.0
//
// RunManager with real processes (label integration): io_test_child started from native run
// configurations through the real IProcessLauncher. Output streaming, the exit code, arguments,
// environment and working directory as the child itself reports them (it is an independent oracle,
// tests/io/io_test_child.cpp), input, one run per configuration, a before-launch handler that
// resolves the program, and a launch failure. Stop of a whole tree is run_manager_tree_test.cpp.

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/run/run_configuration.hpp"
#include "frost/run/run_manager.hpp"

#include "io/process_env.hpp"
#include "run/run_collector.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

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

RunConfiguration child(std::string id, std::vector<std::string> arguments) {
    RunConfiguration configuration;
    configuration.id = std::move(id);
    configuration.name = "io_test_child " + (arguments.empty() ? std::string() : arguments.front());
    configuration.type = RunType::Native;
    configuration.program = test::childPath();
    configuration.arguments = std::move(arguments);
    return configuration;
}

TEST(RunManager, StreamsOutputAndReportsTheExitCode) {
    std::vector<std::string> listened;
    RunManagerOptions options;
    options.listener.onStarted = [&](const RunStatus& status) {
        listened.push_back("started:" + status.configuration.id);
    };
    options.listener.onPhaseChanged = [&](const RunStatus& status) {
        listened.push_back("phase:" + std::string(toString(status.phase)));
    };
    options.listener.onFinished = [&](const RunResult& result) {
        listened.push_back("finished:" + result.configurationId);
    };
    auto manager = RunManager::create(io::realProcessLauncher(), std::move(options));

    test::RunCollector collector;
    auto id = manager->start(child("print", {"print", "out text", "err text"}), collector.handlers());
    ASSERT_TRUE(id.has_value()) << id.error().describe();
    const auto finished = collector.waitFinished(kWait);
    ASSERT_TRUE(finished.result.has_value());
    const RunResult& result = *finished.result;
    EXPECT_EQ(result.id, *id);
    ASSERT_TRUE(result.ran());
    EXPECT_EQ(*result.exitCode, 0);
    EXPECT_FALSE(result.stopped);
    EXPECT_FALSE(result.error.has_value());
    EXPECT_GT(result.duration.count(), 0);
    EXPECT_EQ(finished.out, "out text");
    EXPECT_EQ(finished.err, "err text");
    ASSERT_FALSE(finished.events.empty());
    EXPECT_EQ(finished.events.front(), "phase:Running");
    EXPECT_EQ(finished.events.back(), "finished");
    EXPECT_TRUE(finished.callbacksOnOneThread) << "callbacks run on the dispatcher thread only";
    ASSERT_TRUE(result.plan.has_value());
    EXPECT_EQ(result.plan->program, test::childPath());
    EXPECT_EQ(result.plan->workingDirectory, std::string(io::parentPath(test::childPath())));

    ASSERT_TRUE(manager->waitForIdle(kWait));
    EXPECT_EQ(listened, (std::vector<std::string>{"started:print", "phase:Running", "finished:print"}));
    EXPECT_TRUE(manager->activeRuns().empty());

    test::RunCollector failing;
    ASSERT_TRUE(manager->start(child("exit", {"exit", "7"}), failing.handlers()).has_value());
    const auto failed = failing.waitFinished(kWait);
    ASSERT_TRUE(failed.result && failed.result->ran());
    EXPECT_EQ(*failed.result->exitCode, 7);
}

TEST(RunManager, PassesArgumentsEnvironmentAndWorkingDirectoryExactly) {
    auto manager = RunManager::create(io::realProcessLauncher());
    const std::vector<std::string> arguments{"two words", "", "quote\"inside", "back\\slash\\", "\xC3\xA9t\xC3\xA9"};
    std::vector<std::string> argv{"argv"};
    argv.insert(argv.end(), arguments.begin(), arguments.end());
    test::RunCollector args;
    ASSERT_TRUE(manager->start(child("argv", argv), args.handlers()).has_value());
    const auto argsDone = args.waitFinished(kWait);
    ASSERT_TRUE(argsDone.result && argsDone.result->ran());
    std::string expected = "crt " + std::to_string(arguments.size()) + "\n";
    for (const std::string& argument : arguments) {
        expected += "arg:" + hex(argument) + "\n";
    }
    EXPECT_TRUE(argsDone.out.starts_with(expected)) << argsDone.out;

    RunConfiguration env = child("env", {"env", "FROST_RUN_TEST_VALUE"});
    env.environment = {{"FROST_RUN_TEST_VALUE", "set by the configuration"}};
    test::RunCollector setRun;
    ASSERT_TRUE(manager->start(env, setRun.handlers()).has_value());
    EXPECT_EQ(setRun.waitFinished(kWait).out, "set:" + hex("set by the configuration") + "\n");
    env.environment.push_back({"FROST_RUN_TEST_VALUE", std::nullopt}); // Applied in order: removed again.
    test::RunCollector unsetRun;
    ASSERT_TRUE(manager->start(env, unsetRun.handlers()).has_value());
    EXPECT_EQ(unsetRun.waitFinished(kWait).out, "unset\n");

    test::ScratchDir scratch("run-cwd");
    RunConfiguration cwd = child("cwd", {"cwd"});
    cwd.workingDirectory = scratch.path();
    test::RunCollector cwdRun;
    ASSERT_TRUE(manager->start(cwd, cwdRun.handlers()).has_value());
    EXPECT_EQ(cwdRun.waitFinished(kWait).out, "cwd:" + hex(scratch.path()) + "\n");
    cwd.workingDirectory.clear();
    test::RunCollector folderRun;
    ASSERT_TRUE(manager->start(cwd, folderRun.handlers()).has_value());
    EXPECT_EQ(folderRun.waitFinished(kWait).out, "cwd:" + hex(io::parentPath(test::childPath())) + "\n")
        << "without a working directory the program runs in its own folder";
}

TEST(RunManager, InputReachesTheProgram) {
    auto manager = RunManager::create(io::realProcessLauncher());
    test::RunCollector collector;
    auto id = manager->start(child("cat", {"cat"}), collector.handlers());
    ASSERT_TRUE(id.has_value());
    // Input is accepted from Running on; wait for the phase rather than for time.
    collector.waitUntil([](const test::RunCollector::Snapshot& s) { return !s.phases.empty(); }, kWait);
    const auto status = manager->status(*id);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->phase, RunPhase::Running);
    EXPECT_TRUE(status->acceptsInput);
    EXPECT_NE(status->processId, 0U);
    EXPECT_NE(status->commandLine.find("cat"), std::string::npos) << status->commandLine;
    ASSERT_TRUE(manager->sendInput(*id, "first line\n").has_value());
    ASSERT_TRUE(manager->sendInput(*id, "second\n").has_value());
    manager->closeInput(*id);
    const auto finished = collector.waitFinished(kWait);
    ASSERT_TRUE(finished.result && finished.result->ran());
    EXPECT_EQ(*finished.result->exitCode, 0) << "cat ends at end of file";
    EXPECT_EQ(finished.out, "first line\nsecond\n");
    EXPECT_EQ(manager->sendInput(*id, "late").error().code(), ErrorCode::NotFound);
}

TEST(RunManager, OneRunPerConfigurationAndStopEndsIt) {
    auto manager = RunManager::create(io::realProcessLauncher());
    test::RunCollector first;
    auto id = manager->start(child("sleeper", {"sleep", "60000"}), first.handlers());
    ASSERT_TRUE(id.has_value());
    auto again = manager->start(child("sleeper", {"sleep", "60000"}));
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code(), ErrorCode::Busy);
    EXPECT_EQ(manager->activeRun("sleeper"), *id);

    test::RunCollector second;
    auto other = manager->start(child("other", {"sleep", "60000"}), second.handlers());
    ASSERT_TRUE(other.has_value()) << "different configurations run side by side";
    EXPECT_EQ(manager->activeRuns().size(), 2U);

    first.waitUntil([](const test::RunCollector::Snapshot& s) { return !s.phases.empty(); }, kWait);
    manager->stop(*id);
    manager->stop(*id); // Idempotent.
    const auto stopped = first.waitFinished(kWait);
    ASSERT_TRUE(stopped.result && stopped.result->ran());
    EXPECT_TRUE(stopped.result->stopped);
    EXPECT_EQ(*stopped.result->exitCode, io::kTerminatedExitCode);
    EXPECT_FALSE(stopped.result->cleanupFailed);
    EXPECT_NE(std::ranges::find(stopped.phases, RunPhase::Stopping), stopped.phases.end());
    EXPECT_FALSE(manager->activeRun("sleeper").has_value());

    // stopAll ends the rest (whether its program had started yet or not).
    manager->stopAll();
    const auto ended = second.waitFinished(kWait);
    ASSERT_TRUE(ended.result.has_value());
    EXPECT_TRUE(ended.result->stopped);
    ASSERT_TRUE(manager->waitForIdle(kWait));
    EXPECT_TRUE(manager->activeRuns().empty());
}

TEST(RunManager, ABeforeLaunchHandlerResolvesTheProgram) {
    std::vector<std::jthread> builds;
    RunManagerOptions options;
    options.beforeLaunch = [&](const RunConfiguration& configuration, const std::stop_token&, BeforeLaunchDone done) {
        // The "build" finishes on another thread and names the program, as RUN-2's CMake type will.
        builds.emplace_back([configuration, done = std::move(done)] {
            const LaunchPlan plan{.program = test::childPath(),
                                  .arguments = {"print", "built " + configuration.target, ""},
                                  .workingDirectory = {},
                                  .environment = configuration.environment};
            done(plan);
            done(plan); // Against the contract; the manager ignores it rather than start a second program.
        });
    };
    auto manager = RunManager::create(io::realProcessLauncher(), std::move(options));
    RunConfiguration cmake;
    cmake.id = "tool";
    cmake.name = "Tool";
    cmake.type = RunType::CMakeTarget;
    cmake.target = "Tool";
    cmake.beforeLaunch = BeforeLaunch::Build;
    test::RunCollector collector;
    ASSERT_TRUE(manager->start(cmake, collector.handlers()).has_value());
    const auto finished = collector.waitFinished(kWait);
    ASSERT_TRUE(finished.result && finished.result->ran());
    EXPECT_EQ(*finished.result->exitCode, 0);
    EXPECT_EQ(finished.out, "built Tool");
    builds.clear(); // Both calls made.
    ASSERT_TRUE(manager->waitForIdle(kWait));
    const auto settled = collector.snapshot();
    EXPECT_EQ(std::ranges::count(settled.events, std::string("finished")), 1);
    EXPECT_EQ(settled.out, "built Tool");
}

TEST(RunManager, ALaunchFailureIsReportedNotRun) {
    test::ScratchDir scratch("run-missing");
    auto manager = RunManager::create(io::realProcessLauncher());
    RunConfiguration missing;
    missing.id = "missing";
    missing.name = "Missing";
    missing.program = scratch.join(io::kWindowsProcessRules ? "absent.exe" : "absent");
    test::RunCollector collector;
    ASSERT_TRUE(manager->start(missing, collector.handlers()).has_value()) << "launch errors come through onFinished";
    const auto finished = collector.waitFinished(kWait);
    ASSERT_TRUE(finished.result.has_value());
    EXPECT_FALSE(finished.result->ran());
    ASSERT_TRUE(finished.result->error.has_value());
    EXPECT_EQ(finished.result->error->code(), ErrorCode::NotFound) << finished.result->error->describe();
    EXPECT_FALSE(finished.result->beforeLaunchFailed);
    EXPECT_EQ(finished.events, std::vector<std::string>{"finished"});
}

} // namespace
} // namespace frost::run
