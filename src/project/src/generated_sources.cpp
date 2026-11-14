// SPDX-License-Identifier: Apache-2.0
// Merge proven insertions from the existing CMake planner before any filesystem effects.
#include "frost/project/generated_sources.hpp"

#include "frost/io/path.hpp"
#include "frost/project/cmake_syntax.hpp"

#include <algorithm>
#include <map>
namespace frost::project {
namespace {
bool samePath(std::string_view a, std::string_view b) {
    auto left = io::normalizePath(a), right = io::normalizePath(b);
    if (!left || !right)
        return false;
#ifdef _WIN32
    const auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return std::ranges::equal(*left, *right, [&](char x, char y) { return fold(x) == fold(y); });
#else
    return *left == *right;
#endif
}
} // namespace
Result<void> validateTemplateCMakeCache(const CMakeProject& model, std::string_view bytes) {
    if (bytes.size() > 8 * 1024 * 1024 || bytes.find('\0') != bytes.npos)
        return fail(ErrorCode::OutOfRange, "CMake cache template-input limit exceeded or contains NUL");
    std::map<std::string, std::string> identities;
    for (std::size_t start = 0; start < bytes.size();) {
        const auto end = bytes.find('\n', start);
        auto line = bytes.substr(start, end == bytes.npos ? end : end - start);
        start = end == bytes.npos ? bytes.size() : end + 1;
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.starts_with("//") || line.starts_with('#') || line.empty())
            continue;
        const auto equals = line.find('=');
        const auto colon = line.rfind(':', equals);
        if (equals == line.npos || colon == line.npos || colon > equals)
            continue;
        auto name = line.substr(0, colon);
        if (name.size() > 1 && name.front() == '"' && name.back() == '"')
            name = name.substr(1, name.size() - 2);
        const auto value = line.substr(equals + 1);
        const bool hook =
            name == "CMAKE_PROJECT_INCLUDE" || name == "CMAKE_PROJECT_INCLUDE_BEFORE" ||
            name == "CMAKE_PROJECT_TOP_LEVEL_INCLUDES" ||
            (name.starts_with("CMAKE_PROJECT_") && (name.ends_with("_INCLUDE") || name.ends_with("_INCLUDE_BEFORE")));
        if (hook && !value.empty())
            return fail(ErrorCode::Unsupported,
                        "Current CMake cache has a project input hook; configure/use an explicit static graph first");
        if (name == "CMAKE_HOME_DIRECTORY" || name == "CMAKE_CACHEFILE_DIR" || name == "CMAKE_GENERATOR")
            if (!identities.emplace(std::string(name), std::string(value)).second)
                return fail(ErrorCode::Corrupt, "Duplicate CMake cache identity");
    }
    const auto home = identities.find("CMAKE_HOME_DIRECTORY"), build = identities.find("CMAKE_CACHEFILE_DIR"),
               generator = identities.find("CMAKE_GENERATOR");
    if (home == identities.end() || build == identities.end() || generator == identities.end())
        return fail(ErrorCode::Busy, "Current CMake cache identity is incomplete");
    if (!samePath(home->second, model.sourceDirectory) || !samePath(build->second, model.buildDirectory) ||
        generator->second != model.generator)
        return fail(ErrorCode::Busy, "CMake cache belongs to another source/build/profile");
    return {};
}
Result<std::vector<CMakeMembershipPlan>> planCMakeGeneratedSources(const CMakeProject& model, std::size_t target,
                                                                   std::span<const std::string> paths,
                                                                   std::span<const CMakeInputSnapshot> inputs) {
    if (target >= model.targets.size() || paths.empty() || paths.size() > 64 || model.targets[target].backtrace.empty())
        return fail(ErrorCode::InvalidArgument,
                    "Select one configured source-owning target and bounded generated files");
    struct Definition {
        std::string before;
        std::map<std::size_t, std::string> additions;
    };
    std::map<std::string, Definition> definitions;
    for (const auto& input : inputs) {
        const auto descriptor =
            std::ranges::find_if(model.inputs, [&](const auto& file) { return samePath(file.path, input.path); });
        if (descriptor != model.inputs.end() && descriptor->generated)
            continue; // Existing planner validates known compiler-input policy separately.
        auto commands = scanCMakeSyntax(input.contents);
        if (!commands)
            return std::unexpected(commands.error());
        for (const auto& command : *commands)
            if (command.name == "add_library" || command.name == "add_executable" || command.name == "target_sources") {
                if (command.arguments.empty() || !command.arguments.front().isLiteral ||
                    !model.findTarget(command.arguments.front().literal))
                    return fail(ErrorCode::Busy, "Current CMake target set differs from the configured model");
                for (std::size_t i = 1; i < command.arguments.size(); ++i) {
                    const auto& argument = command.arguments[i];
                    if (!argument.isLiteral)
                        return fail(ErrorCode::Unsupported,
                                    "Dynamic current CMake membership cannot admit generated files");
                    auto resolved = io::isAbsolutePath(argument.literal)
                                        ? io::normalizePath(argument.literal)
                                        : io::joinPath(io::parentPath(input.path), argument.literal);
                    if (!resolved)
                        return std::unexpected(resolved.error());
                    for (const auto& path : paths)
                        if (samePath(*resolved, path))
                            return fail(ErrorCode::AlreadyExists,
                                        "A current CMake declaration already names a generated destination");
                }
            }
        for (const auto& command : *commands)
            if (command.name == "if" || command.name == "elseif") {
                for (const auto& argument : command.arguments)
                    if (!argument.isLiteral || argument.literal == "EXISTS" || argument.literal == "IS_DIRECTORY" ||
                        argument.literal == "IS_SYMLINK" || argument.literal == "IS_READABLE" ||
                        argument.literal == "IS_WRITABLE")
                        return fail(
                            ErrorCode::Unsupported,
                            "File-dependent CMake conditions require an explicit source declaration before templating");
            }
    }
    const auto& origin = model.targets[target].backtrace.front().file;
    // The existing planner accepts only literal, unscoped commands throughout the input graph.
    // That proof is independent of Configuration; do not infer conditional membership.
    auto staticModel = model;
    staticModel.multiConfig = false;
    staticModel.configurations = {model.configuration};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        for (const auto& existing : model.targets)
            for (const auto& source : existing.sources)
                if (samePath(source.path, paths[i]))
                    return fail(ErrorCode::AlreadyExists, "Generated file is already declared in a configured target");
        for (std::size_t j = 0; j < i; ++j)
            if (samePath(paths[j], paths[i]))
                return fail(ErrorCode::InvalidArgument, "Duplicate generated source path");
        auto change = planCMakeMembership(staticModel, target, CMakeMembershipOperation::Add, paths[i], origin, inputs);
        if (!change)
            return std::unexpected(change.error());
        std::size_t prefix = 0;
        while (prefix < change->original.size() && prefix < change->replacement.size() &&
               change->original[prefix] == change->replacement[prefix])
            ++prefix;
        const auto added = change->replacement.size() - std::min(change->original.size(), change->replacement.size());
        if (!added || change->replacement.size() < change->original.size() ||
            change->replacement.substr(prefix + added) != change->original.substr(prefix))
            return fail(ErrorCode::Internal, "Generated membership must be a proven insertion");
        auto [entry, fresh] = definitions.try_emplace(change->definitionPath, Definition{change->original, {}});
        if (!fresh && entry->second.before != change->original)
            return fail(ErrorCode::Busy, "Membership input snapshots disagree");
        entry->second.additions[prefix] += change->replacement.substr(prefix, added);
    }
    std::vector<CMakeMembershipPlan> result;
    for (auto& [path, definition] : definitions) {
        auto after = definition.before;
        for (auto it = definition.additions.rbegin(); it != definition.additions.rend(); ++it)
            after.insert(it->first, it->second);
        result.push_back({path, std::move(definition.before), std::move(after)});
    }
    return result;
}
} // namespace frost::project
