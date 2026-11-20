// SPDX-License-Identifier: Apache-2.0
// Conservative whole-input CMake target rename slots and parser-backed diagnostics.
#include "frost/lang/cmake_refactoring.hpp"

#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/cmake_syntax.hpp"
#include "frost/text/text_buffer.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
namespace frost::lang {
namespace {
bool targetCommand(std::string_view name) {
    return name == "target_sources" || name == "target_link_libraries" || name == "target_include_directories" ||
           name == "target_compile_definitions" || name == "target_compile_options" ||
           name == "target_compile_features" || name == "target_link_options" || name == "target_link_directories" ||
           name == "target_precompile_headers" || name == "set_target_properties" || name == "add_dependencies";
}
bool declaration(std::string_view name) {
    return name == "add_library" || name == "add_executable" || name == "add_custom_target";
}
bool reservedTarget(std::string_view name) {
    constexpr std::string_view words[] = {
        "PUBLIC",        "PRIVATE",          "INTERFACE", "LINK_PUBLIC", "LINK_PRIVATE", "LINK_INTERFACE_LIBRARIES",
        "debug",         "optimized",        "general",   "ALIAS",       "IMPORTED",     "GLOBAL",
        "STATIC",        "SHARED",           "MODULE",    "OBJECT",      "UNKNOWN",      "WIN32",
        "MACOSX_BUNDLE", "EXCLUDE_FROM_ALL", "ALL",       "COMMAND",     "NAME",         "TARGET",
        "PROPERTY",      "PROPERTIES",       "BEFORE",    "AFTER",       "SYSTEM"};
    return std::ranges::find(words, name) != std::end(words);
}
bool irrelevantDynamicCommand(std::string_view name) {
    constexpr std::string_view names[] = {"set",
                                          "unset",
                                          "option",
                                          "project",
                                          "cmake_minimum_required",
                                          "message",
                                          "string",
                                          "list",
                                          "math",
                                          "get_filename_component",
                                          "cmake_path",
                                          "get_cmake_property",
                                          "mark_as_advanced",
                                          "cmake_policy",
                                          "cmake_host_system_information"};
    return std::ranges::find(names, name) != std::end(names);
}
bool validTarget(std::string_view name) {
    return !name.empty() && !reservedTarget(name) && std::ranges::all_of(name, [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '+';
    });
}
} // namespace
Result<CMakeRenameCapture> planCMakeTargetRename(const CMakeLanguageInput& input, LspPosition position,
                                                 std::string newName) {
    if (!input.model || !input.model->inputsAvailable)
        return fail(ErrorCode::Unsupported,
                    "Configure this project to supply actual target and CMake input provenance");
    if (!validTarget(newName))
        return fail(ErrorCode::InvalidArgument, "Invalid literal CMake target name");
    auto at = fromLspPosition(input.source, position, PositionEncoding::Utf16);
    if (!at)
        return std::unexpected(at.error());
    auto active = project::scanCMakeSyntax(input.source.text());
    if (!active)
        return std::unexpected(active.error());
    std::string oldName;
    for (const auto& command : *active)
        for (const auto& arg : command.arguments)
            if (arg.isLiteral && arg.begin <= at->value() && at->value() <= arg.end)
                oldName = arg.literal;
    if (!validTarget(oldName) || oldName == newName)
        return fail(ErrorCode::Unsupported, "Select a literal configured target name");
    const auto* configured = input.model->findTarget(oldName);
    if (!configured || configured->generatorProvided || configured->backtrace.empty())
        return fail(ErrorCode::Unsupported, "Target has no configured source definition");
    if (input.model->findTarget(newName))
        return fail(ErrorCode::AlreadyExists, "A configured target already has that name");
    auto files = input.inputs;
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const auto& row) { return documentKey(row.first) == documentKey(input.path); }),
                files.end());
    files.insert(files.begin(), {input.path, input.source.text()});
    for (const auto& file : input.model->inputs) {
        if (file.cmake)
            continue; // Pinned CMake distribution is outside editable user source.
        if (!std::ranges::any_of(files,
                                 [&](const auto& row) { return documentKey(row.first) == documentKey(file.path); }))
            return fail(ErrorCode::Busy, "Configured CMake input was not captured: " + file.path);
    }
    // add_subdirectory(relative) is relative to the caller's source directory (CMake
    // Help/command/add_subdirectory.rst). Absolute include() retains that directory.
    // Relative include() also searches modules first (Kitware/CMake, BSD-3-Clause,
    // Source/cmIncludeCommand.cxx); without evaluated search provenance it stays opaque.
    std::map<std::string, std::set<std::string>> sourceContexts;
    std::vector<std::pair<std::string, std::string>> pending;
    auto rootList = io::joinPath(input.model->sourceDirectory, "CMakeLists.txt");
    if (!rootList)
        return std::unexpected(rootList.error());
    pending.emplace_back(*rootList, input.model->sourceDirectory);
    for (std::size_t visit = 0; visit < pending.size(); ++visit) {
        if (pending.size() > 4096)
            return fail(ErrorCode::OutOfRange, "CMake source-directory contexts exceed rename budget");
        const auto path = pending[visit].first, directory = pending[visit].second;
        if (!sourceContexts[documentKey(path)].insert(directory).second)
            continue;
        const auto file = std::find_if(files.begin(), files.end(),
                                       [&](const auto& row) { return documentKey(row.first) == documentKey(path); });
        if (file == files.end())
            return fail(ErrorCode::Busy, "CMake source graph is not fully captured");
        auto parsed = project::scanCMakeSyntax(file->second);
        if (!parsed)
            return std::unexpected(parsed.error());
        for (const auto& command : *parsed)
            if (command.name == "include" || command.name == "add_subdirectory") {
                if (command.scopeDepth != 0 || command.nestedArguments || command.arguments.empty() ||
                    !command.arguments[0].isLiteral ||
                    (command.name == "include" &&
                     (command.arguments.size() != 1 || !io::isAbsolutePath(command.arguments[0].literal))))
                    return fail(
                        ErrorCode::Unsupported,
                        "Conditional/optional/dynamic/module-search CMake imports require evaluated import provenance");
                auto target = io::isAbsolutePath(command.arguments[0].literal)
                                  ? io::normalizePath(command.arguments[0].literal)
                                  : io::joinPath(directory, command.arguments[0].literal);
                if (!target)
                    return std::unexpected(target.error());
                auto list = command.name == "include" ? target : io::joinPath(*target, "CMakeLists.txt");
                if (!list)
                    return std::unexpected(list.error());
                if (!std::ranges::any_of(files,
                                         [&](const auto& row) { return documentKey(row.first) == documentKey(*list); }))
                    return fail(ErrorCode::Busy, "Direct CMake import is absent from actual File API inputs");
                pending.emplace_back(*list, command.name == "include" ? directory : *target);
            }
    }
    CMakeRenameCapture result;
    result.model = input.model;
    result.inputs = files;
    std::size_t definitions = 0;
    for (const auto& [path, bytes] : files) {
        auto parsed = project::scanCMakeSyntax(bytes);
        if (!parsed)
            return std::unexpected(parsed.error());
        auto buffer = text::TextBuffer::fromUtf8(bytes);
        if (!buffer)
            return std::unexpected(buffer.error());
        FileEdit edits;
        edits.path = path;
        edits.uri = pathToUri(path).value_or("");
        edits.path = uriToPath(edits.uri).value_or(path);
        for (const auto& command : *parsed) {
            if (command.name == "cmake_language" || command.name == "load_command" || command.name == "variable_watch")
                return fail(ErrorCode::Unsupported,
                            "Indirect CMake code/callback evaluation is outside target rename provenance");
            if ((command.name == "include" || command.name == "add_subdirectory") &&
                !sourceContexts.contains(documentKey(path)))
                return fail(ErrorCode::Unsupported, "CMake import has no captured caller source-directory context");
            for (std::size_t i = 0; i < command.arguments.size(); ++i) {
                const auto& argument = command.arguments[i];
                if (!argument.isLiteral) {
                    if (!irrelevantDynamicCommand(command.name))
                        return fail(
                            ErrorCode::Unsupported,
                            "Dynamic target/import/user-command arguments require evaluated refactoring provenance");
                    continue;
                }
                if (argument.literal == newName && declaration(command.name) && i == 0)
                    return fail(ErrorCode::AlreadyExists, "A source target already has that name");
                if (argument.literal != oldName)
                    continue;
                bool slot = false;
                if (declaration(command.name) && i == 0) {
                    if (command.scopeDepth != 0 || command.nestedArguments)
                        return fail(ErrorCode::Unsupported,
                                    "Conditional/function target definition is outside the literal rename profile");
                    const auto& origin = configured->backtrace.front();
                    if (documentKey(origin.file) != documentKey(path) || origin.line.value_or(0) != command.line)
                        return fail(ErrorCode::Busy, "Target definition differs from the last configured backtrace");
                    ++definitions;
                    slot = true;
                } else if (command.name == "target_link_libraries" || command.name == "add_dependencies")
                    slot = true;
                else if (targetCommand(command.name) && i == 0)
                    slot = true;
                else if (command.name == "get_target_property" && i == 1)
                    slot = true;
                else if ((command.name == "if" || command.name == "elseif") && i > 0 &&
                         command.arguments[i - 1].literal == "TARGET")
                    slot = true;
                else if (command.name == "add_test" && i > 0 && command.arguments[i - 1].literal == "COMMAND")
                    slot = true;
                if (!slot)
                    return fail(ErrorCode::Unsupported,
                                "Target name occurs in an unsupported/indirect CMake command: " + command.name);
                const auto root = documentKey(input.model->sourceDirectory), key = documentKey(path);
                const auto inputFile = std::find_if(input.model->inputs.begin(), input.model->inputs.end(),
                                                    [&](const auto& file) { return documentKey(file.path) == key; });
                if (root.empty() ||
                    !(key.starts_with(root) && key.size() > root.size() &&
                      (key[root.size()] == '/' || key[root.size()] == '\\')) ||
                    (inputFile == input.model->inputs.end() || inputFile->generated || inputFile->external))
                    return fail(ErrorCode::Unsupported, "Target rename cannot edit generated/external project inputs");
                const auto span = std::string_view(bytes).substr(argument.begin, argument.end - argument.begin);
                auto start = argument.begin, end = argument.end;
                // Quoted and bracket arguments retain delimiters; only exact spelling is replaced.
                if (span != oldName) {
                    const auto found = span.find(oldName);
                    if (found == span.npos || span.find(oldName, found + 1) != span.npos)
                        return fail(ErrorCode::Unsupported, "CMake target token has an opaque spelling");
                    start += found;
                    end = start + oldName.size();
                }
                auto begin = toLspPosition(buffer->snapshot(), ByteOffset{start}, PositionEncoding::Utf16);
                auto finish = toLspPosition(buffer->snapshot(), ByteOffset{end}, PositionEncoding::Utf16);
                edits.edits.push_back({{begin, finish}, newName});
            }
        }
        if (!edits.edits.empty())
            result.edit.files.push_back(std::move(edits));
    }
    if (definitions != 1)
        return fail(ErrorCode::Unsupported, "A unique configured literal target definition is required");
    return result;
}
Result<std::vector<SnapshotValidationIssue>> inspectCMakeSource(const text::Snapshot& source) {
    if (source.size().value() > CMakeLanguageService::kMaxInputBytes)
        return fail(ErrorCode::OutOfRange, "CMake source exceeds the 256 KiB analysis budget");
    constexpr std::string_view provider = "FrostIDE CMake source parser";
    auto parsed = project::scanCMakeSyntax(source.text());
    if (!parsed)
        return std::vector<SnapshotValidationIssue>{{provider, "cmake.syntax", parsed.error().describe(), {}}};
    std::set<std::string> targets;
    std::vector<SnapshotValidationIssue> result;
    for (const auto& command : *parsed)
        if (declaration(command.name) && command.scopeDepth == 0 && !command.arguments.empty() &&
            command.arguments[0].isLiteral && !targets.insert(command.arguments[0].literal).second) {
            const auto& target = command.arguments.front();
            const auto line = source.lineInfo(LineIndex{static_cast<std::uint32_t>(command.line - 1)});
            const auto start = line->content.begin;
            result.push_back({provider, "cmake.duplicate-target",
                              "Duplicate unconditional literal target declaration: " + target.literal,
                              {start, start}});
            if (result.size() == kSnapshotValidationMaxIssues)
                break;
        }
    return result;
}
} // namespace frost::lang
