// SPDX-License-Identifier: Apache-2.0
// Authored acceptance against actual externally provisioned, exact-version framework executables.
#include "frost/io/file_system.hpp"
#include "frost/run/test_runner.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <future>
#include <memory>
#include <thread>
namespace frost::run {
namespace {
using namespace std::chrono_literals;
void checkRealFramework(const char* program, FrameworkProfile profile) {
    frost::test::ScratchDir directory("real-framework");
    CTestSetup setup;
    setup.ctest = FROST_TEST_CTEST;
    setup.buildDirectory = directory.path();
    DiscoveredTest test;
    test.name = "fixture";
    test.command = {program};
    test.workingDirectory = directory.path();
    auto runner = TestRunner::create(io::realProcessLauncher(), io::realFileSystem());
    auto listed = std::make_shared<std::promise<Result<std::vector<FrameworkCase>>>>();
    auto list = listed->get_future();
    ASSERT_TRUE(runner->listFrameworkCases(setup, test, profile, [listed](Result<std::vector<FrameworkCase>> result) {
        listed->set_value(std::move(result));
    }));
    ASSERT_EQ(list.wait_for(30s), std::future_status::ready);
    auto cases = list.get();
    ASSERT_TRUE(cases) << cases.error().describe();
    ASSERT_NE(std::ranges::find(*cases, "passes", &FrameworkCase::name), cases->end());
    ASSERT_NE(std::ranges::find(*cases, "fails", &FrameworkCase::name), cases->end());
    ASSERT_TRUE(runner->waitForIdle(5s));
    for (const auto& name : {std::string("passes"), std::string("fails")}) {
        auto finished = std::make_shared<std::promise<TestRunReport>>();
        auto report = finished->get_future();
        ASSERT_TRUE(runner->runFramework(
            setup, {{test, profile, {name}}},
            {.onFinished = [finished](const TestRunReport& result) { finished->set_value(result); }}));
        ASSERT_EQ(report.wait_for(30s), std::future_status::ready);
        const auto result = report.get();
        ASSERT_FALSE(result.error) << result.error->describe();
        ASSERT_EQ(result.results.size(), 1U);
        ASSERT_EQ(result.results.front().cases.size(), 1U);
        EXPECT_EQ(result.results.front().cases.front().name, name);
        EXPECT_EQ(result.results.front().outcome, name == "passes" ? TestOutcome::Passed : TestOutcome::Failed);
        if (name == "fails")
            EXPECT_FALSE(result.results.front().cases.front().failures.empty());
        ASSERT_TRUE(runner->waitForIdle(5s));
    }
}
TEST(RealFramework, Catch2ActualListAndLiteralPassFailureReports) {
    checkRealFramework(FROST_RUN_CATCH_FIXTURE, FrameworkProfile::Catch2Xml3);
}
TEST(RealFramework, BoostActualListAndLiteralPassFailureReports) {
    checkRealFramework(FROST_RUN_BOOST_FIXTURE, FrameworkProfile::BoostTest185);
}
TEST(RealFramework, CancellationNeverManufacturesAPassingCase) {
    frost::test::ScratchDir directory("cancel-framework");
    CTestSetup setup;
    setup.ctest = FROST_TEST_CTEST;
    setup.buildDirectory = directory.path();
    DiscoveredTest test;
    test.name = "cancel";
    test.command = {FROST_RUN_CATCH_FIXTURE};
    const auto ready = directory.join("ready");
    test.environment = {"FROST_FRAMEWORK_READY=" + ready};
    auto runner = TestRunner::create(io::realProcessLauncher(), io::realFileSystem());
    auto finished = std::make_shared<std::promise<TestRunReport>>();
    auto result = finished->get_future();
    ASSERT_TRUE(
        runner->runFramework(setup, {{test, FrameworkProfile::Catch2Xml3, {"slow"}}},
                             {.onFinished = [finished](const TestRunReport& report) { finished->set_value(report); }}));
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!io::realFileSystem().exists(ready).value_or(false) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    ASSERT_TRUE(io::realFileSystem().exists(ready).value_or(false)); // Actual fixture reached its sleeping test body.
    runner->cancel();
    ASSERT_EQ(result.wait_for(30s), std::future_status::ready);
    const auto report = result.get();
    EXPECT_TRUE(report.stopped);
    EXPECT_TRUE(report.results.empty());
}

} // namespace
} // namespace frost::run
