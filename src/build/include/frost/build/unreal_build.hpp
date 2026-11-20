// SPDX-License-Identifier: Apache-2.0
// User-invoked installed Unreal tooling; pure argv plans and a trust-gated BuildSession.
#pragma once
#include "frost/build/build_session.hpp"
#include "frost/io/process.hpp"

#include <memory>
#include <string>
namespace frost::project {
class TrustStore;
}
namespace frost::build {
enum class UnrealOperation { Build, Clean, GenerateProjectFiles, GenerateCompileDatabase, HeaderTool };
enum class UnrealPlatform { Win64, Linux };
enum class UnrealConfiguration { Debug, DebugGame, Development, Shipping, Test };
struct UnrealBuildRequest {
    // Absolute executable selected by the user. Direct UBT executable, or selected dotnet host
    // with absolute buildToolAssembly. No PATH discovery, shell/batch wrapper or engine scan.
    std::string tool;
    std::string buildToolAssembly;
    std::string projectFile;
    std::string target; // Explicit UBT target from user selection; never inferred from project name.
    UnrealPlatform platform = UnrealPlatform::Win64;
    UnrealConfiguration configuration = UnrealConfiguration::Development;
    UnrealOperation operation = UnrealOperation::Build;
    std::string headerManifest; // Explicit existing UHT manifest; required only for HeaderTool.
};
// No execution or filesystem access. Inputs reject NUL, relative paths and switch-like target names.
// Caller owns tool/version selection, project saving, freshness and any remote translation.
[[nodiscard]] Result<io::ProcessSpec> unrealBuildSpec(const UnrealBuildRequest& request);
// Call only from an explicit user action. UBT can execute arbitrary project rules and build steps;
// this checks project-directory trust before process creation. BuildSession owns tree cancellation,
// real output, exit status and diagnostics. It never treats stdout text as a successful build.
[[nodiscard]] Result<std::unique_ptr<BuildSession>>
startUnrealBuild(io::IProcessLauncher& launcher, const project::TrustStore& trust, const UnrealBuildRequest& request,
                 BuildCallbacks callbacks, IClock& clock = systemClock());
} // namespace frost::build
