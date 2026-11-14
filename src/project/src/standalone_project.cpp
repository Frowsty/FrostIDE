// SPDX-License-Identifier: Apache-2.0
// Read-only compilation database discovery and literal GNU Make command planning.
#include "frost/project/standalone_project.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/project/cmake_compile_database.hpp"

#include "json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

namespace frost::project {
namespace {
Result<std::string> resolve(std::string_view base, std::string_view path) {
    return io::isAbsolutePath(path) ? io::normalizePath(path) : io::joinPath(base, path);
}
} // namespace

Result<std::vector<std::string>> compilationDatabaseSources(std::string_view text, std::string_view directory) {
    auto parsed = detail::parseJson(text);
    if (!parsed)
        return std::unexpected(parsed.error());
    if (!parsed->isArray())
        return fail(ErrorCode::Corrupt, "compile_commands.json must be an array");
    std::set<std::string> seen;
    std::vector<std::string> result;
    for (const auto& entry : parsed->items) {
        if (!entry.isObject())
            return fail(ErrorCode::Corrupt, "compilation database entry must be an object");
        std::set<std::string> keys;
        for (const auto& key : entry.keys)
            if (!keys.insert(key).second)
                return fail(ErrorCode::Corrupt, "duplicate compilation database field");
        const auto working = entry.stringAt("directory");
        const auto file = entry.stringAt("file");
        const auto* arguments = entry.find("arguments");
        const bool validArguments =
            arguments && arguments->isArray() && !arguments->items.empty() &&
            std::ranges::all_of(
                arguments->items,
                [](const auto& arg) { return arg.isString() && arg.text.find('\0') == std::string::npos; }) &&
            !arguments->items.front().text.empty();
        const auto command = entry.stringAt("command");
        if (working.empty() || file.empty() || (arguments && !validArguments) ||
            (!validArguments && (command.empty() || command.find('\0') != std::string::npos)))
            return fail(ErrorCode::Corrupt,
                        "compilation database entry needs directory, file and command or arguments");
        auto base = resolve(directory, working);
        if (!base)
            return std::unexpected(base.error());
        auto path = resolve(*base, file);
        if (!path)
            return std::unexpected(path.error());
        if (seen.insert(*path).second)
            result.push_back(std::move(*path));
    }
    return result;
}

Result<std::string> compilationDatabaseObjectTarget(std::string_view text, std::string_view directory,
                                                    std::string_view source) {
    auto valid = compilationDatabaseSources(text, directory);
    if (!valid)
        return std::unexpected(valid.error());
    auto parsed = detail::parseJson(text);
    if (!parsed)
        return std::unexpected(parsed.error());
    const auto key = [](std::string path) {
        if constexpr (io::kNativePathStyle == io::PathStyle::Windows)
            std::ranges::transform(path, path.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return path;
    };
    auto normalizedSource = io::normalizePath(source);
    if (!normalizedSource)
        return std::unexpected(normalizedSource.error());
    auto normalizedDirectory = io::normalizePath(directory);
    if (!normalizedDirectory)
        return std::unexpected(normalizedDirectory.error());
    std::set<std::string> targets;
    for (const auto& entry : parsed->items) {
        auto base = resolve(directory, entry.stringAt("directory"));
        if (!base)
            return std::unexpected(base.error());
        auto file = resolve(*base, entry.stringAt("file"));
        if (!file)
            return std::unexpected(file.error());
        if (key(*file) != key(*normalizedSource))
            continue;
        std::string output = entry.stringAt("output");
        if (output.empty()) {
            std::vector<std::string> arguments;
            if (const auto* args = entry.find("arguments"); args && args->isArray()) {
                for (const auto& arg : args->items)
                    arguments.push_back(arg.text);
            } else
                arguments = splitCompileCommandFragment(entry.stringAt("command"), nativeCMakeShell());
            for (std::size_t i = 1; i < arguments.size(); ++i) {
                if ((arguments[i] == "-o" || arguments[i] == "/Fo") && i + 1 < arguments.size())
                    output = arguments[++i];
                else if (arguments[i].starts_with("-o") && arguments[i].size() > 2)
                    output = arguments[i].substr(2);
                else if (arguments[i].starts_with("/Fo") && arguments[i].size() > 3)
                    output = arguments[i].substr(3);
            }
        }
        if (output.empty())
            return fail(ErrorCode::Unsupported, "the database has no object output for this file");
        auto object = resolve(*base, output);
        if (!object)
            return std::unexpected(object.error());
        if (!key(*object).ends_with(".o") && !key(*object).ends_with(".obj"))
            return fail(ErrorCode::Unsupported, "the database output is not a compiler object file");
        std::string prefix = key(*normalizedDirectory);
        if (!prefix.ends_with('/') && !prefix.ends_with('\\'))
            prefix += io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';
        if (!key(*object).starts_with(prefix))
            return fail(ErrorCode::Unsupported, "object output is outside the active build directory");
        std::string target = object->substr(prefix.size());
        std::ranges::replace(target, '\\', '/');
        if (target.empty() || target.front() == '-' || target.find_first_of("\r\n\0", 0, 3) != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "invalid object build target");
        targets.insert(std::move(target));
    }
    if (targets.size() != 1)
        return fail(ErrorCode::Unsupported,
                    "choose a unique target context for this file; its object output is missing or ambiguous");
    return *targets.begin();
}

Result<std::optional<StandaloneProject>> readStandaloneProject(io::IFileSystem& fs, std::string_view root,
                                                               bool readSources) {
    auto normalized = io::normalizePath(root);
    if (!normalized)
        return std::unexpected(normalized.error());
    StandaloneProject result{.root = *normalized};
    const auto find = [&](auto names) -> Result<std::optional<std::string>> {
        for (const auto name : names) {
            auto path = io::joinPath(*normalized, name);
            if (!path)
                return std::unexpected(path.error());
            auto status = fs.status(*path);
            if (!status) {
                if (status.error().code() == ErrorCode::NotFound)
                    continue;
                return std::unexpected(status.error());
            }
            if (status->type == io::FileType::Regular)
                return std::optional<std::string>(*path);
        }
        return std::optional<std::string>{};
    };
    auto make = find(std::array{"GNUmakefile", "makefile", "Makefile"});
    if (!make)
        return std::unexpected(make.error());
    result.makefile = std::move(*make);
    auto database = find(std::array{"compile_commands.json", "build/compile_commands.json"});
    if (!database)
        return std::unexpected(database.error());
    result.database = std::move(*database);
    if (!result.makefile && !result.database)
        return std::optional<StandaloneProject>{};
    if (result.database && readSources) {
        auto status = fs.status(*result.database);
        if (!status)
            return std::unexpected(status.error());
        if (status->size > 64 * 1024 * 1024)
            return fail(ErrorCode::OutOfRange, "compilation database exceeds the 64 MiB project model limit");
        auto bytes = fs.readFile(*result.database);
        if (!bytes)
            return std::unexpected(bytes.error());
        if (bytes->size() > 64 * 1024 * 1024)
            return fail(ErrorCode::OutOfRange, "compilation database grew beyond the 64 MiB project model limit");
        auto sources = compilationDatabaseSources(*bytes, io::parentPath(*result.database));
        if (!sources)
            return std::unexpected(sources.error());
        result.sources = std::move(*sources);
    }
    return std::optional<StandaloneProject>(std::move(result));
}

Result<std::vector<std::vector<std::string>>> makeProjectArguments(const StandaloneProject& project, MakeAction action,
                                                                   std::string_view target) {
    if (!project.makefile)
        return fail(ErrorCode::Unsupported,
                    "a compilation database does not define a build; open a Makefile or CMake project");
    if (!target.empty() && (target.front() == '-' || target.find_first_of("=\r\n\0", 0, 4) != std::string_view::npos))
        return fail(ErrorCode::InvalidArgument,
                    "Make target must be a literal target, not an option or variable assignment");
    std::vector<std::vector<std::string>> steps;
    const auto add = [&](std::string_view name) {
        std::vector<std::string> args{"-f", *project.makefile};
        if (!name.empty())
            args.emplace_back(name);
        steps.push_back(std::move(args));
    };
    if (action == MakeAction::Clean || action == MakeAction::Rebuild)
        add("clean");
    if (action == MakeAction::Build || action == MakeAction::Rebuild)
        add(target);
    if (action == MakeAction::Install)
        add("install");
    return steps;
}
} // namespace frost::project
