// SPDX-License-Identifier: Apache-2.0
// Provenance-bound, byte-preserving CMake membership planning; no filesystem writes.
#include "frost/project/cmake_membership.hpp"
#include "frost/project/generated_sources.hpp"

#include "frost/io/path.hpp"
#include "frost/project/cmake_syntax.hpp"

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace frost::project {
namespace {

bool equalPath(std::string_view a, std::string_view b) {
    auto left = io::normalizePath(a);
    auto right = io::normalizePath(b);
    if (!left || !right || left->size() != right->size())
        return false;
#ifdef _WIN32
    auto fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return std::ranges::equal(*left, *right, [&](char x, char y) { return fold(x) == fold(y); });
#else
    return *left == *right;
#endif
}

// '..' may cross a symlink. The planner does not have file identity evidence for that case.
bool safeLiteral(std::string_view text) {
    if (text.empty() || text.find_first_of("$;\\\"\r\n") != std::string_view::npos ||
        text.find('\0') != std::string_view::npos)
        return false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('/', start);
        const auto component = text.substr(start, end == std::string_view::npos ? end : end - start);
        if (component == "..")
            return false;
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return true;
}

bool sourceMatches(std::string_view spelling, std::string_view source, std::string_view base, bool relativeAllowed) {
    if (!safeLiteral(spelling))
        return false;
    if (io::isAbsolutePath(spelling))
        return equalPath(spelling, source);
    if (!relativeAllowed)
        return false;
    auto joined = io::joinPath(base, spelling);
    return joined && equalPath(*joined, source);
}

bool knownGeneratedInput(const CMakeProject& model, const CMakeInputFile& input) {
    if (!input.generated || input.external || model.cmakeVersion.empty())
        return false;
    auto directory = io::joinPath(model.buildDirectory, "CMakeFiles/" + model.cmakeVersion);
    if (!directory)
        return false;
    for (std::string_view name :
         {"CMakeSystem.cmake", "CMakeCCompiler.cmake", "CMakeCXXCompiler.cmake", "CMakeRCCompiler.cmake"}) {
        auto path = io::joinPath(*directory, name);
        if (path && equalPath(*path, input.path))
            return true;
    }
    return false;
}

bool safeGeneratedCommands(const std::vector<CMakeSyntaxCommand>& commands) {
    for (const auto& command : commands) {
        if (command.name == "if" || command.name == "elseif" || command.name == "else" || command.name == "endif" ||
            command.name == "foreach" || command.name == "endforeach")
            continue;
        if (command.name == "list") {
            if (command.arguments.size() < 2 || !command.arguments[0].isLiteral || !command.arguments[1].isLiteral ||
                command.arguments[0].literal != "REMOVE_ITEM" ||
                command.arguments[1].literal != "CMAKE_CXX_SOURCE_FILE_EXTENSIONS")
                return false;
            continue;
        }
        if (command.name != "set" || command.arguments.empty() || !command.arguments[0].isLiteral)
            return false;
        const auto& name = command.arguments[0].literal;
        const bool known =
            name.starts_with("CMAKE_C_") || name.starts_with("CMAKE_CXX_") || name.starts_with("CMAKE_C90_") ||
            name.starts_with("CMAKE_C99_") || name.starts_with("CMAKE_C11_") || name.starts_with("CMAKE_C17_") ||
            name.starts_with("CMAKE_C23_") || name.starts_with("CMAKE_CXX98_") || name.starts_with("CMAKE_CXX11_") ||
            name.starts_with("CMAKE_CXX14_") || name.starts_with("CMAKE_CXX17_") || name.starts_with("CMAKE_CXX20_") ||
            name.starts_with("CMAKE_CXX23_") || name.starts_with("CMAKE_CXX26_") ||
            name.starts_with("CMAKE_HOST_SYSTEM_") || name.starts_with("CMAKE_SYSTEM_") || name == "CMAKE_SYSTEM" ||
            name == "CMAKE_HOST_SYSTEM" || name == "CMAKE_CROSSCOMPILING" || name == "CMAKE_SIZEOF_VOID_P" ||
            name == "CMAKE_INTERNAL_PLATFORM_ABI" || name == "CMAKE_LIBRARY_ARCHITECTURE" ||
            name == "CMAKE_CL_SHOWINCLUDES_PREFIX" || name == "CMAKE_LINKER_PUSHPOP_STATE_SUPPORTED" ||
            name == "MSVC_C_ARCHITECTURE_ID" || name == "MSVC_CXX_ARCHITECTURE_ID" || name == "CMAKE_RC_COMPILER" ||
            name == "CMAKE_RC_COMPILER_ARG1" || name == "CMAKE_RC_COMPILER_LOADED" ||
            name == "CMAKE_RC_SOURCE_FILE_EXTENSIONS" || name == "CMAKE_RC_OUTPUT_EXTENSION" ||
            name == "CMAKE_RC_COMPILER_ENV_VAR" || name == "CMAKE_AR" || name == "CMAKE_LINKER" ||
            name == "CMAKE_LINKER_LINK" || name == "CMAKE_LINKER_LLD" || name == "CMAKE_MT" || name == "CMAKE_RANLIB" ||
            name == "CMAKE_TAPI" || name == "CMAKE_COMPILER_IS_GNUCC" || name == "CMAKE_COMPILER_IS_GNUCXX";
        if (!known)
            return false;
    }
    return true;
}

bool languageStandardSetting(const CMakeSyntaxCommand& command) {
    if(command.name!="set" || command.arguments.size()!=2 || !command.arguments[0].isLiteral || !command.arguments[1].isLiteral) return false;
    const auto& name=command.arguments[0].literal;
    const auto& value=command.arguments[1].literal;
    if(name=="CMAKE_CXX_STANDARD" || name=="CMAKE_C_STANDARD")
        return value=="98" || value=="11" || value=="14" || value=="17" || value=="20" || value=="23" || value=="26" || value=="90" || value=="99";
    if(name=="CMAKE_CXX_STANDARD_REQUIRED" || name=="CMAKE_CXX_EXTENSIONS" || name=="CMAKE_C_STANDARD_REQUIRED" || name=="CMAKE_C_EXTENSIONS")
        return value=="ON" || value=="OFF" || value=="TRUE" || value=="FALSE" || value=="1" || value=="0";
    return false;
}

Result<void> validateInvocationGraph(const CMakeProject& model, std::span<const CMakeInputSnapshot> inputs) {
    for (const auto& entry : model.cache) {
        const auto& name = entry.name;
        const bool hook =
            name == "CMAKE_PROJECT_INCLUDE" || name == "CMAKE_PROJECT_INCLUDE_BEFORE" ||
            name == "CMAKE_PROJECT_TOP_LEVEL_INCLUDES" ||
            (name.starts_with("CMAKE_PROJECT_") && (name.ends_with("_INCLUDE") || name.ends_with("_INCLUDE_BEFORE")));
        if (hook && !entry.value.empty())
            return fail(ErrorCode::Unsupported, "CMake project hook obscures invocation context");
    }
    auto root = io::joinPath(model.sourceDirectory, "CMakeLists.txt");
    if (!root)
        return fail(ErrorCode::Unsupported, "No configured CMake root");
    std::vector<const CMakeInputSnapshot*> lists;
    for (const auto& input : inputs) {
        const auto entry = std::ranges::find_if(
            model.inputs, [&](const CMakeInputFile& item) { return !item.cmake && equalPath(item.path, input.path); });
        if (entry == model.inputs.end())
            return fail(ErrorCode::Unsupported, "Unknown configured input");
        if (entry->generated)
            continue;
        if (io::fileName(input.path) != "CMakeLists.txt")
            return fail(ErrorCode::Unsupported, "Included CMake scripts require invocation evidence");
        lists.push_back(&input);
    }
    std::size_t rootIndex = lists.size();
    for (std::size_t i = 0; i < lists.size(); ++i)
        if (equalPath(lists[i]->path, *root))
            rootIndex = i;
    if (rootIndex == lists.size())
        return fail(ErrorCode::Unsupported, "Root CMakeLists snapshot is missing");
    std::vector<std::optional<std::size_t>> parents(lists.size());
    for (std::size_t i = 0; i < lists.size(); ++i) {
        auto commands = scanCMakeSyntax(lists[i]->contents);
        if (!commands)
            return std::unexpected(std::move(commands.error()));
        for (const auto& command : *commands) {
            if (command.name != "add_subdirectory")
                continue;
            if (command.scopeDepth || command.nestedArguments || command.arguments.empty())
                return fail(ErrorCode::Unsupported, "Conditional or dynamic subdirectory invocation");
            for (const auto& arg : command.arguments)
                if (!arg.isLiteral)
                    return fail(ErrorCode::Unsupported, "Dynamic subdirectory invocation");
            const auto& spelling = command.arguments.front().literal;
            if (!safeLiteral(spelling))
                return fail(ErrorCode::Unsupported, "Unsupported subdirectory path");
            const auto parent = io::parentPath(lists[i]->path);
            auto directory =
                io::isAbsolutePath(spelling) ? io::normalizePath(spelling) : io::joinPath(parent, spelling);
            if (!directory)
                return fail(ErrorCode::Unsupported, "Invalid subdirectory path");
            auto child = io::joinPath(*directory, "CMakeLists.txt");
            if (!child)
                return fail(ErrorCode::Unsupported, "Invalid subdirectory input");
            std::size_t found = lists.size();
            for (std::size_t j = 0; j < lists.size(); ++j)
                if (equalPath(lists[j]->path, *child))
                    found = j;
            if (found == lists.size() || found == rootIndex || parents[found])
                return fail(ErrorCode::Unsupported, "Unproven or repeated subdirectory invocation");
            parents[found] = i;
        }
    }
    for (std::size_t i = 0; i < lists.size(); ++i) {
        auto current = i;
        for (std::size_t hops = 0; current != rootIndex; ++hops) {
            if (hops >= lists.size() || !parents[current])
                return fail(ErrorCode::Unsupported, "Input is not uniquely reachable from CMake root");
            current = *parents[current];
        }
    }
    return {};
}

bool policyAtLeast313(const CMakeSyntaxCommand& command) {
    if (command.scopeDepth != 0 || command.arguments.size() < 2 || !command.arguments[0].isLiteral ||
        command.arguments[0].literal != "VERSION" || !command.arguments[1].isLiteral)
        return false;
    const auto& version = command.arguments[1].literal;
    const auto dot = version.find('.');
    if (dot == std::string::npos)
        return false;
    unsigned major = 0, minor = 0;
    auto parse = [](std::string_view value, unsigned& out) {
        if (value.empty())
            return false;
        for (char c : value) {
            if (c < '0' || c > '9' || out > 999)
                return false;
            out = out * 10 + static_cast<unsigned>(c - '0');
        }
        return true;
    };
    const auto end = version.find('.', dot + 1);
    return parse(std::string_view(version).substr(0, dot), major) &&
           parse(std::string_view(version).substr(dot + 1, end == std::string::npos ? end : end - dot - 1), minor) &&
           (major > 3 || (major == 3 && minor >= 13));
}

struct SourceArguments {
    std::vector<const CMakeSyntaxArgument*> sources;
    std::vector<std::string> roles;
    std::vector<std::string> sourceRoles;
};

Result<SourceArguments> sourceArguments(const CMakeSyntaxCommand& command, std::string_view target) {
    if (command.scopeDepth != 0 || command.nestedArguments || command.arguments.empty() ||
        !command.arguments.front().isLiteral || command.arguments.front().literal != target)
        return fail(ErrorCode::Unsupported, "Target source declaration is dynamic or scoped");
    for (const auto& arg : command.arguments)
        if (!arg.isLiteral)
            return fail(ErrorCode::Unsupported, "Dynamic source argument");
    SourceArguments result;
    std::size_t i = 1;
    if (command.name == "add_library" || command.name == "add_executable") {
        while (i < command.arguments.size()) {
            const auto& value = command.arguments[i].literal;
            if (value == "IMPORTED" || value == "ALIAS" || value == "INTERFACE" || value == "UNKNOWN" ||
                value == "SYMBOLIC" || value == "GLOBAL")
                return fail(ErrorCode::Unsupported, "Unsupported target declaration role");
            const bool option = value == "EXCLUDE_FROM_ALL" ||
                                (command.name == "add_library" &&
                                 (value == "STATIC" || value == "SHARED" || value == "MODULE" || value == "OBJECT")) ||
                                (command.name == "add_executable" && (value == "WIN32" || value == "MACOSX_BUNDLE"));
            if (!option)
                break;
            result.roles.push_back(value);
            ++i;
        }
        for (; i < command.arguments.size(); ++i) {
            result.sources.push_back(&command.arguments[i]);
            result.sourceRoles.emplace_back();
        }
    } else if (command.name == "target_sources") {
        std::string visibility;
        for (; i < command.arguments.size(); ++i) {
            const auto& arg = command.arguments[i];
            if (arg.literal == "PRIVATE" || arg.literal == "PUBLIC") {
                visibility = arg.literal;
                result.roles.push_back(visibility);
                continue;
            }
            if (arg.literal == "INTERFACE" || arg.literal == "FILE_SET")
                return fail(ErrorCode::Unsupported, "Interface and file-set sources need separate handling");
            if (visibility.empty())
                return fail(ErrorCode::Unsupported, "Missing source visibility");
            result.sources.push_back(&arg);
            result.sourceRoles.push_back(visibility);
        }
    } else
        return fail(ErrorCode::Unsupported, "Unsupported source command");
    return result;
}

} // namespace

Result<CMakeMembershipPlan> planCMakeMembership(const CMakeProject& model, std::size_t targetIndex,
                                                CMakeMembershipOperation operation, std::string_view sourcePath,
                                                std::string_view definitionPath,
                                                std::span<const CMakeInputSnapshot> inputs) {
    if (targetIndex >= model.targets.size() || !io::isAbsolutePath(sourcePath) || !io::isAbsolutePath(definitionPath))
        return fail(ErrorCode::InvalidArgument, "Invalid CMake membership request");
    std::string normalizedSource(sourcePath);
    std::replace(normalizedSource.begin(), normalizedSource.end(), '\\', '/');
    if (!safeLiteral(normalizedSource))
        return fail(ErrorCode::InvalidArgument, "Unsupported source path spelling");
    if (!model.inputsAvailable || !model.cacheAvailable)
        return fail(ErrorCode::Unsupported, "Configured input or cache evidence is unavailable");
    if (model.multiConfig || model.configurations.size() > 1)
        return fail(ErrorCode::Unsupported, "Multiple CMake configurations require cross-configuration validation");
    std::vector<std::string_view> projectInputs;
    for (const auto& input : model.inputs) {
        for (const auto& other : model.inputs) {
            if (equalPath(input.path, other.path) &&
                (input.cmake != other.cmake || input.external != other.external || input.generated != other.generated))
                return fail(ErrorCode::Unsupported, "Conflicting configured input metadata");
        }
        if (input.cmake)
            continue;
        if (input.external || (input.generated && !knownGeneratedInput(model, input)))
            return fail(ErrorCode::Unsupported, "External or unsupported generated CMake input");
        if (std::ranges::any_of(projectInputs, [&](std::string_view path) { return equalPath(path, input.path); })) {
            const auto first = std::ranges::find_if(
                model.inputs, [&](const CMakeInputFile& entry) { return equalPath(entry.path, input.path); });
            if (first == model.inputs.end() || first->cmake != input.cmake || first->external != input.external ||
                first->generated != input.generated)
                return fail(ErrorCode::Unsupported, "Conflicting configured input metadata");
            continue;
        }
        projectInputs.push_back(input.path);
    }
    if (projectInputs.size() != inputs.size())
        return fail(ErrorCode::Unsupported, "Incomplete configured input snapshots");
    const CMakeInputSnapshot* definition = nullptr;
    for (const auto& path : projectInputs) {
        const CMakeInputSnapshot* match = nullptr;
        for (const auto& input : inputs) {
            if (!equalPath(input.path, path))
                continue;
            if (match)
                return fail(ErrorCode::Unsupported, "Duplicate configured input snapshot");
            match = &input;
        }
        if (!match)
            return fail(ErrorCode::Unsupported, "Missing configured input snapshot");
        if (equalPath(path, definitionPath)) {
            const auto isEditable = std::ranges::any_of(model.inputs, [&](const CMakeInputFile& entry) {
                return equalPath(entry.path, path) && !entry.generated && !entry.external && !entry.cmake;
            });
            if (!isEditable)
                return fail(ErrorCode::Unsupported, "Generated input cannot be edited");
            definition = match;
        }
    }
    if (!definition)
        return fail(ErrorCode::Unsupported, "Origin is not a configured project input");
    const auto contents = std::string_view(definition->contents);
    if (auto graph = validateInvocationGraph(model, inputs); !graph)
        return std::unexpected(std::move(graph.error()));
    const auto& target = model.targets[targetIndex];
    if (target.generatorProvided || target.type == CMakeTargetType::Utility ||
        target.type == CMakeTargetType::InterfaceLibrary || target.name.empty() || target.backtrace.empty())
        return fail(ErrorCode::Unsupported, "Target has no supported configured declaration");
    const CMakeSource* source = nullptr;
    for (const auto& candidate : target.sources) {
        if (!equalPath(candidate.path, sourcePath))
            continue;
        if (source)
            return fail(ErrorCode::Unsupported, "Ambiguous configured source");
        source = &candidate;
    }
    if (operation == CMakeMembershipOperation::Add && source)
        return fail(ErrorCode::AlreadyExists, "Source is already in target");
    if (operation == CMakeMembershipOperation::Remove && !source)
        return fail(ErrorCode::NotFound, "Source is not in target");
    if (source && (source->generated || source->backtraces.size() != 1 || source->backtraces.front().empty()))
        return fail(ErrorCode::Unsupported, "Source has generated or ambiguous origin");
    const auto& chain = source ? source->backtraces.front() : target.backtrace;
    for (std::size_t i = 1; i < chain.size(); ++i) {
        if (!chain[i].command.empty() && chain[i].command != "add_subdirectory")
            return fail(ErrorCode::Unsupported, "Unsupported command invocation context");
    }
    const auto& trace = chain.front();
    if (!trace.line || !equalPath(trace.file, definitionPath))
        return fail(ErrorCode::Unsupported, "Configured origin does not identify this input");
    // With CMP0076 NEW, target_sources resolves relative paths at the calling directory.
    // Require a 3.13+ policy baseline in the root and every explicit minimum-version command;
    // cmake_policy and indirect code are rejected by the command whitelist below.
    bool rootPolicy = false;
    bool policiesKnown = true;
    const auto rootList = io::joinPath(model.sourceDirectory, "CMakeLists.txt");
    for (const auto& snapshot : inputs) {
        const auto input = std::ranges::find_if(model.inputs, [&](const CMakeInputFile& entry) {
            return equalPath(entry.path, snapshot.path) && !entry.cmake;
        });
        auto commands = scanCMakeSyntax(snapshot.contents);
        if (!commands)
            return std::unexpected(std::move(commands.error()));
        if (input != model.inputs.end() && input->generated) {
            if (!safeGeneratedCommands(*commands))
                return fail(ErrorCode::Unsupported, "Unexpected generated input code");
            continue;
        }
        if (rootList && equalPath(snapshot.path, *rootList)) {
            rootPolicy = !commands->empty() && commands->front().name == "cmake_minimum_required" &&
                         policyAtLeast313(commands->front());
        }
        for (const auto& command : *commands) {
            if (command.name == "cmake_minimum_required" && !policyAtLeast313(command))
                policiesKnown = false;
        }
    }
    const bool newRelativePolicy = rootPolicy && policiesKnown;
    // A source can be named in multiple commands although the File API reports only one
    // backtrace. Scan every configured project input, and reject indirect mutation forms.
    std::size_t occurrences = 0;
    for (const auto& snapshot : inputs) {
        const auto input = std::ranges::find_if(model.inputs, [&](const CMakeInputFile& entry) {
            return equalPath(entry.path, snapshot.path) && !entry.cmake;
        });
        if (input != model.inputs.end() && input->generated)
            continue;
        auto commands = scanCMakeSyntax(snapshot.contents);
        if (!commands)
            return std::unexpected(std::move(commands.error()));
        const auto list = io::joinPath(target.sourceDirectory, "CMakeLists.txt");
        const bool sameDirectory = list && equalPath(*list, snapshot.path);
        const auto parent = io::parentPath(snapshot.path);
        const bool directList = io::fileName(snapshot.path) == "CMakeLists.txt" && !parent.empty();
        const std::string_view invocationBase = sameDirectory ? std::string_view(target.sourceDirectory)
                                                : directList && newRelativePolicy ? parent
                                                                                  : std::string_view{};
        for (const auto& command : *commands) {
            const bool sourceCommand =
                command.name == "add_library" || command.name == "add_executable" || command.name == "target_sources";
            const bool safeOther =
                command.name == "cmake_minimum_required" || command.name == "project" ||
                command.name == "add_subdirectory" || command.name == "if" || command.name == "elseif" ||
                command.name == "else" || command.name == "endif" || command.name == "target_include_directories" ||
                command.name == "target_compile_definitions" || command.name == "target_compile_options" ||
                command.name == "target_compile_features" || languageStandardSetting(command);
            if (!sourceCommand) {
                if (!safeOther)
                    return fail(ErrorCode::Unsupported, "Cannot prove source ownership through this command");
                continue;
            }
            if (command.arguments.empty() || !command.arguments[0].isLiteral)
                return fail(ErrorCode::Unsupported, "Dynamic target name cannot establish source ownership");
            if (command.arguments[0].literal != target.name)
                continue;
            auto args = sourceArguments(command, target.name);
            if (!args)
                return std::unexpected(std::move(args.error()));
            for (const auto* argument : args->sources) {
                const auto& arg = *argument;
                if (!io::isAbsolutePath(arg.literal) && invocationBase.empty())
                    return fail(ErrorCode::Unsupported, "Cannot prove relative source invocation directory");
                if (!safeLiteral(arg.literal))
                    return fail(ErrorCode::Unsupported, "Cannot prove source spelling");
                if (sourceMatches(arg.literal, sourcePath, invocationBase, !invocationBase.empty()))
                    ++occurrences;
            }
        }
    }
    if ((operation == CMakeMembershipOperation::Add && occurrences != 0) ||
        (operation == CMakeMembershipOperation::Remove && occurrences != 1))
        return fail(ErrorCode::Unsupported, "Source has zero or multiple literal declarations");
    auto scanned = scanCMakeSyntax(contents);
    if (!scanned)
        return std::unexpected(std::move(scanned.error()));
    const CMakeSyntaxCommand* selected = nullptr;
    for (const auto& command : *scanned) {
        if (command.line != *trace.line || command.name != trace.command)
            continue;
        if (selected)
            return fail(ErrorCode::Unsupported, "Ambiguous command at configured origin");
        selected = &command;
    }
    if (!selected)
        return fail(ErrorCode::Unsupported, "Configured origin has no unique command");
    auto selectedSources = sourceArguments(*selected, target.name);
    if (!selectedSources)
        return std::unexpected(std::move(selectedSources.error()));
    if (operation == CMakeMembershipOperation::Add && selected->name == "target_sources")
        return fail(ErrorCode::Unsupported, "Target origin must be an ordinary target declaration");
    auto listPath = io::joinPath(target.sourceDirectory, "CMakeLists.txt");
    const bool targetList = listPath && equalPath(*listPath, definitionPath);
    const auto definitionParent = io::parentPath(definitionPath);
    const bool directList = io::fileName(definitionPath) == "CMakeLists.txt" && !definitionParent.empty();
    const std::string_view selectedBase = targetList                        ? std::string_view(target.sourceDirectory)
                                          : directList && newRelativePolicy ? definitionParent
                                                                            : std::string_view{};
    const bool relativeAllowed = !selectedBase.empty();
    CMakeMembershipPlan plan{.definitionPath = std::string(definitionPath),
                             .original = std::string(contents),
                             .replacement = std::string(contents)};
    const CMakeSyntaxArgument* removedArgument = nullptr;
    std::string addedSpelling;
    if (operation == CMakeMembershipOperation::Remove) {
        const CMakeSyntaxArgument* found = nullptr;
        for (const auto* argument : selectedSources->sources) {
            const auto& arg = *argument;
            if (!sourceMatches(arg.literal, sourcePath, selectedBase, relativeAllowed))
                continue;
            if (found)
                return fail(ErrorCode::Unsupported, "Ambiguous literal source argument");
            found = &arg;
        }
        if (!found)
            return fail(ErrorCode::Unsupported, "Configured source has no unique literal argument");
        removedArgument = found;
        plan.replacement.erase(found->begin, found->end - found->begin);
    } else {
        std::string spelling = normalizedSource;
        // Prefer relocatable paths when the target declaration is in its source directory.
        if (targetList) {
            std::string base = target.sourceDirectory;
            std::replace(base.begin(), base.end(), '\\', '/');
            if (!base.empty() && base.back() != '/')
                base += '/';
            if (spelling.size() > base.size() && spelling[base.size() - 1] == '/' &&
                equalPath(spelling.substr(0, base.size() - 1), target.sourceDirectory)) {
                spelling.erase(0, base.size());
            }
        }
        if (spelling.find('/') == std::string::npos &&
            (spelling == "STATIC" || spelling == "SHARED" || spelling == "MODULE" || spelling == "OBJECT" ||
             spelling == "WIN32" || spelling == "MACOSX_BUNDLE" || spelling == "EXCLUDE_FROM_ALL" ||
             spelling == "IMPORTED" || spelling == "ALIAS" || spelling == "INTERFACE" || spelling == "UNKNOWN" ||
             spelling == "SYMBOLIC" || spelling == "GLOBAL"))
            spelling = "./" + spelling;
        addedSpelling = spelling;
        plan.replacement.insert(selected->close, " \"" + spelling + "\"");
    }
    // Removing the first source can turn the next source into a leading CMake option. Reparse the
    // complete edited input and require unchanged option/visibility roles and remaining source order.
    auto reparsed = scanCMakeSyntax(plan.replacement);
    if (!reparsed)
        return std::unexpected(std::move(reparsed.error()));
    const CMakeSyntaxCommand* after = nullptr;
    for (const auto& command : *reparsed) {
        if (command.line != selected->line || command.name != selected->name)
            continue;
        if (after)
            return fail(ErrorCode::Unsupported, "Edited command is ambiguous");
        after = &command;
    }
    if (!after)
        return fail(ErrorCode::Unsupported, "Edited command cannot be found");
    auto afterSources = sourceArguments(*after, target.name);
    if (!afterSources || afterSources->roles != selectedSources->roles)
        return fail(ErrorCode::Unsupported, "Edit changes CMake command roles");
    std::vector<std::pair<std::string, std::string>> expected, actual;
    for (std::size_t i = 0; i < selectedSources->sources.size(); ++i) {
        const auto* arg = selectedSources->sources[i];
        if (arg != removedArgument)
            expected.emplace_back(arg->literal, selectedSources->sourceRoles[i]);
    }
    if (operation == CMakeMembershipOperation::Add)
        expected.emplace_back(addedSpelling, "");
    for (std::size_t i = 0; i < afterSources->sources.size(); ++i)
        actual.emplace_back(afterSources->sources[i]->literal, afterSources->sourceRoles[i]);
    if (actual != expected)
        return fail(ErrorCode::Unsupported, "Edit changes remaining source roles");
    return plan;
}

Result<CMakeMembershipPlan> planCMakeSourceRename(const CMakeProject& model, std::size_t targetIndex,
                                                  std::string_view oldSourcePath, std::string_view newSourcePath,
                                                  std::string_view definitionPath,
                                                  std::span<const CMakeInputSnapshot> inputs) {
    if (targetIndex >= model.targets.size() || !io::isAbsolutePath(newSourcePath) ||
        equalPath(oldSourcePath, newSourcePath))
        return fail(ErrorCode::InvalidArgument, "Invalid CMake source rename");
    std::string destination(newSourcePath);
    std::replace(destination.begin(), destination.end(), '\\', '/');
    const auto leaf = io::fileName(destination);
    auto normalized = io::normalizePath(destination);
    if (normalized)
        std::replace(normalized->begin(), normalized->end(), '\\', '/');
    if (!safeLiteral(destination) || destination.empty() || destination.back() == '/' || leaf.empty() || leaf == "." ||
        leaf == ".." || !normalized || *normalized != destination)
        return fail(ErrorCode::InvalidArgument, "Destination must be a normalized source-file endpoint");
    if (!equalPath(io::parentPath(oldSourcePath), io::parentPath(destination)))
        return fail(ErrorCode::Unsupported, "CMake source moves require separate path-context support");
    for (std::size_t i = 0; i < model.targets.size(); ++i) {
        for (const auto& source : model.targets[i].sources) {
            if (equalPath(source.path, destination))
                return fail(ErrorCode::AlreadyExists, "Destination is already a configured source");
            if (i != targetIndex && equalPath(source.path, oldSourcePath))
                return fail(ErrorCode::Unsupported, "Source belongs to another configured target");
        }
    }
    // Reuse all invocation/input/role checks. The removal is only an internal proof of the
    // selected literal span, never a published intermediate edit or a mutated configured model.
    auto removal = planCMakeMembership(model, targetIndex, CMakeMembershipOperation::Remove, oldSourcePath,
                                       definitionPath, inputs);
    if (!removal)
        return std::unexpected(std::move(removal.error()));
    auto commands = scanCMakeSyntax(removal->original);
    if (!commands)
        return std::unexpected(std::move(commands.error()));
    const CMakeSyntaxCommand* selected = nullptr;
    const CMakeSyntaxArgument* literal = nullptr;
    for (const auto& command : *commands) {
        for (const auto& argument : command.arguments) {
            if (!argument.isLiteral)
                continue;
            auto without = removal->original;
            without.erase(argument.begin, argument.end - argument.begin);
            if (without != removal->replacement)
                continue;
            if (literal)
                return fail(ErrorCode::Unsupported, "Ambiguous source argument span");
            selected = &command;
            literal = &argument;
        }
    }
    if (!selected || !literal)
        return fail(ErrorCode::Unsupported, "Source argument span is unavailable");
    // Same-parent rename preserves the proven relative invocation base. Quoting prevents an
    // option-looking new leaf from acquiring a command role; all other token bytes are retained.
    auto spelling = literal->literal;
    const auto slash = spelling.find_last_of('/');
    spelling.replace(slash == std::string::npos ? 0 : slash + 1, std::string::npos, io::fileName(destination));
    if (spelling.find('/') == std::string::npos)
        spelling = "./" + spelling;
    CMakeMembershipPlan plan{std::string(definitionPath), removal->original, removal->original};
    plan.replacement.replace(literal->begin, literal->end - literal->begin, "\"" + spelling + "\"");
    auto parsed = scanCMakeSyntax(plan.replacement);
    if (!parsed)
        return std::unexpected(std::move(parsed.error()));
    const CMakeSyntaxCommand* changed = nullptr;
    for (const auto& command : *parsed) {
        if (command.line == selected->line && command.name == selected->name) {
            if (changed)
                return fail(ErrorCode::Unsupported, "Edited command is ambiguous");
            changed = &command;
        }
    }
    if (!changed)
        return fail(ErrorCode::Unsupported, "Edited command is missing");
    auto before = sourceArguments(*selected, model.targets[targetIndex].name);
    auto after = sourceArguments(*changed, model.targets[targetIndex].name);
    if (!before || !after || before->roles != after->roles || before->sourceRoles != after->sourceRoles ||
        before->sources.size() != after->sources.size())
        return fail(ErrorCode::Unsupported, "Rename changes CMake command roles");
    for (std::size_t i = 0; i < before->sources.size(); ++i) {
        const auto& expected = before->sources[i] == literal ? spelling : before->sources[i]->literal;
        if (after->sources[i]->literal != expected)
            return fail(ErrorCode::Unsupported, "Rename changes another source argument");
    }
    return plan;
}

Result<std::vector<CMakeMembershipPlan>> planCMakeSourceRenameAllTargets(const CMakeProject& model,
                                                                         std::string_view oldSourcePath,
                                                                         std::string_view newSourcePath,
                                                                         std::span<const CMakeInputSnapshot> inputs) {
    struct Change {
        std::size_t begin;
        std::size_t end;
        std::string replacement;
    };
    struct Definition {
        std::string original;
        std::vector<Change> changes;
    };
    std::map<std::string, Definition> definitions;
    bool found = false;
    for (const auto& input : inputs) {
        const auto modeled =
            std::ranges::find_if(model.inputs, [&](const auto& item) { return equalPath(item.path, input.path); });
        if (modeled == model.inputs.end())
            return fail(ErrorCode::Unsupported, "Captured CMake input is not in the published input graph");
        if (modeled->generated)
            continue;
        auto parsed = scanCMakeSyntax(input.contents);
        if (!parsed)
            return std::unexpected(std::move(parsed.error()));
        for (const auto& command : *parsed) {
            if (command.name != "add_library" && command.name != "add_executable" && command.name != "target_sources")
                continue;
            if (command.arguments.empty() || !command.arguments[0].isLiteral)
                return fail(ErrorCode::Unsupported, "Current target name is not a literal");
            const auto* target = model.findTarget(command.arguments[0].literal);
            if (!target)
                return fail(ErrorCode::Unsupported,
                            "Current target declarations differ from the published model; reconfigure first");
            auto args = sourceArguments(command, target->name);
            if (!args)
                return std::unexpected(std::move(args.error()));
            for (const auto* argument : args->sources) {
                if (!safeLiteral(argument->literal))
                    return fail(ErrorCode::Unsupported, "Current source membership is indirect");
                if (sourceMatches(argument->literal, newSourcePath, io::parentPath(input.path), true) ||
                    sourceMatches(argument->literal, newSourcePath, target->sourceDirectory, true))
                    return fail(ErrorCode::AlreadyExists,
                                "Destination is already named by a current CMake source command");
                if ((sourceMatches(argument->literal, oldSourcePath, io::parentPath(input.path), true) ||
                     sourceMatches(argument->literal, oldSourcePath, target->sourceDirectory, true)) &&
                    std::ranges::none_of(target->sources,
                                         [&](const auto& source) { return equalPath(source.path, oldSourcePath); }))
                    return fail(ErrorCode::Unsupported,
                                "Current source membership differs from the published model; reconfigure first");
            }
        }
    }
    for (const auto& target : model.targets) {
        for (const auto& source : target.sources) {
            if (equalPath(source.path, newSourcePath))
                return fail(ErrorCode::AlreadyExists, "Destination is already a configured source");
            if (!equalPath(source.path, oldSourcePath))
                continue;
            found = true;
            if (source.backtraces.size() != 1 || source.backtraces[0].empty())
                return fail(ErrorCode::Unsupported, "A source membership has ambiguous provenance");
            CMakeProject selected = model;
            selected.targets = {target};
            // Every source command above is unscoped and contains only literals; the complete
            // invocation graph is checked by the inner planner. Such memberships are identical
            // in each configuration by construction, not inferred from one configuration's
            // generated source list. Dynamic/configuration-dependent commands were refused.
            selected.multiConfig = false;
            selected.configurations = {model.configuration};
            const auto& path = source.backtraces[0][0].file;
            auto plan = planCMakeSourceRename(selected, 0, oldSourcePath, newSourcePath, path, inputs);
            if (!plan)
                return std::unexpected(std::move(plan.error()));
            auto& definition = definitions[plan->definitionPath];
            if (!definition.changes.empty() && definition.original != plan->original)
                return fail(ErrorCode::InvalidArgument, "Configured definition snapshots disagree");
            definition.original = plan->original;
            std::size_t begin = 0;
            while (begin < plan->original.size() && begin < plan->replacement.size() &&
                   plan->original[begin] == plan->replacement[begin])
                ++begin;
            std::size_t suffix = 0;
            while (suffix < plan->original.size() - begin && suffix < plan->replacement.size() - begin &&
                   plan->original[plan->original.size() - 1 - suffix] ==
                       plan->replacement[plan->replacement.size() - 1 - suffix])
                ++suffix;
            Change change{begin, plan->original.size() - suffix,
                          plan->replacement.substr(begin, plan->replacement.size() - begin - suffix)};
            const auto duplicate = std::ranges::find_if(definition.changes, [&](const auto& item) {
                return item.begin == change.begin && item.end == change.end && item.replacement == change.replacement;
            });
            if (duplicate == definition.changes.end())
                definition.changes.push_back(std::move(change));
        }
    }
    if (!found)
        return fail(ErrorCode::NotFound, "Source is not in any configured target");
    std::vector<CMakeMembershipPlan> result;
    for (auto& [path, definition] : definitions) {
        std::ranges::sort(definition.changes, {}, &Change::begin);
        for (std::size_t i = 1; i < definition.changes.size(); ++i)
            if (definition.changes[i].begin < definition.changes[i - 1].end)
                return fail(ErrorCode::InvalidArgument, "Target memberships propose overlapping definition edits");
        CMakeMembershipPlan plan{path, definition.original, definition.original};
        for (auto it = definition.changes.rbegin(); it != definition.changes.rend(); ++it)
            plan.replacement.replace(it->begin, it->end - it->begin, it->replacement);
        result.push_back(std::move(plan));
    }
    return result;
}
Result<CMakeMembershipPlan> planCMakeTemplateTarget(const CMakeProject& model, std::string_view name,
    bool library, std::span<const std::string> paths, std::span<const CMakeInputSnapshot> inputs) {
    if (name.empty() || name.size()>128 || paths.empty() || paths.size()>64 || !model.inputsAvailable || !model.cacheAvailable)
        return fail(ErrorCode::InvalidArgument,"Select a configured CMake project and a bounded target template");
    for (char c:name) if (!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='_'))
        return fail(ErrorCode::InvalidArgument,"Template target names use letters, digits and underscores");
    if (model.findTarget(name)) return fail(ErrorCode::AlreadyExists,"A configured target already has that name");
    std::vector<std::string> required;
    for (const auto& file:model.inputs) if(!file.cmake) {
        if(file.external || (file.generated && !knownGeneratedInput(model,file))) return fail(ErrorCode::Unsupported,"Uncaptured external CMake input");
        if(std::ranges::none_of(required,[&](const auto& path){return equalPath(path,file.path);})) required.push_back(file.path);
    }
    if(required.size()!=inputs.size() || inputs.size()>512) return fail(ErrorCode::Busy,"Capture the complete current CMake input graph");
    for(const auto& path:required) if(std::ranges::count_if(inputs,[&](const auto& input){return equalPath(input.path,path);})!=1)
        return fail(ErrorCode::Busy,"Missing or duplicate CMake input snapshot");
    if(auto graph=validateInvocationGraph(model,inputs);!graph) return std::unexpected(graph.error());
    std::vector<std::string> declared;
    const CMakeInputSnapshot* root=nullptr;
    const auto rootPath=io::joinPath(model.sourceDirectory,"CMakeLists.txt");
    if(!rootPath) return std::unexpected(rootPath.error());
    for(const auto& input:inputs) {
        if(input.contents.size()>8*1024*1024) return fail(ErrorCode::OutOfRange,"CMake template input limit exceeded");
        auto commands=scanCMakeSyntax(input.contents); if(!commands) return std::unexpected(commands.error());
        const auto descriptor=std::ranges::find_if(model.inputs,[&](const auto& file){return equalPath(file.path,input.path);});
        if(descriptor!=model.inputs.end() && descriptor->generated) {
            if(!safeGeneratedCommands(*commands)) return fail(ErrorCode::Unsupported,"Unmodeled generated compiler input");
            continue;
        }
        if(equalPath(input.path,*rootPath)) root=&input;
        for(const auto& command:*commands) {
            const bool declaration=command.name=="add_executable" || command.name=="add_library";
            const bool source=declaration || command.name=="target_sources";
            const bool safe=source || command.name=="project" || command.name=="cmake_minimum_required" || command.name=="add_subdirectory" ||
                command.name=="target_include_directories" || command.name=="target_compile_features" ||
                command.name=="target_compile_options" || command.name=="target_compile_definitions" || languageStandardSetting(command);
            if(!safe || command.scopeDepth || command.nestedArguments) return fail(ErrorCode::Unsupported,"Use a literal top-level CMake graph for this target template");
            if(source) {
                if(command.arguments.empty() || !command.arguments.front().isLiteral) return fail(ErrorCode::Unsupported,"Dynamic target identity");
                const auto& target=command.arguments.front().literal;
                if(target==name) return fail(ErrorCode::AlreadyExists,"Current CMake input already declares this target");
                if(!model.findTarget(target)) return fail(ErrorCode::Busy,"CMake targets changed; configure before generating a target");
                if(declaration) {
                    if(std::ranges::find(declared,target)!=declared.end()) return fail(ErrorCode::Unsupported,"Duplicate target declaration");
                    declared.push_back(target);
                }
                auto arguments=sourceArguments(command,target); if(!arguments) return std::unexpected(arguments.error());
                for(const auto* argument:arguments->sources) for(const auto& path:paths)
                    if(sourceMatches(argument->literal,path,io::parentPath(input.path),true))
                        return fail(ErrorCode::AlreadyExists,"A current CMake declaration already names a generated destination");
            }
        }
    }
    for(const auto& target:model.targets) if(!target.generatorProvided && target.type!=CMakeTargetType::Utility &&
        std::ranges::find(declared,target.name)==declared.end()) return fail(ErrorCode::Busy,"Configured target declaration is no longer current");
    if(!root) return fail(ErrorCode::Busy,"Root CMakeLists input is missing");
    std::string addition="\n"+std::string(library?"add_library(":"add_executable(")+std::string(name)+(library?" STATIC":"");
    for(const auto& path:paths) {
        for(const auto& target:model.targets) for(const auto& source:target.sources) if(equalPath(source.path,path))
            return fail(ErrorCode::AlreadyExists,"A generated target file is already declared by an existing target");
        auto normalized=io::normalizePath(path);
        if(!normalized || !io::isAbsolutePath(path)) return fail(ErrorCode::InvalidArgument,"Generated target file path is invalid");
        std::replace(normalized->begin(),normalized->end(),'\\','/');
        if(!safeLiteral(*normalized)) return fail(ErrorCode::InvalidArgument,"Generated target file path is not a CMake literal");
        // Absolute quoted paths have no dependence on a guessed invocation directory.
        addition+="\n    \""+*normalized+"\"";
    }
    addition+="\n)\ntarget_compile_features("+std::string(name)+" PRIVATE cxx_std_20)\n";
    if(root->contents.find("\r\n")!=std::string::npos) {
        std::string crlf; for(char c:addition) { if(c=='\n') crlf+='\r'; crlf+=c; } addition=std::move(crlf);
    }
    return CMakeMembershipPlan{root->path,root->contents,root->contents+addition};
}
} // namespace frost::project
