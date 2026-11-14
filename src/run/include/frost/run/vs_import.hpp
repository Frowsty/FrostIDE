// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/rpc/json.hpp"
#include "frost/run/run_configuration.hpp"
namespace frost::run {
struct RunImportIssue {
    std::string entry;
    std::string reason;
};
struct RunImportReport {
    std::vector<RunConfiguration> configurations;
    std::vector<RunImportIssue> skipped;
};
// Data only. No environment lookup, shell, process, filesystem writes or implicit task execution.
// Supports explicit local native launches and single-process launch tasks. Unsupported execution
// semantics and unknown substitutions refuse the affected entry, never silently degrade it.
[[nodiscard]] Result<RunImportReport> importVisualStudioConfigurations(std::string_view json, std::string_view fileName,
                                                                       std::string_view workspace,
                                                                       std::span<const RunConfiguration> existing = {});
} // namespace frost::run
