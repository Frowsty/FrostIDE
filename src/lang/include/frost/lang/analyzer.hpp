// SPDX-License-Identifier: Apache-2.0
// Native analyzer orchestration over existing inspection profiles, exact compiler receipts and SARIF.
#pragma once
#include "frost/lang/cpp_rename_plan.hpp"
#include "frost/lang/inspection.hpp"
namespace frost::lang {
struct AnalyzerRuleInfo {
    std::string id, title, category, level, applicability, settingKey;
};
struct AnalyzerInput {
    std::string path, bytes, physicalPath;
    bool overlay = false;
};
struct AnalyzerFix {
    std::string ruleId, path, replacement;
    std::uint64_t offset = 0, length = 0;
};
struct AnalyzerReport {
    InspectionReport inspection;
    std::vector<AnalyzerRuleInfo> rules;
    std::vector<AnalyzerInput> inputs;
    std::vector<std::string> missing, directories, limitations;
    std::vector<AnalyzerFix> fixes; // Only exact fixes agreed by every analyzed configuration reading the target.
    std::size_t completedConfigurations = 0, failedConfigurations = 0;
};
struct AnalyzerRequest {
    std::string executable, llvmRoot, projectRoot, compilationDatabasePath;
    InspectionScope scope;
    InspectionProfile profile;
    std::vector<CppRenameOverlay> overlays;
    sched::ResourceGovernor* governor = nullptr;
    std::chrono::milliseconds timeout{120000};
    unsigned maxWorkers = 4; // Further capped by min(4,cores/4), governor jobs and available RAM/commit.
    std::stop_token stop;
};
// Pure settings decoder; bad severities/empty patterns fail, rather than silently enabling rules.
[[nodiscard]] Result<InspectionProfile> analyzerProfile(std::span<const std::string> entries);
// Worker-thread API. Uses actual native reports; validates input/config identities and current bytes.
// Holds one existing inspection lease around its bounded process pool. All children join before return.
[[nodiscard]] Result<AnalyzerReport> runFrostAnalyzer(io::IProcessLauncher&, io::IFileSystem&, const AnalyzerRequest&);
// Owner calls again at UI delivery/apply. Overlay text ownership remains with request/editor caller.
[[nodiscard]] Result<void> validateAnalyzerInputs(io::IFileSystem&, const AnalyzerReport&);
// SARIF import accepts standard file URI physical locations, exact external rule IDs and severities.
// Unsupported logical-only locations are explicit errors; no guessed source paths/rows.
[[nodiscard]] Result<std::vector<InspectionReport>> importInspectionSarif(const Json&);
} // namespace frost::lang
