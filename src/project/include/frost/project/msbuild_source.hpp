// SPDX-License-Identifier: Apache-2.0
// Read-only MSBuild XML declarations. Conditions and property expressions are never evaluated.
#pragma once
#include "frost/core/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
namespace frost::project {
struct MSBuildDeclaration {
    std::string name, value, condition;
    std::size_t offset = 0, line = 0;
};
struct MSBuildImport {
    std::string expression;
    std::size_t offset = 0;
    bool literal = false;
};
struct MSBuildSource {
    std::vector<MSBuildDeclaration> properties;
    std::vector<MSBuildDeclaration> targets;
    std::vector<MSBuildImport> imports;
};
// Pure bounded XML parser. Source declarations are not evaluated values; no filesystem or execution.
[[nodiscard]] Result<MSBuildSource> parseMSBuildSource(std::string_view source, std::string_view path);
[[nodiscard]] bool isMSBuildSource(std::string_view path);
} // namespace frost::project
