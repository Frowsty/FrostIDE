// SPDX-License-Identifier: Apache-2.0
// Validated native shader diagnostics and genuine bounded engine PNG artifacts.
#pragma once
#include "frost/core/error.hpp"
#include "frost/rpc/json.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace frost::engine {
struct ShaderIssue {
    std::string path, message, severity;
    std::uint32_t line = 0, column = 0;
};
struct ShaderInput {
    std::string path, physicalPath, sha256;
    bool missing = false;
};
struct ShaderValidation {
    std::string path, sourceSha256, compiler;
    bool valid = false;
    std::vector<ShaderIssue> issues;
    std::vector<ShaderInput> inputs;
};
struct AssetPreview {
    std::string path, sourceSha256, png;
    std::uint32_t size = 0;
    std::vector<ShaderInput> inputs;
};
// Pure thread-safe readers, O(payload), <=4096 diagnostics/128 receipts and <=1MiB decoded PNG.
// Refuse mismatched source/path, unsafe virtual names, malformed counts/hashes, invalid PNG identity/dimensions.
[[nodiscard]] Result<ShaderValidation> parseShaderValidation(const rpc::Json&, std::string_view path,
                                                             std::string_view source);
[[nodiscard]] Result<AssetPreview> parseAssetPreview(const rpc::Json&, std::string_view path,
                                                     std::string_view sourceSha256);
} // namespace frost::engine
