// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/build/managed_project.hpp"
#include "frost/run/test_model.hpp"

namespace frost::run {
enum class DotnetTestPlatform { VSTest, MicrosoftTestingPlatform };
struct DotnetTestRequest {
    build::ManagedProjectRequest project{};
    DotnetTestPlatform platform = DotnetTestPlatform::VSTest;
    std::string reportDirectory{}; // Absolute private per-invocation directory; caller creates it.
    std::vector<std::string>
        selected{}; // TRX TestMethod identities only; discovery display strings are never fully qualified identities.
    bool discover = false;
    bool build = false; // Explicit UI choice. Restore remains a separate operation.
};
struct DotnetTestResult {
    std::string id{}, fullyQualifiedName{};
    TestResult result{};
};
[[nodiscard]] Result<io::ProcessSpec> dotnetTestSpec(const DotnetTestRequest& request);
// VSTest's own English --list-tests output, never source declarations. No marker => Corrupt.
[[nodiscard]] Result<std::vector<std::string>> parseDotnetTestDiscovery(std::string_view output);
// Actual TRX result rows with adapter identities, messages and stack traces. Missing/truncated report refuses.
[[nodiscard]] Result<std::vector<DotnetTestResult>> parseDotnetTrx(std::string_view xml);
[[nodiscard]] Result<std::unique_ptr<build::BuildSession>>
startDotnetTest(io::IProcessLauncher& launcher, const project::TrustStore& trust, const DotnetTestRequest& request,
                const std::vector<std::string>& roots, build::BuildCallbacks callbacks, IClock& clock = systemClock());
} // namespace frost::run
