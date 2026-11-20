// SPDX-License-Identifier: Apache-2.0
// Analyzes Unreal reflection macros for navigation, completion, and inspections.
#include "frost/lang/unreal_reflection.hpp"

#include "frost/io/path.hpp"

#include <algorithm>
#include <array>
#include <set>
namespace frost::lang {
namespace {
constexpr std::array<std::string_view, 7> names{"UCLASS",    "USTRUCT",   "UENUM",         "UINTERFACE",
                                                "UPROPERTY", "UFUNCTION", "GENERATED_BODY"};
}
std::span<const std::string_view> unrealReflectionMacroNames() noexcept { return names; }
Result<std::vector<UnrealReflectionMacro>> unrealReflectionMacros(const CppPreprocessorResult& result) {
    if (!result.macros.isArray() || result.macros.items().size() > 200000)
        return fail(ErrorCode::InvalidArgument, "invalid Unreal reflection macro events");
    std::vector<UnrealReflectionMacro> output;
    for (const auto& row : result.macros.items()) {
        const auto name = row.stringAt("name");
        if (std::ranges::find(names, name) == names.end())
            continue;
        const auto nested = row.boolAt("nested");
        if (!nested)
            return fail(ErrorCode::Corrupt, "reflection macro event has no nested flag");
        if (*nested)
            continue;
        const auto* at = row.find("at");
        const auto* definition = row.find("definition");
        if (!at || !definition || !at->isObject() || !definition->isObject())
            return fail(ErrorCode::Corrupt, "reflection macro event has no location");
        const auto path = at->stringAt("path"), definitionPath = definition->stringAt("path");
        const auto* offset = at->find("offset");
        const auto* definitionOffset = definition->find("offset");
        if (!io::isAbsolutePath(path) || !io::isAbsolutePath(definitionPath) || !offset || !offset->asUnsigned() ||
            !definitionOffset || !definitionOffset->asUnsigned())
            return fail(ErrorCode::Corrupt, "reflection macro event has invalid location");
        output.push_back({std::string(name), std::string(path), *offset->asUnsigned(), std::string(definitionPath),
                          *definitionOffset->asUnsigned()});
    }
    return output;
}

namespace {
std::span<const std::string_view> specifiers(std::string_view macro) {
    static constexpr std::array<std::string_view, 12> classes{
        "Abstract",           "Blueprintable", "NotBlueprintable", "BlueprintType", "Config",    "DefaultConfig",
        "DefaultToInstanced", "EditInlineNew", "MinimalAPI",       "NotPlaceable",  "Transient", "meta"};
    static constexpr std::array<std::string_view, 18> properties{"BlueprintReadOnly",
                                                                 "BlueprintReadWrite",
                                                                 "Category",
                                                                 "EditAnywhere",
                                                                 "EditDefaultsOnly",
                                                                 "EditInstanceOnly",
                                                                 "VisibleAnywhere",
                                                                 "VisibleDefaultsOnly",
                                                                 "VisibleInstanceOnly",
                                                                 "Config",
                                                                 "Transient",
                                                                 "SaveGame",
                                                                 "Replicated",
                                                                 "ReplicatedUsing",
                                                                 "Instanced",
                                                                 "AdvancedDisplay",
                                                                 "AssetRegistrySearchable",
                                                                 "meta"};
    static constexpr std::array<std::string_view, 15> functions{"BlueprintCallable",
                                                                "BlueprintPure",
                                                                "BlueprintImplementableEvent",
                                                                "BlueprintNativeEvent",
                                                                "BlueprintAuthorityOnly",
                                                                "BlueprintCosmetic",
                                                                "CallInEditor",
                                                                "Category",
                                                                "Exec",
                                                                "Server",
                                                                "Client",
                                                                "NetMulticast",
                                                                "Reliable",
                                                                "Unreliable",
                                                                "meta"};
    static constexpr std::array<std::string_view, 4> structs{"BlueprintType", "Atomic", "NoExport", "meta"};
    static constexpr std::array<std::string_view, 2> enums{"BlueprintType", "meta"};
    static constexpr std::array<std::string_view, 4> interfaces{"Blueprintable", "NotBlueprintable", "MinimalAPI",
                                                                "meta"};
    if (macro == "UCLASS")
        return classes;
    if (macro == "UPROPERTY")
        return properties;
    if (macro == "UFUNCTION")
        return functions;
    if (macro == "USTRUCT")
        return structs;
    if (macro == "UENUM")
        return enums;
    if (macro == "UINTERFACE")
        return interfaces;
    return {};
}
bool identifierByte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
} // namespace
std::optional<CompletionList> unrealSpecifierCompletions(std::string_view source, std::size_t caret) {
    if (caret > source.size() || source.size() > 256 * 1024)
        return {};
    struct Scope {
        std::string name;
        bool assigned = false;
        char delimiter = '(';
    };
    std::vector<Scope> scopes;
    std::string previous;
    for (std::size_t i = 0; i < caret;) {
        const char c = source[i];
        if (c == '/' && i + 1 < caret && source[i + 1] == '/') {
            auto end = source.find('\n', i + 2);
            while (end != std::string_view::npos && end < caret) {
                const auto before = (end > 0 && source[end - 1] == '\r') ? end - 1 : end;
                if (before == 0 || source[before - 1] != '\\')
                    break;
                end = source.find('\n', end + 1);
            }
            if (end == std::string_view::npos || end >= caret)
                return {};
            i = end + 1;
            previous.clear();
            continue;
        }
        if (c == '/' && i + 1 < caret && source[i + 1] == '*') {
            auto end = source.find("*/", i + 2);
            if (end == std::string_view::npos || end + 2 > caret)
                return {};
            i = end + 2;
            previous.clear();
            continue;
        }
        std::size_t rawPrefix = 0;
        for (const std::string_view prefix : {"R\"", "u8R\"", "uR\"", "UR\"", "LR\""})
            if (source.substr(i).starts_with(prefix)) {
                rawPrefix = prefix.size();
                break;
            }
        if (rawPrefix) {
            const auto open = source.find('(', i + rawPrefix);
            if (open == std::string_view::npos || open - i - rawPrefix > 16)
                return {};
            const std::string closing = ")" + std::string(source.substr(i + rawPrefix, open - i - rawPrefix)) + "\"";
            const auto end = source.find(closing, open + 1);
            if (end == std::string_view::npos || end + closing.size() > caret)
                return {};
            i = end + closing.size();
            previous.clear();
            continue;
        }
        if (c == '\"' || c == '\'') {
            const char quote = c;
            bool closed = false;
            ++i;
            while (i < caret) {
                if (source[i] == '\\') {
                    i += std::min<std::size_t>(2, caret - i);
                    continue;
                }
                if (source[i++] == quote) {
                    closed = true;
                    break;
                }
            }
            if (!closed)
                return {};
            previous.clear();
            continue;
        }
        if (identifierByte(c)) {
            const auto begin = i;
            while (i < caret && identifierByte(source[i]))
                ++i;
            previous = std::string(source.substr(begin, i - begin));
            continue;
        }
        if (c == '(' || c == '[' || c == '{') {
            scopes.push_back({c == '(' ? previous : std::string{}, false, c});
            previous.clear();
        } else if (c == ')' || c == ']' || c == '}') {
            const char opening = c == ')' ? '(' : (c == ']' ? '[' : '{');
            if (scopes.empty() || scopes.back().delimiter != opening)
                return {};
            scopes.pop_back();
            previous.clear();
        } else if (c == '=' && !scopes.empty()) {
            scopes.back().assigned = true;
            previous.clear();
        } else if (c == ',' && !scopes.empty()) {
            scopes.back().assigned = false;
            previous.clear();
        } else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            previous.clear();
        }
        ++i;
    }
    if (scopes.empty() || scopes.back().assigned)
        return {};
    const auto vocabulary = specifiers(scopes.back().name);
    if (vocabulary.empty())
        return {};
    CompletionList list;
    for (const auto word : vocabulary) {
        CompletionItem item;
        item.label = std::string(word);
        item.insertText = item.label;
        item.kind = CompletionItemKind::Keyword;
        item.detail = "Unreal public reflection specifier";
        item.resolved = true;
        if (word == "Category") {
            item.insertText += "=\"${1}\"";
            item.snippet = true;
        }
        if ((word == "Config" && scopes.back().name == "UCLASS") || word == "ReplicatedUsing") {
            item.insertText += "=${1}";
            item.snippet = true;
        }
        if (word == "meta") {
            item.insertText = "meta=(${1})";
            item.snippet = true;
        }
        list.items.push_back(std::move(item));
    }
    return list;
}
Result<std::vector<UnrealReflectionIssue>> inspectUnrealReflection(const CppPreprocessorResult& result) {
    if (!result.macros.isArray() || result.macros.items().size() > 200000)
        return fail(ErrorCode::Corrupt, "invalid reflection macro events");
    std::vector<UnrealReflectionIssue> issues;
    for (const auto& row : result.macros.items()) {
        const auto macro = row.stringAt("name");
        if (specifiers(macro).empty())
            continue;
        const auto nested = row.boolAt("nested");
        if (!nested)
            return fail(ErrorCode::Corrupt, "reflection event missing nested flag");
        if (*nested)
            continue;
        const auto* args = row.find("argumentTokens");
        const auto* at = row.find("at");
        if (!args || !args->isArray())
            return fail(ErrorCode::Unsupported, "selected semantic producer has no reflection argument token evidence");
        if (!at || !io::isAbsolutePath(at->stringAt("path")) || !at->find("line") || !at->find("line")->asUnsigned() ||
            !at->find("column") || !at->find("column")->asUnsigned())
            return fail(ErrorCode::Corrupt, "invalid reflection event location");
        std::set<std::string> words;
        for (const auto& argument : args->items()) {
            if (!argument.isArray() || argument.items().size() > 4096)
                return fail(ErrorCode::Corrupt, "invalid reflection argument tokens");
            int depth = 0;
            bool first = true;
            for (const auto& token : argument.items()) {
                if (!token.asString())
                    return fail(ErrorCode::Corrupt, "invalid reflection argument token");
                const auto& text = *token.asString();
                if (text == "(") {
                    ++depth;
                    continue;
                }
                if (text == ")") {
                    if (depth > 0)
                        --depth;
                    continue;
                }
                if (depth == 0 && text == ",") {
                    first = true;
                    continue;
                }
                if (depth == 0 && first) {
                    if (!text.empty() && std::ranges::all_of(text, identifierByte))
                        words.insert(text);
                    first = false;
                }
            }
        }
        auto conflict = [&](std::string_view a, std::string_view b) {
            if (words.contains(std::string(a)) && words.contains(std::string(b)))
                issues.push_back({std::string(macro), std::string(at->stringAt("path")),
                                  std::string(a) + " and " + std::string(b) + " cannot be combined",
                                  *at->find("line")->asUnsigned(), *at->find("column")->asUnsigned()});
        };
        if (macro == "UPROPERTY") {
            conflict("BlueprintReadOnly", "BlueprintReadWrite");
            for (const auto edit : {"EditAnywhere", "EditDefaultsOnly", "EditInstanceOnly"})
                for (const auto visible : {"VisibleAnywhere", "VisibleDefaultsOnly", "VisibleInstanceOnly"})
                    conflict(edit, visible);
        }
        if (macro == "UCLASS" || macro == "UINTERFACE")
            conflict("Blueprintable", "NotBlueprintable");
    }
    return issues;
}
} // namespace frost::lang
