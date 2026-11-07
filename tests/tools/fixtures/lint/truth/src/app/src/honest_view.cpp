// SPDX-License-Identifier: Apache-2.0
// Code that only reads results for the frost-lint truth-rule tests; none of it is a violation.
#include "fake_runner.hpp"

// A comment may quote "[ RUN      ]" and TestResult{...}; comments are not code.
void show(const TestResult& result, std::vector<TestResult>& all);

int countPassed(const std::vector<TestResult>& results, UnitTestRunState state) {
    std::vector<TestResult> copy = results;
    std::vector<TestResult> none{};
    int passed = 0;
    for (const TestResult& r : copy) {
        if (state != UnitTestRunState::Failed && r.passed)
            ++passed;
    }
    switch (state) {
    case UnitTestRunState::Passed: return passed;
    default: break;
    }
    if (UnitTestRunState::Skipped == state)
        return 0;
    return passed;
}

auto parsedBy(Parser& parser) -> TestResult { return parser.next(); }

const char* kLabel = "Run the tests to see results";

// Declarations, types and copies of existing results are not constructions.
TestResult parse(std::string_view line);
TestResult parseNamed(const std::string& name, int code);
TestResult parsedLine(Parser& parser) { return parser.next(); }
std::function<TestResult()> makeLater;
void keep(std::vector<TestResult>& rows, TestResult existing, const Other& other) {
    rows.push_back(existing);
    rows.emplace_back(std::move(existing));
    const auto& view = static_cast<const TestResult&>(other);
    Config config{.all = {1, 2}};
}
