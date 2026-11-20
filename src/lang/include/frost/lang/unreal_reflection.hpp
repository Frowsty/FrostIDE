// SPDX-License-Identifier: Apache-2.0
// Public reflection macro vocabulary and navigation over actual Clang callback events.
#pragma once
#include "frost/lang/completion.hpp"
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
struct UnrealReflectionIssue {
    std::string macro, path, message;
    std::uint64_t line = 0, column = 0;
};
// Syntax-only completion catalog, bounded to 256 KiB. Comments, strings, raw strings, assigned
// values and nested metadata arguments are excluded. No reflected symbols/Blueprint uses inferred.
[[nodiscard]] std::optional<CompletionList> unrealSpecifierCompletions(std::string_view source, std::size_t caret);
// Inspects actual top-level, active Clang macro argument tokens only. Unknown/version-specific
// specifiers are not rejected. Missing token evidence returns Unsupported, never a clean report.
// Caller must admit the receipt as for unrealReflectionMacros before publishing these issues.
[[nodiscard]] Result<std::vector<UnrealReflectionIssue>> inspectUnrealReflection(const CppPreprocessorResult& result);
} // namespace frost::lang
