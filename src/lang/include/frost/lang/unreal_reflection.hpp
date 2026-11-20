// SPDX-License-Identifier: Apache-2.0
// Public reflection macro vocabulary and navigation over actual Clang callback events.
#pragma once
#include "frost/lang/cpp_preprocessor.hpp"
#include <span>
namespace frost::lang {
struct UnrealReflectionMacro {
    std::string name;
    std::string path;
    std::uint64_t offset = 0;
    std::string definitionPath;
    std::uint64_t definitionOffset = 0;
};
// Completion candidates from Epic's public Reflection System docs, not extracted engine headers.
[[nodiscard]] std::span<const std::string_view> unrealReflectionMacroNames() noexcept;
// Filters real, nonnested macro expansions. Does not lex comments/strings, infer reflected classes
// or fabricate Blueprint usages. Caller must validateCppIndexInputs(result.receipt) and admit its
// configuration/document revision before publishing navigation locations.
[[nodiscard]] Result<std::vector<UnrealReflectionMacro>> unrealReflectionMacros(const CppPreprocessorResult& result);
} // namespace frost::lang
