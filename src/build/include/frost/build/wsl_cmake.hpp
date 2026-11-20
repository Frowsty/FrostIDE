// SPDX-License-Identifier: Apache-2.0
// CMK-16 named WSL/Ninja configure/build command plans. No tool or distribution discovery.
#pragma once
#include "frost/build/remote_target.hpp"
namespace frost::build {
struct WslCMakeProfile {
    std::string id,name,targetId,localSourceDirectory;
    std::string cmakeProgram="/usr/bin/cmake",ninjaProgram="/usr/bin/ninja",buildDirectory;
    std::vector<std::string> cacheDefinitions; // KEY[:TYPE]=VALUE; passed as one -D argv element each.
    std::string buildTarget; // Empty invokes the generator default; never an inferred target list.
    friend bool operator==(const WslCMakeProfile&,const WslCMakeProfile&)=default;
};
enum class WslCMakeOperation {Configure,Build,Clean,ListTargets};
// Windows IDE -> explicitly named WSL distribution. Source mapping must be reversible;
// build output must be separate from source. Command plans preserve remote POSIX paths.
[[nodiscard]] Result<io::ProcessSpec> wslCMakeCommand(const WslCMakeProfile& profile,const RemoteTarget& target,
                                                    WslCMakeOperation operation,unsigned jobs);
[[nodiscard]] Result<std::vector<WslCMakeProfile>> parseWslCMakeProfiles(std::string_view bytes);
[[nodiscard]] std::string wslCMakeProfilesJson(const std::vector<WslCMakeProfile>& profiles);
// Actual remote probe output is checked against sdk.lock pins before configure/build execution.
[[nodiscard]] Result<void> validateWslCMakeToolVersions(std::string_view cmakeOutput,std::string_view ninjaOutput);
} // namespace frost::build
