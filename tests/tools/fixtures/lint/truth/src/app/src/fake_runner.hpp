// SPDX-License-Identifier: Apache-2.0
// Result types for the frost-lint truth-rule tests: definitions and defaulted members are fine.
#pragma once

struct TestResult {
    std::string name;
    bool passed = false;
    int durationMs = 0;
};

enum class UnitTestRunState { NotRun, Running, Passed, Failed, Skipped };

struct Row {
    TestResult result;
    TestResult other{};
    UnitTestRunState state = UnitTestRunState::NotRun;
};

struct FakeTestResult {
    int code = 0;
};

struct UsageGroup {
    UsageGroup();
    explicit UsageGroup(int size);
    ~UsageGroup();
};
inline UsageGroup::UsageGroup() = default;
