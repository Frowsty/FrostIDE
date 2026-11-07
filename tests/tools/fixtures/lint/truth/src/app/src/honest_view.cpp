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
