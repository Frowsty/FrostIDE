// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/core/error.hpp"
#include "frost/run/test_model.hpp"
#include <span>
namespace frost::run {
// Explicit protocol profiles, not framework guesses based on binary names or source macros.
// Catch2 >=3.0.1 XML listing/run schema 3; Boost.Test 1.85 DOT listing + detailed XML report/log.
enum class FrameworkProfile : std::uint8_t { Catch2Xml3, BoostTest185 };
[[nodiscard]] std::string_view frameworkProfileName(FrameworkProfile profile) noexcept;
struct FrameworkCase {
    std::string name;
    std::optional<TestLocation> location;
    bool disabled = false;
};
[[nodiscard]] Result<std::vector<FrameworkCase>> parseFrameworkList(FrameworkProfile profile, std::string_view text);
// Framework arguments, excluding program. Existing report/list controls are replaced. Selection
// is literal or refused; unsupported wildcard names never widen the requested test set.
[[nodiscard]] Result<std::vector<std::string>> frameworkArguments(FrameworkProfile profile,
    const std::vector<std::string>& command, std::span<const std::string> cases,
    std::string_view report, bool listing);
// Only complete upstream reports produce outcomes. A crash/truncated document is a tool error.
// Selected-case reports must name exactly the requested cases. No exit-code-based assumed pass.
[[nodiscard]] Result<TestResult> parseFrameworkRun(FrameworkProfile profile, std::string_view name,
    std::string_view report, std::string_view log, std::span<const std::string> selected,
    int exitCode, std::string_view output);
}
