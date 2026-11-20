// SPDX-License-Identifier: Apache-2.0
// Deferred diagnostic admission/argv contract. No fake metrics stand in for real acceptance.
#include "frost/run/managed_diagnostics.hpp"

#include <gtest/gtest.h>

#include <algorithm>
namespace frost::run {
TEST(ManagedDiagnostics, RequiresRetainedTargetAdmissionAndFullGcConsent) {
    ManagedDiagnosticRequest request;
#ifdef _WIN32
    request.tool = "C:/tools/dotnet-trace.exe";
    request.workingDirectory = "C:/project";
    request.outputDirectory = "C:/captures/new-session";
#else
    request.tool = "/tools/dotnet-trace";
    request.workingDirectory = "/project";
    request.outputDirectory = "/captures/new-session";
#endif
    request.expectedVersion = "10.0.123456+reviewed";
    request.processId = 42;
    EXPECT_FALSE(managedDiagnosticSpec(request));
    request.admit = []() -> Result<void> { return {}; };
    auto trace = managedDiagnosticSpec(request);
    ASSERT_TRUE(trace);
    EXPECT_NE(std::ranges::find(trace->arguments, "dotnet-common,dotnet-sampled-thread-time"), trace->arguments.end());
    request.kind = ManagedDiagnosticKind::GcDump;
    EXPECT_FALSE(managedDiagnosticSpec(request));
    request.allowFullGc = true;
    EXPECT_TRUE(managedDiagnosticSpec(request));
    request.durationSeconds = 301;
    EXPECT_FALSE(managedDiagnosticSpec(request));
    request.durationSeconds = 60;
    request.kind = ManagedDiagnosticKind::Counters;
    auto counters = managedDiagnosticSpec(request);
    ASSERT_TRUE(counters);
    EXPECT_NE(std::ranges::find(counters->arguments, "00:00:01:00"), counters->arguments.end());
}
} // namespace frost::run
