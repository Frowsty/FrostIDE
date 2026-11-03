// SPDX-License-Identifier: Apache-2.0
// Typed SDK-project evaluation and build command construction for managed console projects.
#pragma once

#include "frost/build/build_session.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace frost::project {
class TrustStore;
}

namespace frost::build {

// The caller must check project trust before starting either process and recheck it after build.
struct ManagedProjectRequest {
    std::string dotnet{};            // Absolute host executable.
    std::string project{};           // Absolute .csproj.
    std::string configuration{};     // Explicit configuration.
    std::string targetFramework{};   // Required for build and final evaluation.
    std::string runtimeIdentifier{}; // Empty means no explicit RID.
};

struct ManagedProjectProperties {
    std::string targetPath{};
    std::string targetFramework{};
    std::string targetFrameworks{};
    std::string outputType{};
    std::string runtimeIdentifier{};
};

/* Pure process specifications. The query may omit targetFramework to discover a multi-target
 * project; the build requires it. Both contain the process tree and use absolute paths, pipes,
 * and structured argv. They reject invalid paths or property values with InvalidArgument.
 * Independent calls are thread-safe and cost O(input size). Neither function checks trust or runs
 * project code; callers own trust, cancellation, workspace generation and source revision checks.
 */
[[nodiscard]] Result<io::ProcessSpec> managedProjectQuerySpec(const ManagedProjectRequest& request);
[[nodiscard]] Result<io::ProcessSpec> managedProjectBuildSpec(const ManagedProjectRequest& request);

// Parses MSBuild's JSON property response; malformed or incomplete responses return Corrupt.
[[nodiscard]] Result<ManagedProjectProperties> parseManagedProjectProperties(std::string_view json);

// Validates the final evaluated console target against the explicit framework and optional RID.
// Returns its absolute DLL path, or InvalidArgument/Unsupported on mismatch or unsupported output.
// The caller must separately verify the current assembly, PDB and source revision before launch.
[[nodiscard]] Result<std::string> managedConsoleTarget(const ManagedProjectRequest& request,
                                                       const ManagedProjectProperties& properties);

/* Starts a managed build only after validating its request and every explicit project trust root.
 * Empty or relative roots return InvalidArgument; an untrusted root returns Untrusted without
 * launching. The returned BuildSession owns the process tree and cancellation. Launcher, trust
 * store and clock must outlive the call/session as required by their respective contracts. This
 * does not save documents, select a framework, or recheck workspace generation after the build.
 */
[[nodiscard]] Result<std::unique_ptr<BuildSession>>
startManagedProjectBuild(io::IProcessLauncher& launcher, const project::TrustStore& trust,
                         const ManagedProjectRequest& request, const std::vector<std::string>& trustRoots,
                         BuildCallbacks callbacks, IClock& clock = systemClock());

// Trust-gated property query with the same ownership and cancellation contract as the build.
// The caller collects stdout and parses it only after a successful exit.
[[nodiscard]] Result<std::unique_ptr<BuildSession>>
startManagedProjectQuery(io::IProcessLauncher& launcher, const project::TrustStore& trust,
                         const ManagedProjectRequest& request, const std::vector<std::string>& trustRoots,
                         BuildCallbacks callbacks, IClock& clock = systemClock());

} // namespace frost::build
