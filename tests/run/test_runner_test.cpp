// SPDX-License-Identifier: Apache-2.0
//
// RUN-7 acceptance for frost::run's TestRunner with the real tools (label integration, tag cmake):
// fixtures/ctest_project configured by the pinned cmake and Ninja, run by its own ctest (the
// CMAKE_CTEST_COMMAND of its File API cache, through ctestSetupFor), with run_gtest_fixture as a
// Google Test program. Every expectation comes from an oracle, never from this file:
//   - the test list equals the plain `ctest -N` listing (another output of the same tool);
//   - outcomes equal the verdicts ctest prints on its console for the same run;
//   - Google Test cases and failure lines equal the program's own XML report and JSON listing;
//   - cancel ends the whole tree: no process of a copy of run_tree_child is left (process table);
//   - a stub ctest that exits 1 and prints nothing gives the tool error and no results.

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/scripted_process_launcher.hpp"
#include "frost/project/cmake_file_api.hpp"
#include "frost/rpc/json.hpp"
#include "frost/run/test_runner.hpp"

#include "process_table.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::run::TestOutcome;

constexpr auto kToolTimeout = 5min;

frost::io::ProcessResult runTool(const std::string& program, std::vector<std::string> arguments,
                                 const std::string& directory = {}) {
    frost::io::ProcessSpec spec;
    spec.executable = program;
    spec.arguments = std::move(arguments);
    spec.workingDirectory = directory;
    auto result = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, kToolTimeout);
    EXPECT_TRUE(result.has_value()) << program << ": " << (result ? "" : result.error().describe());
    return result ? std::move(*result) : frost::io::ProcessResult{};
}

/*
 * Configures fixtures/ctest_project into <scratch>/<name> with a File API query, reads the reply and
 * returns the setup the product derives from it (ctestSetupFor). `definitions` are extra -D switches.
 */
frost::Result<frost::run::CTestSetup> configureFixture(const frost::test::ScratchDir& scratch, const std::string& name,
                                                       const std::vector<std::string>& definitions) {
    auto& fs = frost::io::realFileSystem();
    const std::string build = scratch.join(name);
    if (auto query = frost::project::writeCMakeFileApiQuery(fs, build); !query)
        return std::unexpected(query.error());
    std::vector<std::string> arguments = {
        "-S",    std::string(FROST_RUN_FIXTURES) + "/ctest_project",     "-B", build, "-G",
        "Ninja", std::string("-DCMAKE_MAKE_PROGRAM=") + FROST_TEST_NINJA};
    arguments.insert(arguments.end(), definitions.begin(), definitions.end());
    const auto configured = runTool(FROST_TEST_CMAKE, arguments);
    if (configured.exitCode != 0)
        return frost::fail(frost::ErrorCode::IoError,
                           "cmake failed: " + configured.stdoutBytes + configured.stderrBytes);
    auto model = frost::project::readCMakeFileApiReply(fs, build);
    if (!model)
        return std::unexpected(model.error());
    if (!model->has_value())
        return frost::fail(frost::ErrorCode::NotFound, "cmake wrote no File API reply");
    frost::project::CMakeProfile profile;
    profile.name = "fixture";
    return frost::run::ctestSetupFor(**model, profile, nullptr);
}

// The oracle list: `ctest -N` as text, "Test #n: name" (ctest appends " (Disabled)" to disabled ones).
struct ListedTest {
    std::size_t number = 0;
    std::string name;
    bool disabled = false;
};
std::vector<ListedTest> ctestListOracle(const frost::run::CTestSetup& setup) {
    const auto listed = runTool(setup.ctest, {"--test-dir", setup.buildDirectory, "-N"});
    EXPECT_EQ(listed.exitCode, 0) << listed.stdoutBytes << listed.stderrBytes;
    std::vector<ListedTest> out;
    const std::regex line(R"(^\s*Test\s+#\s*(\d+): (.*?)\r?$)");
    std::istringstream stream(listed.stdoutBytes);
    for (std::string text; std::getline(stream, text);) {
        std::smatch match;
        if (!std::regex_match(text, match, line))
            continue;
        ListedTest test;
        test.number = std::stoul(match[1].str());
        test.name = match[2].str();
        constexpr std::string_view kDisabled = " (Disabled)";
        if (test.name.ends_with(kDisabled)) {
            test.disabled = true;
            test.name.resize(test.name.size() - kDisabled.size());
        }
        out.push_back(std::move(test));
    }
    EXPECT_FALSE(out.empty()) << listed.stdoutBytes;
    return out;
}

/*
 * The oracle verdicts: the console lines of a plain ctest run of the same tests
 * ("n/m Test #k: name ....   Passed" or "...***Failed"), mapped to outcomes the way ctest's own
 * summary groups them: Passed; Skipped; "Not Run (Disabled)"; other "Not Run"; everything else failed.
 */
std::map<std::string, TestOutcome> ctestRunOracle(const frost::run::CTestSetup& setup,
                                                  const std::vector<ListedTest>& list,
                                                  std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments = {"--test-dir", setup.buildDirectory};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    const auto ran = runTool(setup.ctest, arguments);
    std::map<std::string, TestOutcome> out;
    const std::regex line(R"(^\s*\d+/\d+ Test\s+#\s*(\d+): .*?(   Passed|\*\*\*([^ ].*?))\s+[0-9.]+ sec\r?$)");
    std::istringstream stream(ran.stdoutBytes);
    for (std::string text; std::getline(stream, text);) {
        std::smatch match;
        if (!std::regex_match(text, match, line))
            continue;
        const auto number = std::stoul(match[1].str());
        const auto listed = std::ranges::find(list, number, &ListedTest::number);
        if (listed == list.end())
            continue;
        const std::string verdict = match[3].str();
        TestOutcome outcome = TestOutcome::Failed;
        if (match[2].str() == "   Passed")
            outcome = TestOutcome::Passed;
        else if (verdict == "Skipped")
            outcome = TestOutcome::Skipped;
        else if (verdict == "Not Run (Disabled)")
            outcome = TestOutcome::Disabled;
        else if (verdict.starts_with("Not Run"))
            outcome = TestOutcome::NotRun;
        out[listed->name] = outcome;
    }
    EXPECT_FALSE(out.empty()) << ran.stdoutBytes;
    return out;
}

// One job's answer, waited for with a deadline (the job reports through its callback).
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

frost::Result<std::vector<frost::run::DiscoveredTest>> discover(frost::run::TestRunner& runner,
                                                                const frost::run::CTestSetup& setup) {
    auto answer = std::make_shared<Answer<frost::Result<std::vector<frost::run::DiscoveredTest>>>>();
    auto started = runner.discover(setup, [answer](auto result) { answer->promise.set_value(std::move(result)); });
    if (!started)
        return std::unexpected(started.error());
    auto result = answer->wait();
    if (!result)
        return frost::fail(frost::ErrorCode::Timeout, "discovery did not finish");
    return std::move(*result);
}

std::optional<frost::run::TestRunReport> runTests(frost::run::TestRunner& runner, const frost::run::CTestSetup& setup,
                                                  const std::vector<frost::run::DiscoveredTest>& tests,
                                                  std::vector<std::string> names) {
    auto answer = std::make_shared<Answer<frost::run::TestRunReport>>();
    frost::run::TestRunHandlers handlers;
    handlers.onFinished = [answer](const frost::run::TestRunReport& report) { answer->promise.set_value(report); };
    auto started = runner.run(setup, tests, std::move(names), std::move(handlers));
    EXPECT_TRUE(started.has_value()) << (started ? "" : started.error().describe());
    if (!started)
        return std::nullopt;
    return answer->wait();
}

std::map<std::string, TestOutcome> outcomesOf(const std::vector<frost::run::TestResult>& results) {
    std::map<std::string, TestOutcome> out;
    for (const auto& result : results)
        out[result.name] = result.outcome;
    return out;
}

// Files the runner leaves in ctest's scratch directory (it must leave none).
std::vector<std::string> runnerLeftovers(const frost::run::CTestSetup& setup) {
    std::vector<std::string> out;
    std::error_code error;
    const std::filesystem::path temporary =
        frost::io::toStdPath(setup.buildDirectory).value_or(std::filesystem::path{}) / "Testing" / "Temporary";
    for (const auto& entry : std::filesystem::directory_iterator(temporary, error)) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with("frostide-"))
            out.push_back(name);
    }
    return out;
}

class TestRunnerCTest : public testing::Test {
protected:
    void SetUp() override {
        auto setup =
            configureFixture(m_scratch, "build", {std::string("-DFROST_GTEST_FIXTURE=") + FROST_RUN_GTEST_FIXTURE});
        ASSERT_TRUE(setup.has_value()) << setup.error().describe();
        m_setup = std::move(*setup);
        m_runner = frost::run::TestRunner::create(frost::io::realProcessLauncher(), frost::io::realFileSystem());
    }
    void TearDown() override {
        if (m_runner) {
            m_runner->cancel();
            EXPECT_TRUE(m_runner->waitForIdle(60s));
        }
    }

    frost::test::ScratchDir m_scratch{"run7-ctest"};
    frost::run::CTestSetup m_setup;
    std::unique_ptr<frost::run::TestRunner> m_runner;
};

TEST_F(TestRunnerCTest, TheTestListEqualsCTestDashN) {
    const auto oracle = ctestListOracle(m_setup);
    auto tests = discover(*m_runner, m_setup);
    ASSERT_TRUE(tests.has_value()) << tests.error().describe();
    ASSERT_EQ(tests->size(), oracle.size());
    for (std::size_t i = 0; i < oracle.size(); ++i) {
        EXPECT_EQ((*tests)[i].name, oracle[i].name);
        EXPECT_EQ((*tests)[i].number, oracle[i].number) << oracle[i].name;
        EXPECT_EQ((*tests)[i].disabled, oracle[i].disabled) << oracle[i].name;
        ASSERT_TRUE((*tests)[i].declaration.has_value()) << oracle[i].name;
        EXPECT_TRUE((*tests)[i].declaration->file.ends_with("ctest_project/CMakeLists.txt"))
            << (*tests)[i].declaration->file;
    }
    EXPECT_FALSE(m_runner->busy());
}

TEST_F(TestRunnerCTest, RunAllGivesTheVerdictsCTestPrintsAndTheGoogleTestCases) {
    const auto list = ctestListOracle(m_setup);
    const auto oracle = ctestRunOracle(m_setup, list);
    auto tests = discover(*m_runner, m_setup);
    ASSERT_TRUE(tests.has_value()) << tests.error().describe();

    std::mutex mutex;
    std::vector<std::string> started;
    std::map<std::string, TestOutcome> interim;
    std::string console;
    auto answer = std::make_shared<Answer<frost::run::TestRunReport>>();
    frost::run::TestRunHandlers handlers;
    handlers.onTestStarted = [&](const std::string& name) {
        std::lock_guard lock(mutex);
        started.push_back(name);
    };
    handlers.onTestFinished = [&](const frost::run::TestResult& result) {
        std::lock_guard lock(mutex);
        EXPECT_TRUE(result.interim);
        interim[result.name] = result.outcome;
    };
    handlers.onOutput = [&](std::string_view text) {
        std::lock_guard lock(mutex);
        console += text;
    };
    handlers.onFinished = [answer](const frost::run::TestRunReport& report) { answer->promise.set_value(report); };
    ASSERT_TRUE(m_runner->run(m_setup, *tests, {}, std::move(handlers)).has_value());
    auto report = answer->wait();
    ASSERT_TRUE(report.has_value()) << "the run did not finish";
    ASSERT_FALSE(report->error.has_value()) << report->error->describe();
    EXPECT_FALSE(report->stopped);
    EXPECT_NE(report->exitCode, 0) << "ctest reports the failed tests in its exit code";
    EXPECT_TRUE(report->commandLine.contains("--output-junit")) << report->commandLine;

    EXPECT_EQ(outcomesOf(report->results), oracle);
    {
        std::lock_guard lock(mutex);
        EXPECT_EQ(interim, oracle) << "the progress lines say what the report says";
        EXPECT_EQ(started.size(), oracle.size());
        EXPECT_TRUE(console.contains("tests failed out of")) << console;
    }
    for (const auto& result : report->results) {
        EXPECT_FALSE(result.interim) << result.name;
        EXPECT_TRUE(result.duration.has_value()) << result.name;
    }
    const auto echo = std::ranges::find(report->results, "spaces (and regex+chars) [x]", &frost::run::TestResult::name);
    ASSERT_NE(echo, report->results.end());
    EXPECT_TRUE(echo->output.contains("output <with> & markup")) << echo->output;

    // The Google Test cases, against the program's own XML report.
    frost::test::ScratchDir xmlDir("run7-gtest-xml");
    const std::string xmlPath = xmlDir.join("oracle.xml");
    runTool(FROST_RUN_GTEST_FIXTURE, {"--gtest_output=xml:" + xmlPath});
    auto xml = frost::io::realFileSystem().readFile(xmlPath);
    ASSERT_TRUE(xml.has_value());
    std::map<std::string, TestOutcome> xmlOutcomes;
    std::map<std::string, int> xmlFailureLines;
    std::string suite;
    const std::regex suiteRe(R"re(<testsuite name="([^"]+)")re");
    const std::regex caseRe(R"re(<testcase name="([^"]+)".*? status="([a-z]+)" result="([a-z]+)")re");
    const std::regex failureRe(R"re(<failure message="[^"]*?:(\d+)&#x0A;)re");
    std::istringstream stream(*xml);
    std::string current;
    for (std::string text; std::getline(stream, text);) {
        std::smatch match;
        if (std::regex_search(text, match, suiteRe)) {
            suite = match[1].str();
        } else if (std::regex_search(text, match, caseRe)) {
            current = suite + "." + match[1].str();
            const std::string status = match[2].str();
            const std::string verdict = match[3].str();
            xmlOutcomes[current] = status == "notrun"     ? TestOutcome::Disabled
                                   : verdict == "skipped" ? TestOutcome::Skipped
                                                          : TestOutcome::Passed;
        } else if (std::regex_search(text, match, failureRe)) {
            xmlOutcomes[current] = TestOutcome::Failed;
            xmlFailureLines[current] = std::stoi(match[1].str());
        }
    }
    ASSERT_FALSE(xmlOutcomes.empty()) << *xml;
    ASSERT_EQ(xmlFailureLines.count("Fixture.Fails"), 1U) << *xml;

    const auto program = std::ranges::find(report->results, "gtest_program", &frost::run::TestResult::name);
    ASSERT_NE(program, report->results.end());
    std::map<std::string, TestOutcome> caseOutcomes;
    for (const auto& testCase : program->cases) {
        caseOutcomes[testCase.name] = testCase.outcome;
        if (const auto line = xmlFailureLines.find(testCase.name); line != xmlFailureLines.end()) {
            ASSERT_FALSE(testCase.failures.empty()) << testCase.name;
            ASSERT_TRUE(testCase.failures.front().location.has_value()) << testCase.name;
            EXPECT_EQ(testCase.failures.front().location->line, line->second) << testCase.name;
            EXPECT_TRUE(testCase.failures.front().location->file.ends_with("gtest_fixture.cpp"));
        }
    }
    EXPECT_EQ(caseOutcomes, xmlOutcomes);
    const auto filtered = std::ranges::find(report->results, "Fixture.Fails", &frost::run::TestResult::name);
    ASSERT_NE(filtered, report->results.end());
    ASSERT_FALSE(filtered->failures.empty()) << "the filtered case's Google Test report reached its ctest test";
    EXPECT_EQ(filtered->failures.front().location->line, xmlFailureLines.at("Fixture.Fails"));

    EXPECT_TRUE(runnerLeftovers(m_setup).empty()) << "the runner removes its report files";
}

TEST_F(TestRunnerCTest, ASelectedRunRunsExactlyThoseTestsByFileAndByRegex) {
    const auto list = ctestListOracle(m_setup);
    auto tests = discover(*m_runner, m_setup);
    ASSERT_TRUE(tests.has_value()) << tests.error().describe();
    const std::vector<std::string> selection = {"fails", "spaces (and regex+chars) [x]", "skipped"};
    // The oracle runs the same selection by number (-I), an independent way to pick them.
    std::string numbers = "0,0,0";
    for (const auto& name : selection)
        numbers += "," + std::to_string(std::ranges::find(list, name, &ListedTest::name)->number);
    const auto oracle = ctestRunOracle(m_setup, list, {"-I", numbers});
    ASSERT_EQ(oracle.size(), selection.size());

    auto byFile = runTests(*m_runner, m_setup, *tests, selection);
    ASSERT_TRUE(byFile.has_value());
    ASSERT_FALSE(byFile->error.has_value()) << byFile->error->describe();
    EXPECT_TRUE(byFile->commandLine.contains("--tests-from-file")) << byFile->commandLine;
    EXPECT_EQ(outcomesOf(byFile->results), oracle);

    // ctest before 3.29 has no --tests-from-file: the anchored, escaped -R selects the same tests.
    frost::run::CTestSetup older = m_setup;
    older.ctestVersion = "3.28.0";
    auto byRegex = runTests(*m_runner, older, *tests, selection);
    ASSERT_TRUE(byRegex.has_value());
    ASSERT_FALSE(byRegex->error.has_value()) << byRegex->error->describe();
    EXPECT_FALSE(byRegex->commandLine.contains("--tests-from-file")) << byRegex->commandLine;
    EXPECT_EQ(outcomesOf(byRegex->results), oracle);
    EXPECT_TRUE(runnerLeftovers(m_setup).empty());
}

TEST_F(TestRunnerCTest, ListedGoogleTestCasesEqualTheProgramsOwnListing) {
    auto tests = discover(*m_runner, m_setup);
    ASSERT_TRUE(tests.has_value()) << tests.error().describe();
    const auto program = std::ranges::find(*tests, "gtest_program", &frost::run::DiscoveredTest::name);
    ASSERT_NE(program, tests->end());

    // The oracle: Google Test's structured listing of the same program.
    frost::test::ScratchDir jsonDir("run7-gtest-list");
    const std::string jsonPath = jsonDir.join("list.json");
    runTool(FROST_RUN_GTEST_FIXTURE, {"--gtest_list_tests", "--gtest_output=json:" + jsonPath});
    auto text = frost::io::realFileSystem().readFile(jsonPath);
    ASSERT_TRUE(text.has_value());
    auto json = frost::rpc::parseJson(*text);
    ASSERT_TRUE(json.has_value());
    std::vector<std::string> expected;
    for (const auto& suite : json->find("testsuites")->items())
        for (const auto& testCase : suite.find("testsuite")->items())
            expected.push_back(std::string(suite.stringAt("name")) + "." + std::string(testCase.stringAt("name")));
    ASSERT_FALSE(expected.empty());

    auto answer = std::make_shared<Answer<frost::Result<std::vector<frost::run::GTestCase>>>>();
    ASSERT_TRUE(
        m_runner
            ->listGTestCases(m_setup, *program, [answer](auto result) { answer->promise.set_value(std::move(result)); })
            .has_value());
    auto cases = answer->wait();
    ASSERT_TRUE(cases.has_value());
    ASSERT_TRUE(cases->has_value()) << cases->error().describe();
    std::vector<std::string> names;
    for (const auto& testCase : **cases)
        names.push_back(testCase.fullName());
    EXPECT_EQ(names, expected);

    // A test whose command is cmake -E is no Google Test program: its listing fails, and says why.
    const auto plain = std::ranges::find(*tests, "passes", &frost::run::DiscoveredTest::name);
    auto refused = std::make_shared<Answer<frost::Result<std::vector<frost::run::GTestCase>>>>();
    ASSERT_TRUE(
        m_runner
            ->listGTestCases(m_setup, *plain, [refused](auto result) { refused->promise.set_value(std::move(result)); })
            .has_value());
    auto notGTest = refused->wait();
    ASSERT_TRUE(notGTest.has_value());
    EXPECT_FALSE(notGTest->has_value() && !(*notGTest)->empty()) << "cmake -E lists no Google Test case";
}

TEST_F(TestRunnerCTest, ADirectGoogleTestRunExecutesAndReportsExactlyTheRequestedCase) {
    auto tests = discover(*m_runner, m_setup);
    ASSERT_TRUE(tests.has_value()) << tests.error().describe();
    const auto test = std::ranges::find(*tests, "gtest_program", &frost::run::DiscoveredTest::name);
    ASSERT_NE(test, tests->end());

    auto answer = std::make_shared<Answer<frost::run::TestRunReport>>();
    frost::run::TestRunner::GTestRunRequest request{.test = *test, .cases = {"Fixture.Passes"}};
    frost::run::TestRunHandlers handlers;
    handlers.onFinished = [answer](const frost::run::TestRunReport& report) { answer->promise.set_value(report); };
    ASSERT_TRUE(m_runner->runGTest(m_setup, {std::move(request)}, std::move(handlers)).has_value());
    auto report = answer->wait();
    ASSERT_TRUE(report.has_value());
    ASSERT_FALSE(report->error.has_value()) << report->error->describe();
    ASSERT_EQ(report->results.size(), 1U);
    EXPECT_NE(report->commandLine.find("--gtest_filter=Fixture.Passes"), std::string::npos) << report->commandLine;
    EXPECT_EQ(report->results.front().outcome, TestOutcome::Passed);
    ASSERT_EQ(report->results.front().cases.size(), 1U);
    EXPECT_EQ(report->results.front().cases.front().name, "Fixture.Passes");
    EXPECT_EQ(report->results.front().cases.front().outcome, TestOutcome::Passed);
    EXPECT_TRUE(m_runner->waitForIdle(60s));
    EXPECT_TRUE(runnerLeftovers(m_setup).empty());
}

TEST_F(TestRunnerCTest, AGoogleTestFilterThatMatchesNoCaseCannotReportSuccess) {
    auto tests = discover(*m_runner, m_setup);
    ASSERT_TRUE(tests.has_value()) << tests.error().describe();
    const auto test = std::ranges::find(*tests, "gtest_program", &frost::run::DiscoveredTest::name);
    ASSERT_NE(test, tests->end());

    auto answer = std::make_shared<Answer<frost::run::TestRunReport>>();
    frost::run::TestRunner::GTestRunRequest request{.test = *test, .cases = {"Fixture.DoesNotExist"}};
    frost::run::TestRunHandlers handlers;
    handlers.onFinished = [answer](const frost::run::TestRunReport& report) { answer->promise.set_value(report); };
    ASSERT_TRUE(m_runner->runGTest(m_setup, {std::move(request)}, std::move(handlers)).has_value());
    auto report = answer->wait();
    ASSERT_TRUE(report.has_value());
    ASSERT_TRUE(report->error.has_value());
    EXPECT_TRUE(report->results.empty());
    EXPECT_NE(report->error->message().find("no executed cases"), std::string::npos) << report->error->describe();
    EXPECT_TRUE(m_runner->waitForIdle(60s));
    EXPECT_TRUE(runnerLeftovers(m_setup).empty());
}

TEST_F(TestRunnerCTest, OneJobAtATime) {
    auto first = std::make_shared<Answer<frost::Result<std::vector<frost::run::DiscoveredTest>>>>();
    ASSERT_TRUE(
        m_runner->discover(m_setup, [first](auto result) { first->promise.set_value(std::move(result)); }).has_value());
    auto second = m_runner->discover(m_setup, [](auto) { ADD_FAILURE() << "a second job ran"; });
    // The first job is still starting ctest (milliseconds); the second is refused, not queued.
    if (m_runner->busy()) {
        ASSERT_FALSE(second.has_value());
        EXPECT_EQ(second.error().code(), frost::ErrorCode::Busy);
    }
    auto result = first->wait();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_value());
    EXPECT_TRUE(m_runner->waitForIdle(60s));
    EXPECT_FALSE(m_runner->busy());
}

TEST(TestRunnerCancel, CancelEndsCTestAndEveryProcessItStarted) {
    // A copy of run_tree_child under a name no other test uses, so the process table tells its
    // processes apart from those of tests running beside this one.
    frost::test::ScratchDir scratch("run7-cancel");
    const std::string image = std::format("run7-tree-{}{}", std::chrono::steady_clock::now().time_since_epoch().count(),
#ifdef _WIN32
                                          ".exe");
#else
                                          "");
#endif
    const std::string tree = scratch.join(image);
    std::error_code copyError;
    const auto builtTree = frost::io::toStdPath(FROST_RUN_TREE_CHILD).value();
    std::filesystem::copy_file(builtTree, frost::io::toStdPath(tree).value(), copyError);
    ASSERT_FALSE(copyError) << copyError.message();
#if defined(_WIN32) && __has_feature(address_sanitizer)
    // Relocation must retain the instrumented helper's pinned build-side runtime.
    // The renamed program and every descendant load it from this scratch directory.
    const auto runtime = builtTree.parent_path() / "clang_rt.asan_dynamic-x86_64.dll";
    std::filesystem::copy_file(runtime, frost::io::toStdPath(scratch.path()).value() / runtime.filename(), copyError);
    ASSERT_FALSE(copyError) << "copy required ASan runtime: " << copyError.message();
#endif
#ifndef _WIN32
    std::filesystem::permissions(frost::io::toStdPath(tree).value(), std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
#endif
    auto setup = configureFixture(scratch, "build", {"-DFROST_TREE_PROGRAM=" + tree});
    ASSERT_TRUE(setup.has_value()) << setup.error().describe();
    auto runner = frost::run::TestRunner::create(frost::io::realProcessLauncher(), frost::io::realFileSystem());
    auto tests = discover(*runner, *setup);
    ASSERT_TRUE(tests.has_value()) << tests.error().describe();

    const std::string lowerImage = frost::test::lowerAscii(image);
    const auto treeProcesses = [&] {
        std::size_t count = 0;
        for (const auto& process : frost::test::processTable())
            if (process.image == lowerImage)
                ++count;
        return count;
    };
    auto answer = std::make_shared<Answer<frost::run::TestRunReport>>();
    frost::run::TestRunHandlers handlers;
    handlers.onFinished = [answer](const frost::run::TestRunReport& report) { answer->promise.set_value(report); };
    ASSERT_TRUE(runner->run(*setup, *tests, {"tree"}, std::move(handlers)).has_value());

    // The tree is 1 + 2 + 4 processes once it is built (it then sleeps 60 s).
    const auto deadline = std::chrono::steady_clock::now() + 120s;
    while (treeProcesses() < 7 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(50ms); // Polls the OS process table: the tree signals nothing.
    ASSERT_EQ(treeProcesses(), 7U) << "the test's process tree never came up";
    runner->cancel();
    auto report = answer->wait(120s);
    ASSERT_TRUE(report.has_value()) << "the cancelled run never finished";
    EXPECT_TRUE(report->stopped);
    EXPECT_FALSE(report->error.has_value()) << report->error->describe();
    EXPECT_TRUE(report->results.empty()) << "the tree test never finished, so nothing reports it";
    // Ending a Job Object or process group is asynchronous; the processes go within moments.
    const auto gone = std::chrono::steady_clock::now() + 30s;
    while (treeProcesses() > 0 && std::chrono::steady_clock::now() < gone)
        std::this_thread::sleep_for(50ms);
    EXPECT_EQ(treeProcesses(), 0U) << "cancel left processes of the test's tree running";
    EXPECT_TRUE(runner->waitForIdle(60s));
    EXPECT_TRUE(runnerLeftovers(*setup).empty());
}

TEST(TestRunnerDoctestSelection, WildcardCaseNamesAreRefusedBeforeAnyLaunch) {
    frost::test::ScratchDir scratch("run10-doctest-wildcard");
    frost::run::CTestSetup setup;
    setup.ctest = FROST_RUN_CTEST_STUB;
    setup.buildDirectory = scratch.path();
    frost::io::ScriptedProcessLauncher launcher;
    auto runner = frost::run::TestRunner::create(launcher, frost::io::realFileSystem());
    frost::run::DiscoveredTest test;
    test.name = "doctest";
    test.command = {FROST_RUN_CTEST_STUB, "--test-case=Old"};
    for (const std::string name : {"literal*name", "literal?name", "escaped\\*name"}) {
        SCOPED_TRACE(name);
        const auto launch =
            frost::run::buildSingleTestLaunch(setup, test, name, frost::run::TestCaseFramework::Doctest);
        ASSERT_FALSE(launch.has_value());
        EXPECT_EQ(launch.error().code(), frost::ErrorCode::Unsupported);
        // Validate the entire batch before starting even the preceding, representable case.
        auto run = runner->runDoctest(setup, {{test, {"Foo"}}, {test, {name}}}, {});
        ASSERT_FALSE(run.has_value());
        EXPECT_EQ(run.error().code(), frost::ErrorCode::Unsupported);
        EXPECT_FALSE(runner->busy());
        EXPECT_TRUE(launcher.launches().empty());
    }
}

TEST(TestRunnerDoctestSelection, SingleLaunchReplacesAllCaseAndSensitivityAliases) {
    frost::test::ScratchDir scratch("run10-doctest-case-sensitive");
    frost::run::CTestSetup setup;
    setup.buildDirectory = scratch.path();
    frost::run::DiscoveredTest test;
    test.name = "doctest";
    for (const std::string prefix : {"", "dt-"}) {
        SCOPED_TRACE(prefix);
        test.command = {
            FROST_RUN_CTEST_STUB,     "--test-case=Other", "-tc=Another",    "---dt-tc=Third",
            "--case-sensitive=false", "-cs=false",         "---dt-cs=false", "--" + prefix + "test-suite=Suite",
            "--reporters=xml",        "--out=old.xml"};
        // The existing prefixed aliases require prefixed replacement options, even when the
        // retained suite uses unprefixed options. Commas and backslashes still escape literally.
        const auto launch =
            frost::run::buildSingleTestLaunch(setup, test, "Foo,bar\\baz", frost::run::TestCaseFramework::Doctest);
        ASSERT_TRUE(launch.has_value()) << launch.error().describe();
        EXPECT_EQ(launch->arguments,
                  (std::vector<std::string>{"--" + prefix + "test-suite=Suite", "--dt-case-sensitive=true",
                                            "--dt-test-case=Foo\\,bar\\\\baz"}));
    }
    test.command = {FROST_RUN_CTEST_STUB, "-tc=foo", "-cs=false"};
    const auto unprefixed =
        frost::run::buildSingleTestLaunch(setup, test, "Foo", frost::run::TestCaseFramework::Doctest);
    ASSERT_TRUE(unprefixed.has_value());
    EXPECT_EQ(unprefixed->arguments, (std::vector<std::string>{"--case-sensitive=true", "--test-case=Foo"}));
}

TEST(TestRunnerDoctestSelection, DirectRunStartsOnlyTheReplacementExactCaseFilter) {
    frost::test::ScratchDir scratch("run10-doctest-direct-selection");
    frost::run::CTestSetup setup;
    setup.ctest = FROST_RUN_CTEST_STUB;
    setup.buildDirectory = scratch.path();
    frost::io::ScriptedProcessLauncher launcher;
    frost::io::ScriptedProcess script;
    script.launchError = frost::Error{frost::ErrorCode::NotFound, "capture arguments without starting a program"};
    launcher.expect(std::move(script));
    auto runner = frost::run::TestRunner::create(launcher, frost::io::realFileSystem());
    frost::run::DiscoveredTest test;
    test.name = "doctest";
    test.command = {FROST_RUN_CTEST_STUB, "--test-case=Other", "-dt-tc=foo", "-dt-cs=false", "--case-sensitive=false"};
    auto answer = std::make_shared<Answer<frost::run::TestRunReport>>();
    frost::run::TestRunHandlers handlers;
    handlers.onFinished = [answer](const auto& report) { answer->promise.set_value(report); };
    ASSERT_TRUE(runner->runDoctest(setup, {{test, {"Foo"}}}, std::move(handlers)).has_value());
    const auto report = answer->wait(10s);
    ASSERT_TRUE(report.has_value());
    EXPECT_TRUE(report->error.has_value());
    EXPECT_TRUE(report->results.empty());
    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    const auto& args = launches.front().spec.arguments;
    ASSERT_GE(args.size(), 2U);
    EXPECT_EQ(args[0], "--dt-case-sensitive=true");
    EXPECT_EQ(args[1], "--dt-test-case=Foo");
    EXPECT_FALSE(std::ranges::any_of(
        args, [](const auto& arg) { return arg.contains("Other") || arg.contains("foo") || arg.contains("false"); }));
}

TEST(TestRunnerStub, AStubCTestThatExitsOneGivesTheToolErrorAndNoResults) {
    frost::test::ScratchDir scratch("run7-stub");
    frost::run::CTestSetup setup;
    setup.ctest = frost::io::normalizePath(FROST_RUN_CTEST_STUB).value();
    setup.buildDirectory = scratch.path();
    auto runner = frost::run::TestRunner::create(frost::io::realProcessLauncher(), frost::io::realFileSystem());

    auto tests = discover(*runner, setup);
    ASSERT_FALSE(tests.has_value()) << "a stub that prints nothing listed tests";
    EXPECT_EQ(tests.error().code(), frost::ErrorCode::IoError);
    EXPECT_TRUE(tests.error().message().contains("exited with code 1 and wrote no output")) << tests.error().describe();

    auto report = runTests(*runner, setup, {}, {});
    ASSERT_TRUE(report.has_value());
    ASSERT_TRUE(report->error.has_value()) << "a stub that wrote no report gave results";
    EXPECT_TRUE(report->error->message().contains("exited with code 1 and wrote no output"))
        << report->error->describe();
    EXPECT_TRUE(report->results.empty());
    EXPECT_FALSE(report->stopped);
    EXPECT_TRUE(runner->waitForIdle(60s));
}

} // namespace
