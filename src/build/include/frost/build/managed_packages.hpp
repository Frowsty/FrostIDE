// SPDX-License-Identifier: Apache-2.0
// Real .NET SDK NuGet queries. Configured sources and credentials stay with NuGet; no credentials enter IDE settings.
#pragma once
#include "frost/build/managed_project.hpp"

namespace frost::build {
enum class PackageQuery { Search, Installed, Outdated, Vulnerable, Deprecated, Restore };
struct ManagedPackageRequest {
    ManagedProjectRequest project{};
    PackageQuery query = PackageQuery::Installed;
    std::string search{};
    std::string source{}; // Empty: NuGet.config hierarchy of the project working directory.
    std::string configFile{};
    bool prerelease = false;
    bool exactMatch = false;
};
struct ManagedPackage {
    std::string id{}, version{}, requested{}, framework{}, source{}, latest{}, status{};
    bool transitive = false;
};
struct ManagedPackageReport {
    std::vector<ManagedPackage> packages{};
    std::vector<std::string> problems{}; // Feed failures remain visible even when another feed returned packages.
};
// Specs never execute. Listing explicitly disables implicit restore; Restore is a separate action.
[[nodiscard]] Result<io::ProcessSpec> managedPackageSpec(const ManagedPackageRequest& request);
[[nodiscard]] Result<ManagedPackageReport> parseManagedPackageReport(std::string_view json, PackageQuery query);
// Trust-gated real process with streamed output and whole-tree cancellation (BuildSession contract).
[[nodiscard]] Result<std::unique_ptr<BuildSession>> startManagedPackageQuery(
    io::IProcessLauncher& launcher, const project::TrustStore& trust, const ManagedPackageRequest& request,
    const std::vector<std::string>& roots, BuildCallbacks callbacks, IClock& clock = systemClock());
}
