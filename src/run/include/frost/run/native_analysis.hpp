// SPDX-License-Identifier: Apache-2.0
// Native coverage/profiling value models and bounded owned-process collection (RUN-12/13).
#pragma once
#include "frost/io/file_system.hpp"
#include "frost/run/run_manager.hpp"

#include <map>
namespace frost::run {
// Evidence is captured before collection and revalidated before publication. Hashes describe
// observed inputs, not a claim that an arbitrary external build used these exact source bytes.
struct AnalysisFileIdentity {
    std::string path, physicalPath, sha256;
    io::FileStatus status;
    friend bool operator==(const AnalysisFileIdentity&, const AnalysisFileIdentity&) = default;
};
[[nodiscard]] Result<AnalysisFileIdentity> captureAnalysisIdentity(io::IFileSystem& fs, std::string_view path,
                                                                   std::size_t cap = 256 * 1024 * 1024);
[[nodiscard]] Result<void> validateAnalysisIdentity(io::IFileSystem& fs, const AnalysisFileIdentity& identity);
using AnalysisAdmission = std::function<Result<void>()>;
struct AnalysisCommandResult {
    int exitCode = 0;
    std::string output, error;
    io::ProcessId pid = 0;
};
// Worker-only. Uses RunSession ownership, caps retained output, ends/reaps the owned tree on stop,
// overflow or deadline. admission runs immediately before launch; never invokes shell syntax.
// liveAdmission is a cheap thread-safe revocation check polled every20ms while the tree runs.
[[nodiscard]] Result<AnalysisCommandResult>
runAnalysisCommand(io::IProcessLauncher& launcher, io::ProcessSpec spec, std::stop_token stop,
                   AnalysisAdmission admission = {}, std::optional<IClock::Duration> timeout = std::chrono::minutes(5),
                   std::size_t outputCap = 32 * 1024 * 1024, std::function<void(io::ProcessId)> started = {},
                   AnalysisAdmission liveAdmission = {});
struct CoverageLine {
    std::uint32_t line = 0;
    std::uint64_t hits = 0;
    friend bool operator==(const CoverageLine&, const CoverageLine&) = default;
};
struct CoverageBranch {
    std::uint32_t line = 0, block = 0, branch = 0;
    std::optional<std::uint64_t> hits;
};
struct CoverageFile {
    std::string path;
    std::vector<CoverageLine> lines;
    std::vector<CoverageBranch> branches;
    std::uint64_t found = 0, covered = 0, branchesFound = 0, branchesCovered = 0;
    std::optional<AnalysisFileIdentity> identity;
    std::string sourceUnavailable;
    bool aggregateDiffersFromDetails = false;
};
struct CoverageReport {
    AnalysisFileIdentity binary;
    std::vector<CoverageFile> files;
    std::string lcov, profileDataPath;
    std::optional<int> programExitCode;
};
// Exact LLVM banner token matching; version prefixes such as23.1.40 never match23.1.4.
[[nodiscard]] Result<void> validateAnalysisLlvmVersion(std::string_view banner, std::string_view expected);
// llvm-cov can report file aggregates that differ from the per-source line and branch projections.
enum class CoverageSummaryMode { RequireRowTotals, LlvmFileAggregates };
[[nodiscard]] Result<std::vector<CoverageFile>>
parseLlvmCoverage(std::string_view lcov, std::string_view compilationDirectory,
                  CoverageSummaryMode summaryMode = CoverageSummaryMode::RequireRowTotals);
struct CoverageRequest {
    std::string llvmCov, llvmProfdata, expectedLlvmVersion = "23.1.4";
    LaunchPlan plan;
    std::string outputDirectory; // New private directory only; existing paths are never overwritten.
    std::string compilationDirectory;
    bool execute = true;
    std::vector<std::string> rawProfiles; // Imported profiles when execute=false; never removed.
};
// Worker-only vertical: baseline mappings -> input identities -> optional instrumented run ->
// profdata merge -> actual llvm-cov export -> revalidation. No build flags are inferred/modified.
// Missing instrumentation/tools, warnings, changed identities or cancellation return explicit errors.
[[nodiscard]] Result<CoverageReport> collectNativeCoverage(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                           const CoverageRequest& request, std::stop_token stop = {},
                                                           AnalysisAdmission admission = {});
// Profiling preserves the installed tool's genuine report, including unresolved symbols. It does
// not manufacture a normalized sample table from undocumented/version-dependent columns.
enum class NativeProfileBackend { Perf, WindowsEtw };
struct NativeProfileRequest {
    NativeProfileBackend backend = NativeProfileBackend::Perf;
    LaunchPlan plan;
    std::string outputDirectory;
    std::string perf, wpr, xperf;
    // Unique FrostIDE-<UUID> instance. Only this owned WPR instance is ever stopped/cancelled.
    std::string sessionName;
    bool systemWideEtwConsent = false;
    IClock::Duration duration = std::chrono::seconds(10); // Positive, at most5minutes; owned tree ends at interval.
};
struct NativeProfileReport {
    NativeProfileBackend backend = NativeProfileBackend::Perf;
    AnalysisFileIdentity binary, trace;
    std::string toolReport, diagnostics;
    std::optional<int> programExitCode;
    std::optional<io::ProcessId> programPid; // Windows owned program PID; perf report retains real PIDs.
};
// Worker only. perf owns its command's tree. WPR owns a uniquely named ETW CPU instance;
// retiring that OS instance is mandatory cleanup even after cancellation/trust revocation.
// admission gates every ordinary process; cleanup executes only the captured installed WPR.
// Windows reports system-wide samples explicitly. Symbol paths are local and no server is used.
[[nodiscard]] Result<NativeProfileReport> collectNativeProfile(io::IFileSystem& fs, io::IProcessLauncher& launcher,
                                                               const NativeProfileRequest& request,
                                                               std::stop_token stop = {},
                                                               AnalysisAdmission admission = {});
} // namespace frost::run
