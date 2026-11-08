// SPDX-License-Identifier: Apache-2.0
//
// RUN-8: TestRunner's doctest jobs and the doctest part of a ctest run (label integration, tag
// cmake). fixtures/doctest_project is configured by the pinned cmake and Ninja and listed by its own
// ctest; its FrostEngine-style per-suite tests run run_doctest_stub, which hands back captured real
// doctest 2.5.3 reports (fixtures/doctest). What this proves is the runner's side: the command
// lines it starts (the test's own arguments, then doctest's), the working directory and ENVIRONMENT
// it starts them with, that every report is parsed and attached to the right test, that a program
// without a report gives an error and no result, and that Stop ends the program. Expected values
// come from ctest's own list and from the captured reports. doctest itself (FrostEngine's
// FrostTests) is driven by tests/app/corpus_doctest_test.cpp.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/project/cmake_file_api.hpp"
#include "frost/run/doctest_parsers.hpp"
#include "frost/run/test_runner.hpp"

#include "process_table.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::run::TestOutcome;

constexpr auto kToolTimeout = 5min;

std::string fixtures() { return std::string(FROST_RUN_FIXTURES); }

frost::Result<frost::run::CTestSetup> configureFixture(const frost::test::ScratchDir& scratch,
                                                       const std::string& stub) {
    auto& fs = frost::io::realFileSystem();
    const std::string build = scratch.join("build");
    if (auto query = frost::project::writeCMakeFileApiQuery(fs, build); !query)
        return std::unexpected(query.error());
    frost::io::ProcessSpec spec;
    spec.executable = FROST_TEST_CMAKE;
    spec.arguments = {"-S",
                      fixtures() + "/doctest_project",
                      "-B",
                      build,
                      "-G",
                      "Ninja",
                      std::string("-DCMAKE_MAKE_PROGRAM=") + FROST_TEST_NINJA,
                      "-DFROST_DOCTEST_STUB=" + stub,
                      "-DFROST_DOCTEST_FIXTURES=" + fixtures() + "/doctest"};
    auto configured = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, kToolTimeout);
    if (!configured)
        return std::unexpected(configured.error());
    if (configured->exitCode != 0)
        return frost::fail(frost::ErrorCode::IoError,
                           "cmake failed: " + configured->stdoutBytes + configured->stderrBytes);
    auto model = frost::project::readCMakeFileApiReply(fs, build);
    if (!model)
        return std::unexpected(model.error());
    if (!model->has_value())
        return frost::fail(frost::ErrorCode::NotFound, "cmake wrote no File API reply");
    frost::project::CMakeProfile profile;
    profile.name = "fixture";
    return frost::run::ctestSetupFor(**model, profile, nullptr);
}

template<typename T>
struct Answer {
    std::promise<T> promise;
    std::future<T> future = promise.get_future();
    [[nodiscard]] std::optional<T> wait(std::chrono::seconds timeout = 300s) {
        if (future.wait_for(timeout) != std::future_status::ready)
            return std::nullopt;
        return future.get();
    }
};

// The fixture configured, its runner and the tests its ctest lists.
struct Fixture {
    explicit Fixture(std::string_view tag, std::string stubProgram = FROST_RUN_DOCTEST_STUB)
        : scratch(tag), stub(std::move(stubProgram)) {
        auto configured = configureFixture(scratch, stub);
        if (!configured) {
            ADD_FAILURE() << configured.error().describe();
            return;
        }
        setup = std::move(*configured);
        auto answer = std::make_shared<Answer<frost::Result<std::vector<frost::run::DiscoveredTest>>>>();
        auto started = runner->discover(setup, [answer](auto result) { answer->promise.set_value(std::move(result)); });
        if (!started) {
            ADD_FAILURE() << started.error().describe();
            return;
        }
        auto listed = answer->wait();
        if (!listed || !listed->has_value()) {
            ADD_FAILURE() << "ctest listed no tests" << (listed ? listed->error().describe() : "");
            return;
        }
        tests = std::move(**listed);
    }

    [[nodiscard]] const frost::run::DiscoveredTest& test(std::string_view name) const {
        const auto found = std::ranges::find(tests, name, &frost::run::DiscoveredTest::name);
        EXPECT_NE(found, tests.end()) << name;
        static const frost::run::DiscoveredTest none;
        return found == tests.end() ? none : *found;
    }

    std::optional<std::vector<frost::run::TestRunner::DoctestListing>>
    list(std::vector<frost::run::DiscoveredTest> chosen) {
        auto answer = std::make_shared<Answer<std::vector<frost::run::TestRunner::DoctestListing>>>();
        auto started = runner->listDoctestCases(
            setup, std::move(chosen), [answer](auto listings) { answer->promise.set_value(std::move(listings)); });
        EXPECT_TRUE(started.has_value()) << (started ? "" : started.error().describe());
        if (!started)
            return std::nullopt;
        return answer->wait();
    }

    std::optional<frost::run::TestRunReport> run(std::vector<frost::run::TestRunner::DoctestRunRequest> requests,
                                                 frost::run::TestRunHandlers handlers = {}) {
        auto answer = std::make_shared<Answer<frost::run::TestRunReport>>();
        handlers.onFinished = [answer](const frost::run::TestRunReport& report) { answer->promise.set_value(report); };
        auto started = runner->runDoctest(setup, std::move(requests), std::move(handlers));
        EXPECT_TRUE(started.has_value()) << (started ? "" : started.error().describe());
        if (!started)
            return std::nullopt;
        return answer->wait();
    }

    frost::test::ScratchDir scratch;
    std::string stub;
    frost::run::CTestSetup setup;
    std::unique_ptr<frost::run::TestRunner> runner =
        frost::run::TestRunner::create(frost::io::realProcessLauncher(), frost::io::realFileSystem());
    std::vector<frost::run::DiscoveredTest> tests;
};

// The fixture report a stub run hands back, parsed: the oracle of a run's cases.
frost::run::DoctestReport capturedReport(std::string_view name) {
    auto text = frost::io::realFileSystem().readFile(fixtures() + "/doctest/" + std::string(name));
    EXPECT_TRUE(text.has_value()) << name;
    auto report = frost::run::parseDoctestXmlReport(text ? *text : std::string());
    EXPECT_TRUE(report.has_value()) << name;
    return report ? *report : frost::run::DoctestReport{};
}

// Files the runner leaves in ctest's scratch directory (it must leave none).
std::vector<std::string> leftovers(const frost::run::CTestSetup& setup) {
    std::vector<std::string> out;
    std::error_code error;
    const std::filesystem::path temporary =
        frost::io::toStdPath(setup.buildDirectory).value_or(std::filesystem::path{}) / "Testing" / "Temporary";
    for (const auto& entry : std::filesystem::directory_iterator(temporary, error))
        if (entry.path().filename().string().starts_with("frostide-"))
            out.push_back(entry.path().filename().string());
    return out;
}

TEST(DoctestRunner, CTestListsTheSuitesAsDoctestTests) {
    Fixture fixture("run8-discover");
    ASSERT_FALSE(fixture.tests.empty());
    EXPECT_TRUE(frost::run::isDoctestCommand(fixture.test("Doctest.Math").command));
    EXPECT_FALSE(frost::run::isDoctestCommand(fixture.test("plain_fails").command));
    // The ENVIRONMENT property comes through discovery (the runner sets it on its own runs).
    EXPECT_EQ(fixture.test("Doctest.Math").environment,
              (std::vector<std::string>{"FROST_DOCTEST_STUB_ENV=from the ENVIRONMENT property"}));
}

TEST(DoctestRunner, ListsEachTestsCasesFromItsOwnCommandInOrder) {
    Fixture fixture("run8-list");
    ASSERT_FALSE(fixture.tests.empty());
    std::vector<frost::run::DiscoveredTest> chosen;
    // More tests than kDoctestListJobs, so several listings run at once and must come back in order.
    for (int i = 0; i < 3; ++i)
        for (const char* name : {"Doctest.Math", "Doctest.Exit", "Doctest.NoReport"})
            chosen.push_back(fixture.test(name));
    auto listings = fixture.list(chosen);
    ASSERT_TRUE(listings.has_value()) << "the listing never finished";
    ASSERT_EQ(listings->size(), chosen.size());
    auto read = [](std::string_view file) {
        return frost::run::parseDoctestCaseList(
                   frost::io::realFileSystem().readFile(fixtures() + "/doctest/" + std::string(file)).value_or(""))
            .value_or(std::vector<frost::run::DoctestCase>{});
    };
    const auto math = read("list_math.xml");
    const auto all = read("list.xml");
    ASSERT_EQ(math.size(), 7u);
    for (std::size_t i = 0; i < listings->size(); ++i) {
        const auto& listing = (*listings)[i];
        EXPECT_EQ(listing.test, chosen[i].name);
        if (listing.test == "Doctest.Math") {
            ASSERT_TRUE(listing.cases.has_value()) << listing.cases.error().describe();
            EXPECT_EQ(*listing.cases, math);
        } else if (listing.test == "Doctest.Exit") {
            ASSERT_TRUE(listing.cases.has_value()) << listing.cases.error().describe();
            EXPECT_EQ(*listing.cases, all);
        } else {
            // Exit code 2 and no case list: the tool error, no cases.
            ASSERT_FALSE(listing.cases.has_value());
            EXPECT_EQ(listing.cases.error().code(), frost::ErrorCode::IoError);
            EXPECT_NE(listing.cases.error().message().find("exited with code 2"), std::string::npos)
                << listing.cases.error().message();
        }
    }
    EXPECT_TRUE(fixture.runner->waitForIdle(60s));
    EXPECT_TRUE(leftovers(fixture.setup).empty());
}

TEST(DoctestRunner, RunsCasesDirectlyWithTheTestsDirectoryEnvironmentAndArguments) {
    Fixture fixture("run8-run");
    ASSERT_FALSE(fixture.tests.empty());
    frost::run::TestRunner::DoctestRunRequest whole;
    whole.test = fixture.test("Doctest.Math");
    frost::run::TestRunner::DoctestRunRequest exits;
    exits.test = fixture.test("Doctest.Exit");
    exits.cases = {"passes first", "exits, the process"};
    std::vector<std::string> started;
    std::vector<std::string> finished;
    frost::run::TestRunHandlers handlers;
    handlers.onTestStarted = [&started](const std::string& name) { started.push_back(name); };
    handlers.onTestFinished = [&finished](const frost::run::TestResult& r) { finished.push_back(r.name); };
    auto report = fixture.run({whole, exits}, std::move(handlers));
    ASSERT_TRUE(report.has_value()) << "the run never finished";
    EXPECT_FALSE(report->error.has_value()) << report->error->describe();
    EXPECT_FALSE(report->stopped);
    EXPECT_EQ(started, (std::vector<std::string>{"Doctest.Math", "Doctest.Exit"}));
    EXPECT_EQ(finished, started);
    ASSERT_EQ(report->results.size(), 2u);

    const frost::run::TestResult& math = report->results[0];
    const frost::run::DoctestReport mathOracle = capturedReport("run_math.xml");
    EXPECT_EQ(math.name, "Doctest.Math");
    EXPECT_EQ(math.outcome, TestOutcome::Failed);
    EXPECT_EQ(math.reason, std::format("{} of {} case(s) failed", mathOracle.failedCases,
                                       mathOracle.failedCases + mathOracle.passedCases));
    EXPECT_EQ(math.cases, mathOracle.cases);
    EXPECT_TRUE(math.duration.has_value()); // Measured from launch to exit.
    // Where and how the program ran (the stub's first lines).
    // The stub prints its directory with '/' separators (generic_string).
    const std::string work =
        (frost::io::toStdPath(fixture.setup.buildDirectory).value() / "work").lexically_normal().generic_string();
    EXPECT_NE(math.output.find("cwd=" + work), std::string::npos) << math.output;
    EXPECT_NE(math.output.find("env=from the ENVIRONMENT property"), std::string::npos) << math.output;
    EXPECT_NE(math.output.find("args=--no-intro=true --test-suite=Math --stub-list="), std::string::npos)
        << math.output;
    EXPECT_NE(math.output.find("--reporters=xml --duration=true --no-skipped-summary=true --out="), std::string::npos)
        << math.output;
    EXPECT_EQ(math.output.find("--test-case="), std::string::npos) << "a whole test runs without a case filter";

    const frost::run::TestResult& exit = report->results[1];
    EXPECT_EQ(exit.outcome, TestOutcome::Failed);
    EXPECT_EQ(exit.reason, "Exit code 3 before doctest finished its report");
    ASSERT_EQ(exit.cases.size(), 2u);
    EXPECT_EQ(exit.cases[0].outcome, TestOutcome::Passed);
    EXPECT_EQ(exit.cases[1].name, "exits the process");
    EXPECT_EQ(exit.cases[1].outcome, TestOutcome::Failed);
    // The selected cases, escaped as doctest reads them.
    EXPECT_NE(exit.output.find("--test-case=passes first,exits\\, the process --reporters=xml"), std::string::npos)
        << exit.output;
    EXPECT_TRUE(fixture.runner->waitForIdle(60s));
    EXPECT_TRUE(leftovers(fixture.setup).empty());
}

TEST(DoctestRunner, AProgramWithoutAReportGivesAnErrorAndNoResultTheOthersKeepTheirs) {
    Fixture fixture("run8-noreport");
    ASSERT_FALSE(fixture.tests.empty());
    frost::run::TestRunner::DoctestRunRequest none;
    none.test = fixture.test("Doctest.NoReport");
    frost::run::TestRunner::DoctestRunRequest math;
    math.test = fixture.test("Doctest.Math");
    auto report = fixture.run({none, math});
    ASSERT_TRUE(report.has_value());
    ASSERT_TRUE(report->error.has_value());
    EXPECT_NE(report->error->message().find("Doctest.NoReport"), std::string::npos) << report->error->message();
    EXPECT_NE(report->error->message().find("exited with code 2"), std::string::npos) << report->error->message();
    EXPECT_NE(report->error->message().find("wrote no doctest report"), std::string::npos) << report->error->message();
    ASSERT_EQ(report->results.size(), 1u);
    EXPECT_EQ(report->results[0].name, "Doctest.Math");
}

TEST(DoctestRunner, ACTestRunOfAFailedDoctestSuiteNamesItsFailedCasesFromTheConsole) {
    Fixture fixture("run8-ctest");
    ASSERT_FALSE(fixture.tests.empty());
    auto answer = std::make_shared<Answer<frost::run::TestRunReport>>();
    frost::run::TestRunHandlers handlers;
    handlers.onFinished = [answer](const frost::run::TestRunReport& report) { answer->promise.set_value(report); };
    ASSERT_TRUE(fixture.runner->run(fixture.setup, fixture.tests, {"Doctest.Math", "plain_fails"}, std::move(handlers))
                    .has_value());
    auto report = answer->wait();
    ASSERT_TRUE(report.has_value());
    EXPECT_FALSE(report->error.has_value()) << report->error->describe();
    const auto math = std::ranges::find(report->results, "Doctest.Math", &frost::run::TestResult::name);
    ASSERT_NE(math, report->results.end());
    EXPECT_EQ(math->outcome, TestOutcome::Failed);
    // The oracle: the failure blocks of the console output the program printed (ctest's report keeps it).
    const auto console = frost::io::realFileSystem().readFile(fixtures() + "/doctest/console_math.txt");
    ASSERT_TRUE(console.has_value());
    const auto expected = frost::run::parseDoctestConsoleFailures(*console);
    ASSERT_FALSE(expected.empty());
    ASSERT_EQ(math->cases.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(math->cases[i].name, expected[i].name);
        EXPECT_EQ(math->cases[i].outcome, TestOutcome::Failed);
        EXPECT_EQ(math->cases[i].failures.front().location, expected[i].failures.front().location);
    }
    const auto plain = std::ranges::find(report->results, "plain_fails", &frost::run::TestResult::name);
    ASSERT_NE(plain, report->results.end());
    EXPECT_EQ(plain->outcome, TestOutcome::Failed);
    EXPECT_TRUE(plain->cases.empty()) << "a program that is no doctest program gets no cases";
}

TEST(DoctestRunner, StopEndsTheRunningProgram) {
    // A copy of the stub under a name no other test uses, so the process table tells it apart.
    frost::test::ScratchDir copies("run8-cancel-stub");
    const std::string image = std::format("run8-stub-{}{}", std::chrono::steady_clock::now().time_since_epoch().count(),
#ifdef _WIN32
                                          ".exe");
#else
                                          "");
#endif
    const std::string stub = copies.join(image);
    std::error_code copyError;
    std::filesystem::create_directories(frost::io::toStdPath(copies.path()).value(), copyError);
    std::filesystem::copy_file(frost::io::toStdPath(FROST_RUN_DOCTEST_STUB).value(), frost::io::toStdPath(stub).value(),
                               copyError);
    ASSERT_FALSE(copyError) << copyError.message();
#ifndef _WIN32
    std::filesystem::permissions(frost::io::toStdPath(stub).value(), std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
#endif
    Fixture fixture("run8-cancel", stub);
    ASSERT_FALSE(fixture.tests.empty());
    const std::string lowerImage = frost::test::lowerAscii(image);
    const auto stubs = [&] {
        return std::ranges::count_if(frost::test::processTable(), [&](const auto& p) { return p.image == lowerImage; });
    };
    auto answer = std::make_shared<Answer<frost::run::TestRunReport>>();
    frost::run::TestRunHandlers handlers;
    handlers.onFinished = [answer](const frost::run::TestRunReport& report) { answer->promise.set_value(report); };
    frost::run::TestRunner::DoctestRunRequest slow;
    slow.test = fixture.test("Doctest.Slow");
    frost::run::TestRunner::DoctestRunRequest after;
    after.test = fixture.test("Doctest.Math");
    const auto begun = std::chrono::steady_clock::now();
    ASSERT_TRUE(fixture.runner->runDoctest(fixture.setup, {slow, after}, std::move(handlers)).has_value());
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (stubs() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(50ms); // Polls the OS process table: the stub signals nothing.
    ASSERT_EQ(stubs(), 1) << "the slow suite's program never started";
    fixture.runner->cancel();
    auto report = answer->wait(60s);
    ASSERT_TRUE(report.has_value()) << "the stopped run never finished";
    EXPECT_TRUE(report->stopped);
    EXPECT_FALSE(report->error.has_value()) << report->error->describe();
    EXPECT_TRUE(report->results.empty()) << "the stopped program reported no case, and the next never started";
    EXPECT_LT(std::chrono::steady_clock::now() - begun, 55s) << "the program ran out its sleep";
    const auto gone = std::chrono::steady_clock::now() + 30s;
    while (stubs() > 0 && std::chrono::steady_clock::now() < gone)
        std::this_thread::sleep_for(50ms);
    EXPECT_EQ(stubs(), 0);
    EXPECT_TRUE(fixture.runner->waitForIdle(60s));
    EXPECT_TRUE(leftovers(fixture.setup).empty());
}

} // namespace
