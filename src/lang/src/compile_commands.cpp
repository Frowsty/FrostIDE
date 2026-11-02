// SPDX-License-Identifier: Apache-2.0
//
// Compile-command providers (compile_commands.hpp): writing databases, finding an existing one,
// synthesizing one for a Visual Studio solution, the generator hook and the chain.

#include "frost/lang/compile_commands.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/lang/json.hpp"
#include "frost/project/solution_model.hpp"
#include "frost/project/vcxproj.hpp"

#include <algorithm>
#include <format>
#include <string_view>

namespace frost::lang {
namespace {

constexpr std::string_view kDatabaseName = "compile_commands.json";

[[nodiscard]] char lowerAscii(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

[[nodiscard]] std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), lowerAscii);
    return out;
}

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lowerAscii(x) == lowerAscii(y); });
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() &&
           (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

// ';'-separated entries, trimmed, without empty ones and without unresolved $(...) / %(...) references.
[[nodiscard]] std::vector<std::string> resolvedEntries(std::string_view list) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= list.size()) {
        const std::size_t end = std::min(list.find(';', start), list.size());
        const std::string_view entry = trim(list.substr(start, end - start));
        if (!entry.empty() && entry.find("$(") == std::string_view::npos &&
            entry.find("%(") == std::string_view::npos && entry.find("@(") == std::string_view::npos) {
            out.emplace_back(entry);
        }
        start = end + 1;
    }
    return out;
}

// `entry` (absolute, or relative to `directory`, with '\' or '/') as a normalised absolute path.
[[nodiscard]] std::optional<std::string> resolvePath(std::string_view directory, std::string_view entry) {
    std::string text(entry);
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }
    if constexpr (io::kNativePathStyle == io::PathStyle::Posix) {
        std::ranges::replace(text, '\\', '/');
    }
    while (text.size() > 1 && (text.back() == '\\' || text.back() == '/')) {
        text.pop_back();
    }
    if (text.empty()) {
        return std::nullopt;
    }
    if (io::isAbsolutePath(text)) {
        auto normalized = io::normalizePath(text);
        return normalized ? std::optional<std::string>(std::move(*normalized)) : std::nullopt;
    }
    auto joined = io::joinPath(directory, text);
    if (!joined) {
        return std::nullopt;
    }
    auto normalized = io::normalizePath(*joined);
    return normalized ? std::optional<std::string>(std::move(*normalized)) : std::nullopt;
}

[[nodiscard]] std::string metadataOf(const std::map<std::string, std::string, std::less<>>& metadata,
                                     std::string_view name) {
    auto it = metadata.find(name);
    return it != metadata.end() ? std::string(trim(it->second)) : std::string();
}

[[nodiscard]] std::string_view targetFor(std::string_view platform) noexcept {
    if (iequals(platform, "Win32") || iequals(platform, "x86")) {
        return "--target=i686-pc-windows-msvc";
    }
    if (iequals(platform, "ARM64") || iequals(platform, "ARM64EC")) {
        return "--target=aarch64-pc-windows-msvc";
    }
    if (iequals(platform, "ARM")) {
        return "--target=thumbv7-pc-windows-msvc";
    }
    return "--target=x86_64-pc-windows-msvc";
}

[[nodiscard]] std::string cxxStandard(std::string_view value) {
    const std::string v = lower(value);
    if (v == "stdcpp17") {
        return "/std:c++17";
    }
    if (v == "stdcpp20") {
        return "/std:c++20";
    }
    if (v == "stdcpplatest" || v == "stdcpp23") {
        return "/std:c++latest";
    }
    return "/std:c++14"; // stdcpp14 and MSVC's default.
}

[[nodiscard]] std::optional<std::string> cStandard(std::string_view value) {
    const std::string v = lower(value);
    if (v == "stdc11") {
        return "/std:c11";
    }
    if (v == "stdc17" || v == "stdclatest") {
        return "/std:c17";
    }
    return std::nullopt; // Default: clang's C default.
}

[[nodiscard]] std::string runtimeFlag(std::string_view value, std::string_view configuration) {
    const std::string v = lower(value);
    if (v == "multithreaded") {
        return "/MT";
    }
    if (v == "multithreadeddebug") {
        return "/MTd";
    }
    if (v == "multithreadeddll") {
        return "/MD";
    }
    if (v == "multithreadeddebugdll") {
        return "/MDd";
    }
    // Not in the project file (it usually comes from Microsoft.Cpp.props): guess by name.
    return lower(configuration).find("debug") != std::string::npos ? "/MDd" : "/MD";
}

[[nodiscard]] bool isCSource(std::string_view path) noexcept {
    const std::size_t dot = path.find_last_of('.');
    return dot != std::string_view::npos && iequals(path.substr(dot), ".c");
}

[[nodiscard]] Result<std::optional<CompileCommandsLocation>> writeToCache(io::IFileSystem& fs,
                                                                          std::string_view cacheDirectory,
                                                                          std::span<const CompileCommand> commands,
                                                                          CompileCommandsSource source) {
    if (commands.empty()) {
        return std::optional<CompileCommandsLocation>();
    }
    auto directory = io::normalizePath(cacheDirectory);
    if (!directory) {
        return std::unexpected(std::move(directory.error()));
    }
    if (auto written = writeCompileCommands(fs, *directory, commands); !written) {
        return std::unexpected(std::move(written.error()));
    }
    auto file = io::joinPath(*directory, kDatabaseName);
    if (!file) {
        return std::unexpected(std::move(file.error()));
    }
    return std::optional<CompileCommandsLocation>(
        CompileCommandsLocation{.directory = std::move(*directory), .file = std::move(*file), .source = source});
}

// Directory listings come in file-system order; candidates are tried in name order.
void sortByName(std::vector<io::DirEntry>& entries) { std::ranges::sort(entries, {}, &io::DirEntry::name); }

} // namespace

std::string compileCommandsJson(std::span<const CompileCommand> commands) {
    std::string out = "[\n";
    for (std::size_t i = 0; i < commands.size(); ++i) {
        Json arguments = Json::array();
        for (const std::string& argument : commands[i].arguments) {
            arguments.push(argument);
        }
        out += "  ";
        out += Json::object()
                   .set("directory", commands[i].directory)
                   .set("file", commands[i].file)
                   .set("arguments", std::move(arguments))
                   .dump();
        out += i + 1 < commands.size() ? ",\n" : "\n";
    }
    out += "]\n";
    return out;
}

Result<bool> writeCompileCommands(io::IFileSystem& fs, std::string_view directory,
                                  std::span<const CompileCommand> commands) {
    if (auto created = fs.createDirectories(directory); !created) {
        return std::unexpected(std::move(created.error()));
    }
    auto path = io::joinPath(directory, kDatabaseName);
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    const std::string text = compileCommandsJson(commands);
    auto existing = fs.readFile(*path);
    if (existing && *existing == text) {
        return false;
    }
    if (!existing && existing.error().code() != ErrorCode::NotFound) {
        return std::unexpected(std::move(existing.error()));
    }
    if (auto written = fs.writeFileAtomic(*path, text); !written) {
        return std::unexpected(std::move(written.error()));
    }
    return true;
}

std::string_view toString(CompileCommandsSource source) noexcept {
    switch (source) {
    case CompileCommandsSource::Existing: return "existing";
    case CompileCommandsSource::Synthesized: return "synthesized";
    case CompileCommandsSource::Generator: return "generator";
    }
    return "existing";
}

Result<std::optional<CompileCommandsLocation>> findCompileCommands(io::IFileSystem& fs, std::string_view root) {
    auto base = io::normalizePath(root);
    if (!base) {
        return std::unexpected(std::move(base.error()));
    }
    std::vector<std::string> candidates{*base};
    if (auto build = io::joinPath(*base, "build")) {
        candidates.push_back(std::move(*build));
    }
    // out/build/<preset>/ (Visual Studio's CMake layout).
    if (auto outBuild =
            io::joinPath(*base, io::kNativePathStyle == io::PathStyle::Windows ? "out\\build" : "out/build")) {
        if (auto entries = fs.listDirectory(*outBuild)) {
            sortByName(*entries);
            for (const io::DirEntry& entry : *entries) {
                if (entry.type == io::FileType::Directory) {
                    if (auto dir = io::joinPath(*outBuild, entry.name)) {
                        candidates.push_back(std::move(*dir));
                    }
                }
            }
        }
    }
    // cmake-build-<profile>/ (CLion's layout).
    if (auto entries = fs.listDirectory(*base)) {
        sortByName(*entries);
        for (const io::DirEntry& entry : *entries) {
            if (entry.type == io::FileType::Directory && entry.name.starts_with("cmake-build-")) {
                if (auto dir = io::joinPath(*base, entry.name)) {
                    candidates.push_back(std::move(*dir));
                }
            }
        }
    }
    for (const std::string& directory : candidates) {
        auto file = io::joinPath(directory, kDatabaseName);
        if (!file) {
            continue;
        }
        auto status = fs.status(*file);
        if (status && status->type == io::FileType::Regular) {
            return std::optional<CompileCommandsLocation>(CompileCommandsLocation{
                .directory = directory, .file = std::move(*file), .source = CompileCommandsSource::Existing});
        }
    }
    return std::optional<CompileCommandsLocation>();
}

Result<std::optional<CompileCommandsLocation>> ExistingCompileCommandsProvider::locate() {
    return findCompileCommands(m_fs, m_root);
}

Result<std::optional<CompileCommandsLocation>> BuildDirectoryCompileCommandsProvider::locate() {
    const std::string directory = m_buildDirectory ? m_buildDirectory() : std::string();
    if (directory.empty()) {
        return std::optional<CompileCommandsLocation>();
    }
    auto base = io::normalizePath(directory);
    if (!base) {
        return std::unexpected(std::move(base.error()));
    }
    auto file = io::joinPath(*base, kDatabaseName);
    if (!file) {
        return std::unexpected(std::move(file.error()));
    }
    auto status = m_fs.status(*file);
    if (!status || status->type != io::FileType::Regular) {
        return std::optional<CompileCommandsLocation>();
    }
    return std::optional<CompileCommandsLocation>(CompileCommandsLocation{
        .directory = *base, .file = std::move(*file), .source = CompileCommandsSource::Existing});
}

Result<std::vector<CompileCommand>> synthesizeCompileCommands(const project::SolutionModel& model,
                                                              const SynthesisOptions& options) {
    if (model.configurations().empty()) {
        return fail(ErrorCode::InvalidArgument, std::format("{} has no solution configurations", model.path()));
    }
    std::size_t configurationIndex = 0;
    if (!options.configuration.empty()) {
        const std::optional<std::size_t> found = model.findConfiguration(options.configuration);
        if (!found) {
            return fail(ErrorCode::NotFound,
                        std::format("{} has no configuration '{}'", model.path(), options.configuration));
        }
        configurationIndex = *found;
    }
    std::vector<CompileCommand> out;
    for (std::size_t p = 0; p < model.projects().size(); ++p) {
        const project::ProjectId id{static_cast<std::uint32_t>(p)};
        const project::ProjectInfo& info = model.project(id);
        if (!info.vcxproj) {
            continue;
        }
        const std::optional<project::ProjectConfigurationMapping>& mapping =
            model.projectConfiguration(id, configurationIndex);
        if (!mapping) {
            continue;
        }
        const project::VcxProject& vcx = *info.vcxproj;
        const std::string configName = mapping->configuration + '|' + mapping->platform;
        const auto metadata = project::clCompileMetadata(vcx, configName);
        std::string characterSet;
        for (const project::ProjectConfiguration& config : vcx.configurations) {
            if (iequals(config.name(), configName)) {
                characterSet = config.characterSet;
            }
        }

        std::vector<std::string> common{options.compiler, "--driver-mode=cl", std::string(targetFor(mapping->platform)),
                                        runtimeFlag(metadataOf(metadata, "runtimelibrary"), mapping->configuration)};
        if (!iequals(metadataOf(metadata, "exceptionhandling"), "false")) {
            common.emplace_back("/EHsc");
        }
        if (iequals(metadataOf(metadata, "conformancemode"), "true")) {
            common.emplace_back("/permissive-");
        }
        if (iequals(characterSet, "Unicode")) {
            common.emplace_back("/DUNICODE");
            common.emplace_back("/D_UNICODE");
        } else if (iequals(characterSet, "MultiByte")) {
            common.emplace_back("/D_MBCS");
        }
        for (const std::string& define : resolvedEntries(metadataOf(metadata, "preprocessordefinitions"))) {
            common.push_back("/D" + define);
        }
        for (const std::string& entry : resolvedEntries(metadataOf(metadata, "additionalincludedirectories"))) {
            if (auto dir = resolvePath(vcx.directory, entry)) {
                common.push_back("/I" + *dir);
            }
        }
        for (const std::string& dir : options.systemIncludeDirectories) {
            common.push_back("/imsvc" + dir);
        }
        for (const std::string& entry : resolvedEntries(metadataOf(metadata, "forcedincludefiles"))) {
            if (auto file = resolvePath(vcx.directory, entry)) {
                common.push_back("/FI" + *file);
            }
        }
        const std::string cxxStd = cxxStandard(metadataOf(metadata, "languagestandard"));
        const std::optional<std::string> cStd = cStandard(metadataOf(metadata, "languagestandard_c"));

        for (const project::ProjectItem& item : vcx.items) {
            if (item.type != project::ItemType::ClCompile) {
                continue;
            }
            CompileCommand command{.directory = vcx.directory, .file = item.path, .arguments = common};
            if (isCSource(item.path)) {
                if (cStd) {
                    command.arguments.push_back(*cStd);
                }
            } else {
                command.arguments.push_back(cxxStd);
            }
            command.arguments.emplace_back("/c");
            command.arguments.push_back(item.path);
            out.push_back(std::move(command));
        }
    }
    return out;
}

SolutionCompileCommandsProvider::SolutionCompileCommandsProvider(io::IFileSystem& fs,
                                                                 std::shared_ptr<const project::SolutionModel> model,
                                                                 std::string cacheDirectory, SynthesisOptions options,
                                                                 std::function<Result<MsvcEnvironment>()> msvc)
    : m_fs(fs), m_model(std::move(model)), m_cacheDirectory(std::move(cacheDirectory)), m_options(std::move(options)),
      m_msvc(std::move(msvc)) {}

Result<std::optional<CompileCommandsLocation>> SolutionCompileCommandsProvider::locate() {
    if (m_options.systemIncludeDirectories.empty() && m_msvc && !m_msvcLooked) {
        m_msvcLooked = true;
        if (auto environment = m_msvc()) {
            m_options.systemIncludeDirectories = std::move(environment->includeDirectories);
        } // Without MSVC the commands still parse code that avoids the Windows headers.
    }
    auto commands = synthesizeCompileCommands(*m_model, m_options);
    if (!commands) {
        return std::unexpected(std::move(commands.error()));
    }
    return writeToCache(m_fs, m_cacheDirectory, *commands, CompileCommandsSource::Synthesized);
}

Result<std::optional<CompileCommandsLocation>> GeneratorCompileCommandsProvider::locate() {
    if (!m_generate) {
        return std::optional<CompileCommandsLocation>();
    }
    auto commands = m_generate();
    if (!commands) {
        return std::unexpected(std::move(commands.error()));
    }
    return writeToCache(m_fs, m_cacheDirectory, *commands, CompileCommandsSource::Generator);
}

Result<std::optional<CompileCommandsLocation>> FirstAvailableCompileCommandsProvider::locate() {
    std::optional<Error> firstError;
    for (const std::shared_ptr<ICompileCommandsProvider>& provider : m_providers) {
        if (!provider) {
            continue;
        }
        auto found = provider->locate();
        if (found && *found) {
            return found;
        }
        if (!found && !firstError) {
            firstError = std::move(found.error());
        }
    }
    if (firstError) {
        return std::unexpected(std::move(*firstError));
    }
    return std::optional<CompileCommandsLocation>();
}

} // namespace frost::lang
