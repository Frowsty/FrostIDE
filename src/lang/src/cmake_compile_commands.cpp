// SPDX-License-Identifier: Apache-2.0
//
// CPP-3 (cmake_compile_commands.hpp): the owned compile-commands directory per CMake configuration,
// the copy or synthesis that fills it, and the persisted resolve-context choices.

#include "frost/lang/cmake_compile_commands.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"
#include "frost/project/cmake_compile_database.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace frost::lang {

namespace {

constexpr std::string_view kDatabaseName = "compile_commands.json";
constexpr std::string_view kContextsName = "resolve-contexts.json";

constexpr char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](char x, char y) { return lowerAscii(x) == lowerAscii(y); });
}

// FNV-1a 64 (public domain, http://www.isthe.com/chongo/tech/comp/fnv/): stable across builds and
// runs, unlike std::hash, so a configuration finds its directory (and shards) again.
std::uint64_t fnv1a(std::string_view text) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

std::string sanitized(std::string_view text) {
    std::string out(text);
    std::ranges::replace_if(out, [](char c) { return !std::isalnum(static_cast<unsigned char>(c)) && c != '-'; }, '_');
    return out;
}

CompileCommand toCompileCommand(project::CMakeCompileCommand command) {
    return CompileCommand{.directory = std::move(command.directory),
                          .file = std::move(command.file),
                          .arguments = std::move(command.arguments)};
}

// `text` (a compile_commands.json array) with `first` inserted as its first entries, or nullopt
// when the text is not an array.
std::optional<std::string> prepended(std::string_view text, std::span<const CompileCommand> first) {
    const std::size_t open = text.find_first_not_of(" \t\r\n");
    if (open == std::string_view::npos || text[open] != '[')
        return std::nullopt;
    const std::size_t next = text.find_first_not_of(" \t\r\n", open + 1);
    const bool empty = next != std::string_view::npos && text[next] == ']';
    std::string entries = compileCommandsJson(first); // "[\n  {...},\n  {...}\n]\n"
    const std::size_t begin = entries.find('{');
    const std::size_t end = entries.rfind('}');
    std::string out(text.substr(0, open + 1));
    out += "\n  ";
    out += entries.substr(begin, end - begin + 1);
    out += empty ? "\n" : ",";
    out += text.substr(open + 1);
    return out;
}

// Writes `text` to `<directory>/compile_commands.json` unless it already holds it.
Result<void> writeIfChanged(io::IFileSystem& fs, const std::string& directory, const std::string& text) {
    if (auto created = fs.createDirectories(directory); !created)
        return std::unexpected(std::move(created.error()));
    auto path = io::joinPath(directory, kDatabaseName);
    if (!path)
        return std::unexpected(std::move(path.error()));
    auto existing = fs.readFile(*path);
    if (existing && *existing == text)
        return {};
    if (!existing && existing.error().code() != ErrorCode::NotFound)
        return std::unexpected(std::move(existing.error()));
    return fs.writeFileAtomic(*path, text);
}

} // namespace

Result<std::string> cmakeCompileCommandsDirectory(std::string_view cacheRoot, std::string_view buildDirectory,
                                                  std::string_view configuration) {
    if (!io::isAbsolutePath(cacheRoot) || !io::isAbsolutePath(buildDirectory))
        return fail(ErrorCode::InvalidArgument, "the compile-commands cache and the build directory must be absolute");
    auto build = io::normalizePath(buildDirectory);
    if (!build)
        return std::unexpected(std::move(build.error()));
    std::string config(configuration);
    std::ranges::transform(config, config.begin(), lowerAscii);
    const std::string identity = documentKey(*build) + "\n" + config;
    std::string name = "cmake-" + sanitized(io::fileName(*build));
    if (!config.empty())
        name += "-" + sanitized(config);
    name += std::format("-{:016x}", fnv1a(identity));
    return io::joinPath(cacheRoot, name);
}

std::vector<CompileCommand> cmakeCodeModelCompileCommands(const project::CMakeProject& project) {
    std::vector<CompileCommand> out;
    for (auto& command : project::synthesizeCMakeCompileCommands(project))
        out.push_back(toCompileCommand(std::move(command)));
    return out;
}

bool cmakeModelDescribes(const project::CMakeProject& model, std::string_view buildDirectory,
                         std::string_view configuration) {
    auto a = io::normalizePath(model.buildDirectory);
    auto b = io::normalizePath(buildDirectory);
    if (!a || !b || documentKey(*a) != documentKey(*b))
        return false;
    return !model.multiConfig || configuration.empty() || equalsIgnoringCase(model.configuration, configuration);
}

Json compilationDatabaseChange(const CompileCommand& command) {
    Json arguments = Json::array();
    for (const std::string& argument : command.arguments)
        arguments.push(argument);
    Json changes = Json::object();
    changes.set(
        command.file,
        Json::object().set("workingDirectory", command.directory).set("compilationCommand", std::move(arguments)));
    return Json::object().set("settings", Json::object().set("compilationDatabaseChanges", std::move(changes)));
}

CMakeCompileCommandsProvider::CMakeCompileCommandsProvider(io::IFileSystem& fs, std::string cacheRoot,
                                                           std::function<CMakeCompileCommandsInput()> input)
    : m_fs(fs), m_cacheRoot(std::move(cacheRoot)), m_input(std::move(input)) {}

void CMakeCompileCommandsProvider::loadContexts() {
    if (m_contextsLoaded)
        return;
    m_contextsLoaded = true;
    auto path = io::joinPath(m_cacheRoot, kContextsName);
    if (!path)
        return;
    auto text = m_fs.readFile(*path);
    if (!text)
        return; // None saved yet (or unreadable: every file is automatic).
    auto json = parseJson(*text);
    if (!json)
        return;
    const Json* contexts = json->find("contexts");
    if (contexts == nullptr || !contexts->isArray())
        return;
    for (const Json& entry : contexts->items()) {
        const Json* file = entry.find("file");
        const Json* target = entry.find("target");
        if (file && target && file->asString() && target->asString() && !target->asString()->empty())
            m_contexts[documentKey(*file->asString())] = {*file->asString(), *target->asString()};
    }
}

std::string CMakeCompileCommandsProvider::context(std::string_view file) {
    const std::scoped_lock lock(m_mutex);
    loadContexts();
    const auto found = m_contexts.find(documentKey(file));
    return found == m_contexts.end() ? std::string() : found->second.second;
}

Result<void> CMakeCompileCommandsProvider::setContext(std::string_view file, std::string target) {
    const std::scoped_lock lock(m_mutex);
    loadContexts();
    if (target.empty())
        m_contexts.erase(documentKey(file));
    else
        m_contexts[documentKey(file)] = {std::string(file), std::move(target)};
    Json contexts = Json::array();
    for (const auto& [key, entry] : m_contexts)
        contexts.push(Json::object().set("file", entry.first).set("target", entry.second));
    const std::string text = Json::object().set("version", 1).set("contexts", std::move(contexts)).dump() + "\n";
    if (auto created = m_fs.createDirectories(m_cacheRoot); !created)
        return std::unexpected(std::move(created.error()));
    auto path = io::joinPath(m_cacheRoot, kContextsName);
    if (!path)
        return std::unexpected(std::move(path.error()));
    return m_fs.writeFileAtomic(*path, text);
}

std::optional<CompileCommand> CMakeCompileCommandsProvider::contextCommandLocked(const project::CMakeProject& model,
                                                                                 std::string_view file) {
    const auto found = m_contexts.find(documentKey(file));
    if (found == m_contexts.end())
        return std::nullopt;
    const project::CMakeTarget* target = model.findTarget(found->second.second);
    if (target == nullptr)
        return std::nullopt;
    const auto index = static_cast<std::size_t>(target - model.targets.data());
    auto command = project::cmakeCommandInTarget(model, index, found->second.first);
    if (!command)
        return std::nullopt;
    return toCompileCommand(std::move(*command));
}

std::optional<CompileCommand> CMakeCompileCommandsProvider::contextCommand(const project::CMakeProject& model,
                                                                           std::string_view file) {
    const std::scoped_lock lock(m_mutex);
    loadContexts();
    return contextCommandLocked(model, file);
}

Result<std::optional<CompileCommandsLocation>> CMakeCompileCommandsProvider::locate() {
    const std::scoped_lock writing(m_writeMutex);
    const CMakeCompileCommandsInput input = m_input ? m_input() : CMakeCompileCommandsInput{};
    if (input.buildDirectory.empty())
        return std::optional<CompileCommandsLocation>();
    auto build = io::normalizePath(input.buildDirectory);
    if (!build)
        return std::unexpected(std::move(build.error()));
    auto directory = cmakeCompileCommandsDirectory(m_cacheRoot, *build, input.configuration);
    if (!directory)
        return std::unexpected(std::move(directory.error()));
    const bool modelFits = input.model && cmakeModelDescribes(*input.model, *build, input.configuration);
    std::vector<CompileCommand> chosen;
    if (modelFits) {
        const std::scoped_lock lock(m_mutex);
        loadContexts();
        for (const auto& [key, entry] : m_contexts)
            if (auto command = contextCommandLocked(*input.model, entry.first))
                chosen.push_back(std::move(*command));
    }

    auto cmakeDatabase = io::joinPath(*build, kDatabaseName);
    if (!cmakeDatabase)
        return std::unexpected(std::move(cmakeDatabase.error()));
    std::string text;
    CompileCommandsSource source = CompileCommandsSource::CMakeCopy;
    if (auto status = m_fs.status(*cmakeDatabase); status && status->type == io::FileType::Regular) {
        auto read = m_fs.readFile(*cmakeDatabase);
        if (!read)
            return std::unexpected(std::move(read.error()));
        text = std::move(*read);
        if (!chosen.empty())
            if (auto merged = prepended(text, chosen))
                text = std::move(*merged);
    } else if (modelFits) {
        std::vector<CompileCommand> commands = std::move(chosen);
        auto synthesized = cmakeCodeModelCompileCommands(*input.model);
        if (synthesized.empty())
            return std::optional<CompileCommandsLocation>();
        commands.insert(commands.end(), std::make_move_iterator(synthesized.begin()),
                        std::make_move_iterator(synthesized.end()));
        text = compileCommandsJson(commands);
        source = CompileCommandsSource::CMakeCodeModel;
    } else {
        return std::optional<CompileCommandsLocation>(); // Not configured yet, or the reply is another's.
    }
    if (auto written = writeIfChanged(m_fs, *directory, text); !written)
        return std::unexpected(std::move(written.error()));
    auto file = io::joinPath(*directory, kDatabaseName);
    if (!file)
        return std::unexpected(std::move(file.error()));
    return std::optional<CompileCommandsLocation>(CompileCommandsLocation{
        .directory = *directory, .file = std::move(*file), .source = source, .origin = std::move(*build)});
}

} // namespace frost::lang
