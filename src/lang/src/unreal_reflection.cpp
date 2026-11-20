// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/unreal_reflection.hpp"
#include "frost/io/path.hpp"
#include <algorithm>
#include <array>
namespace frost::lang {
namespace {
constexpr std::array<std::string_view, 7> names{"UCLASS", "USTRUCT", "UENUM", "UINTERFACE", "UPROPERTY", "UFUNCTION", "GENERATED_BODY"};
}
std::span<const std::string_view> unrealReflectionMacroNames() noexcept { return names; }
Result<std::vector<UnrealReflectionMacro>> unrealReflectionMacros(const CppPreprocessorResult& result) {
    if (!result.macros.isArray() || result.macros.items().size() > 200000)
        return fail(ErrorCode::InvalidArgument, "invalid Unreal reflection macro events");
    std::vector<UnrealReflectionMacro> output;
    for (const auto& row : result.macros.items()) {
        const auto name = row.stringAt("name");
        if (std::ranges::find(names, name) == names.end()) continue;
        const auto nested = row.boolAt("nested");
        if (!nested) return fail(ErrorCode::Corrupt, "reflection macro event has no nested flag");
        if (*nested) continue;
        const auto* at = row.find("at");
        const auto* definition = row.find("definition");
        if (!at || !definition || !at->isObject() || !definition->isObject())
            return fail(ErrorCode::Corrupt, "reflection macro event has no location");
        const auto path = at->stringAt("path"), definitionPath = definition->stringAt("path");
        const auto* offset = at->find("offset");
        const auto* definitionOffset = definition->find("offset");
        if (!io::isAbsolutePath(path) || !io::isAbsolutePath(definitionPath) || !offset || !offset->asUnsigned() || !definitionOffset || !definitionOffset->asUnsigned())
            return fail(ErrorCode::Corrupt, "reflection macro event has invalid location");
        output.push_back({std::string(name), std::string(path), *offset->asUnsigned(), std::string(definitionPath), *definitionOffset->asUnsigned()});
    }
    return output;
}
} // namespace frost::lang
