// SPDX-License-Identifier: Apache-2.0
// Defines shader diagnostic requests and results for DirectX Shader Compiler.
#pragma once
#include "frost/build/build_diagnostics.hpp"
#include "frost/io/process.hpp"
namespace frost::app {
struct DxcProfile {
    std::string executable, root, source, entry, target;
};
struct DxcReport {
    int exitCode = 0;
    std::string output;
    std::vector<build::BuildDiagnostic> diagnostics;
};
Result<io::ProcessSpec> dxcSpec(const DxcProfile& profile);
Result<std::vector<build::BuildDiagnostic>> parseDxcDiagnostics(std::string_view output, std::string_view root);
Result<DxcReport> runDxc(io::IProcessLauncher& launcher, const DxcProfile& profile, std::stop_token stop = {});
} // namespace frost::app
