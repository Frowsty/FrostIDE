// SPDX-License-Identifier: Apache-2.0
// Seeded fabrications for the frost-lint truth-rule tests: results built without any tool.
#include "fake_runner.hpp"

TestResult fakeRun(const std::string& name) {
    TestResult result{.name = name, .passed = true, .durationMs = 3};
    return result;
}

void fakeAll(std::vector<TestResult>& out) {
    out.push_back(TestResult{.name = "a", .passed = false});
    TestResult second;
    out.push_back(second);
    auto third = std::make_shared<TestResult>();
}

static constexpr std::array<CatalogItem, 2> kCatalog{{{"Newtonsoft.Json", "13.0.3"}, {"Serilog", "4.0.1"}}};

UnitTestRunState fakeState(bool failed) { return failed ? UnitTestRunState::Failed : UnitTestRunState::Passed; }

const char* kFakeOutput = "[ RUN      ] Suite.Case\n"
                          "[       OK ] Suite.Case (3 ms)\n";
const char* kFakeXunit = "[xUnit.net 00:00:00.01]   Suite.Case [PASS]";
const char* kFakeIl = ".assembly extern System.Runtime {}";
