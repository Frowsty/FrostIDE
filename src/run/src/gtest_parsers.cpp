// SPDX-License-Identifier: Apache-2.0
//
// Parsers of Google Test's case list and JSON report, and the attachment of reports to ctest results
// (gtest_parsers.hpp). JSON through frost::rpc's yyjson reader.

#include "frost/run/gtest_parsers.hpp"

#include "frost/rpc/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <utility>

namespace frost::run {

namespace {

constexpr std::string_view kWhitespace = " \t\r\n";

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos)
        return {};
    const auto last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoringAsciiCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return lowerAscii(x) == lowerAscii(y); });
}

// "  # TypeParam = int" / "  # GetParam() = 4": the comment Google Test appends, split off.
std::pair<std::string_view, std::string_view> splitComment(std::string_view text, std::string_view key) {
    const auto hash = text.find("  # ");
    if (hash == std::string_view::npos)
        return {trim(text), {}};
    std::string_view comment = trim(text.substr(hash + 4));
    if (comment.starts_with(key))
        comment = trim(comment.substr(key.size()));
    return {trim(text.substr(0, hash)), comment};
}

// Google Test's "0.123s" as microseconds.
std::optional<std::chrono::microseconds> parseGTestTime(std::string_view text) {
    if (text.ends_with('s'))
        text.remove_suffix(1);
    double seconds = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (ec != std::errc{} || end != text.data() + text.size() || text.empty() || !std::isfinite(seconds) || seconds < 0)
        return std::nullopt;
    return std::chrono::microseconds(static_cast<std::int64_t>(std::llround(seconds * 1e6)));
}

// "file:line\nmessage": Google Test's compiler-independent location on the first line, when there is one.
TestFailure parseFailure(std::string_view text) {
    TestFailure failure;
    const auto newline = text.find('\n');
    const std::string_view first = text.substr(0, newline);
    const auto colon = first.rfind(':');
    int line = 0;
    if (colon != std::string_view::npos && colon > 0) {
        const std::string_view digits = first.substr(colon + 1);
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), line);
        if (ec == std::errc{} && end == digits.data() + digits.size() && !digits.empty() && line > 0) {
            TestLocation location;
            location.file = std::string(first.substr(0, colon));
            location.line = line;
            failure.location = std::move(location);
        }
    }
    failure.message = std::string(failure.location && newline != std::string_view::npos ? text.substr(newline + 1)
                                  : failure.location                                    ? std::string_view{}
                                                                                        : text);
    return failure;
}

std::string_view fileNameOf(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace

Result<std::vector<GTestCase>> parseGTestList(std::string_view output) {
    std::vector<GTestCase> out;
    std::string suite;
    std::string typeParam;
    bool suiteHasCases = true;
    std::size_t lineNumber = 0;
    while (!output.empty()) {
        const auto newline = output.find('\n');
        std::string_view line = output.substr(0, newline);
        output = newline == std::string_view::npos ? std::string_view{} : output.substr(newline + 1);
        ++lineNumber;
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (trim(line).empty())
            continue;
        if (line.starts_with("  ")) {
            if (suite.empty())
                continue; // Indented text before the first suite is not part of the list.
            const auto [name, valueParam] = splitComment(line, "GetParam() = ");
            if (name.empty() || name.find(' ') != std::string_view::npos)
                return fail(ErrorCode::Corrupt,
                            std::format("--gtest_list_tests line {} is not a case: '{}'", lineNumber, line));
            GTestCase testCase;
            testCase.suite = suite;
            testCase.name = std::string(name);
            testCase.typeParam = typeParam;
            testCase.valueParam = std::string(valueParam);
            out.push_back(std::move(testCase));
            suiteHasCases = true;
            continue;
        }
        const auto [head, comment] = splitComment(line, "TypeParam = ");
        // A suite line is one word ending in '.'; anything else (a banner, a warning) is not the list.
        if (!head.ends_with('.') || head.size() < 2 || head.find(' ') != std::string_view::npos) {
            if (!suite.empty())
                return fail(ErrorCode::Corrupt,
                            std::format("--gtest_list_tests line {} is not a suite: '{}'", lineNumber, line));
            continue;
        }
        if (!suiteHasCases)
            return fail(ErrorCode::Corrupt, std::format("--gtest_list_tests suite '{}' lists no case", suite));
        suite = std::string(head.substr(0, head.size() - 1));
        typeParam = std::string(comment);
        suiteHasCases = false;
    }
    if (!suiteHasCases)
        return fail(ErrorCode::Corrupt, std::format("--gtest_list_tests suite '{}' lists no case", suite));
    return out;
}

Result<std::vector<TestResult>> parseGTestJsonReport(std::string_view report) {
    auto parsed = rpc::parseJson(report);
    if (!parsed)
        return fail(ErrorCode::Corrupt,
                    std::format("the Google Test report is not JSON: {}", parsed.error().describe()));
    const rpc::Json* suites = parsed->find("testsuites");
    if (!suites || !suites->isArray())
        return fail(ErrorCode::Corrupt, "the Google Test report has no testsuites array");
    std::vector<TestResult> out;
    for (const rpc::Json& suite : suites->items()) {
        const std::string_view suiteName = suite.stringAt("name");
        const rpc::Json* cases = suite.find("testsuite");
        if (suiteName.empty() || !cases || !cases->isArray())
            continue;
        for (const rpc::Json& item : cases->items()) {
            const std::string_view name = item.stringAt("name");
            const std::string_view status = item.stringAt("status");
            const std::string_view verdict = item.stringAt("result");
            if (name.empty() || verdict.empty())
                continue; // A listing entry: nothing ran.
            TestResult result;
            result.name = std::format("{}.{}", suiteName, name);
            result.duration = parseGTestTime(item.stringAt("time"));
            if (const auto line = item.integerAt("line"); line && *line > 0 && !item.stringAt("file").empty()) {
                TestLocation definition;
                definition.file = std::string(item.stringAt("file"));
                definition.line = static_cast<int>(*line);
                result.definition = std::move(definition);
            }
            if (const rpc::Json* failures = item.find("failures"); failures && failures->isArray()) {
                for (const rpc::Json& failure : failures->items())
                    result.failures.push_back(parseFailure(failure.stringAt("failure")));
            }
            if (status == "NOTRUN" || verdict == "SUPPRESSED") {
                result.outcome = TestOutcome::Disabled;
                result.reason = "Disabled";
            } else if (verdict == "SKIPPED") {
                result.outcome = TestOutcome::Skipped;
                result.reason = "Skipped";
                // GTEST_SKIP's message ("file:line\nmessage"), when the program gave one.
                if (const rpc::Json* skipped = item.find("skipped");
                    skipped && skipped->isArray() && skipped->size() > 0) {
                    const std::string message(trim(parseFailure(skipped->items().front().stringAt("message")).message));
                    if (!message.empty())
                        result.reason = message;
                }
            } else if (verdict == "COMPLETED") {
                result.outcome = result.failures.empty() ? TestOutcome::Passed : TestOutcome::Failed;
                if (!result.failures.empty())
                    result.reason = "Failed";
            } else {
                continue; // A verdict this parser does not know: no result rather than a guess.
            }
            out.push_back(std::move(result));
        }
    }
    return out;
}

std::string gtestProgramStem(std::string_view program) {
    std::string_view name = fileNameOf(program);
    if (name.size() > 4 && equalsIgnoringAsciiCase(name.substr(name.size() - 4), ".exe"))
        name.remove_suffix(4);
    return std::string(name);
}

std::string gtestReportStem(std::string_view fileName) {
    if (fileName.size() <= 5 || !fileName.ends_with(".json"))
        return {};
    std::string_view stem = fileName.substr(0, fileName.size() - 5);
    // Google Test's unique names: <stem>_<n>.json.
    const auto underscore = stem.rfind('_');
    if (underscore != std::string_view::npos && underscore + 1 < stem.size() && underscore > 0 &&
        std::ranges::all_of(stem.substr(underscore + 1), [](char c) { return c >= '0' && c <= '9'; }))
        stem = stem.substr(0, underscore);
    return std::string(stem);
}

void attachGTestReports(std::vector<TestResult>& results, const std::vector<DiscoveredTest>& tests,
                        const std::vector<GTestReport>& reports) {
    std::map<std::string, const DiscoveredTest*, std::less<>> byName;
    for (const DiscoveredTest& test : tests)
        byName.emplace(test.name, &test);
    // Results of this run by the case their filter names, and whole programs by stem.
    std::map<std::string, std::size_t, std::less<>> byFilter;
    std::map<std::string, std::vector<std::size_t>, std::less<>> wholeByStem;
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto found = byName.find(results[i].name);
        if (found == byName.end() || found->second->command.empty())
            continue;
        const DiscoveredTest& test = *found->second;
        if (!test.gtestFilter.empty()) {
            byFilter.emplace(test.gtestFilter, i);
        } else {
            std::string stem = gtestProgramStem(test.command.front());
            std::ranges::transform(stem, stem.begin(), lowerAscii);
            wholeByStem[stem].push_back(i);
        }
    }
    for (const GTestReport& report : reports) {
        std::string stem = report.fileStem;
        std::ranges::transform(stem, stem.begin(), lowerAscii);
        const auto whole = wholeByStem.find(stem);
        const bool oneWholeProgram = whole != wholeByStem.end() && whole->second.size() == 1;
        const bool filteredCase = report.cases.size() == 1 && byFilter.contains(report.cases.front().name);
        // A whole program's report holds its cases; one filtered process writes one case.
        if (oneWholeProgram && !filteredCase) {
            for (const TestResult& testCase : report.cases)
                results[whole->second.front()].cases.push_back(testCase);
            continue;
        }
        for (const TestResult& testCase : report.cases) {
            if (const auto filtered = byFilter.find(testCase.name); filtered != byFilter.end()) {
                TestResult& target = results[filtered->second];
                if (target.failures.empty())
                    target.failures = testCase.failures;
            }
        }
    }
}

} // namespace frost::run
