// SPDX-License-Identifier: Apache-2.0
// Strict reader for actual engine-owned entity/component merge reports and source correlation.
#pragma once
#include "frost/engine/engine_cli.hpp"

#include <optional>
#include <string>
#include <vector>
namespace frost::engine {
struct SceneMergeImages {
    std::string path, base, ours, theirs; // Actual Git stage images, losslessly decoded UTF8.
    bool baseExists = true;
};
struct SceneMergeChoice {
    std::string path, side;
};
struct SceneMergeRequest {
    SceneMergeImages images;
    std::vector<SceneMergeChoice> choices;
    std::optional<std::string> candidate;
};
struct SceneMergeConflict {
    std::string path, kind, choice;
    std::optional<rpc::Json> base, ours, theirs; // Missing differs from a present JSON null.
};
struct SceneMergeIssue {
    std::string phase, severity, location, message;
};
struct SceneMergeReport {
    std::string result;
    std::vector<SceneMergeConflict> conflicts;
    std::vector<SceneMergeIssue> issues, inputIssues;
    std::uint64_t unresolved = 0;
    bool valid = false, clean = false, edited = false;
};
// Pure/thread-safe bounded request construction and actual reply reader. Every source hash,
// explicit ancestor absence, manual candidate hash, choice and validity counter must agree.
[[nodiscard]] Result<rpc::Json> sceneMergeParams(const SceneMergeRequest&);
[[nodiscard]] Result<SceneMergeReport> parseSceneMergeReport(const rpc::Json&, const SceneMergeRequest&);
} // namespace frost::engine
