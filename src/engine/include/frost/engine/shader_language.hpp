// SPDX-License-Identifier: Apache-2.0
// Projections of authenticated actual engine semantic queries into ordinary LSP editor replies.
#pragma once
#include "frost/engine/shader_preview.hpp"

#include <span>
namespace frost::engine {
struct ShaderLanguageSource {
    std::string path, physicalPath, bytes;
};
struct ShaderLanguageResult {
    rpc::Json response;
    std::vector<ShaderIssue> diagnostics;
};
// Pure, thread-safe, O(reply + captured sources). UTF8 positions, <=1024 completion items/8192 outline rows.
// Validates exact source/hash/method identities and all native byte ranges before any text edit/navigation is offered.
[[nodiscard]] Result<ShaderLanguageResult> projectShaderLanguage(const rpc::Json&, std::string_view method,
                                                                 std::string_view query,
                                                                 std::span<const ShaderLanguageSource>);
} // namespace frost::engine
