// SPDX-License-Identifier: Apache-2.0
// Authoritative SDK-style project snapshots from dotnet MSBuild structured evaluation.
#pragma once
#include "frost/build/managed_launch_settings.hpp"
#include "frost/build/managed_project.hpp"

#include <map>
namespace frost::build {
struct ManagedEvaluatedItem {
    std::string identity;
    std::map<std::string, std::string, std::less<>> metadata;
    friend bool operator==(const ManagedEvaluatedItem&, const ManagedEvaluatedItem&) = default;
};
struct ManagedProjectEvaluation {
    ManagedProjectRequest request;
    ManagedProjectProperties output;
    std::string projectFile, assemblyName, languageVersion, nullable, defineConstants;
    std::vector<std::string> targetFrameworks;
    std::map<std::string, std::vector<ManagedEvaluatedItem>, std::less<>> items;
    std::map<std::string, std::string, std::less<>> properties; // Actual IsTestProject/CPM/compiler facts.
    // Optional execution receipts captured by an owning worker around a second SDK evaluation.
    std::map<std::string, std::string, std::less<>> inputFiles = {};
    std::string launchSettingsPath = {};
    bool launchSettingsPresent = false;
    std::vector<ManagedLaunchProfile> launchProfiles = {};
};
// Pure bounded command/parser pair. The inner evaluation explicitly selects its TFM; no restore
// or build target runs. All item metadata is MSBuild output, including linked and evaluation-time generated sources.
// Outer TFM discovery uses managedProjectQuerySpec separately. Project loading may execute SDK imports: caller owns
// trust, cancellation and physical identity.
[[nodiscard]] Result<io::ProcessSpec> managedProjectEvaluationSpec(const ManagedProjectRequest& request);
[[nodiscard]] Result<ManagedProjectEvaluation> parseManagedProjectEvaluation(const ManagedProjectRequest& request,
                                                                             std::string_view output);
// Same process/tree/cancellation contract as startManagedProjectQuery; no implicit restore.
[[nodiscard]] Result<std::unique_ptr<BuildSession>>
startManagedProjectEvaluation(io::IProcessLauncher& launcher, const project::TrustStore& trust,
                              const ManagedProjectRequest& request, const std::vector<std::string>& roots,
                              BuildCallbacks callbacks, IClock& clock = systemClock());
} // namespace frost::build
