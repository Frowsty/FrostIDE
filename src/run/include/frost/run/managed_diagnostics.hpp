// SPDX-License-Identifier: Apache-2.0
// Explicit installed .NET diagnostic tools; attaches to an admitted process, never owns the target.
#pragma once
#include "frost/run/native_analysis.hpp"
namespace frost::run {
enum class ManagedDiagnosticKind { Trace, Counters, GcDump };
struct ManagedDiagnosticRequest {
    ManagedDiagnosticKind kind = ManagedDiagnosticKind::Trace;
    std::string tool, expectedVersion, workingDirectory, outputDirectory;
    std::uint32_t processId = 0;
    unsigned durationSeconds = 10;
    bool allowFullGc = false;
    std::vector<std::pair<std::string, std::optional<std::string>>> environment;
    // Mandatory, thread-safe, cheap and repeatable. Retains authenticated PID lifetime/image
    // evidence (coreClrProcessAdmission in app), project/trust/config lease and target ownership.
    // Invoked on this worker and a joined watcher; never touches QObjects or kills target PID.
    AnalysisAdmission admit;
};
struct ManagedDiagnosticReport {
    ManagedDiagnosticKind kind;
    std::uint32_t processId;
    std::string version, log, report;
    AnalysisFileIdentity tool, artifact;
};
// Pure validated argv planning. No shell, download, installation, startup tracing or privileged
// collect-linux. Tools must support the documented duration/profile contract; errors stay visible.
[[nodiscard]] Result<io::ProcessSpec> managedDiagnosticSpec(const ManagedDiagnosticRequest& request);
// Worker-only, blocking and bounded: version/capability checks -> collect -> real report ->
// identity revalidation. Requires a new output directory. Cancellation/revoked admission ends
// only the owned collector tree. Failed/partial files remain for diagnosis, never published.
// Artifact cap256MiB, output32MiB; 1..300s collection plus120s shutdown/report deadline.
[[nodiscard]] Result<ManagedDiagnosticReport> collectManagedDiagnostics(io::IFileSystem& fs,
                                                                        io::IProcessLauncher& launcher,
                                                                        const ManagedDiagnosticRequest& request,
                                                                        std::stop_token stop = {});
} // namespace frost::run
