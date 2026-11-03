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

#include "msvc_flags.hpp"

#include <algorithm>
#include <format>
#include <string_view>

namespace frost::lang {

Result<std::optional<CompileCommandsLocation>> ICompileCommandsProvider::locate(std::stop_token stop) {
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "compile commands lookup cancelled");
    auto result = locate();
    if (stop.stop_requested())
        return fail(ErrorCode::Cancelled, "compile commands lookup cancelled");
    return result;
}

namespace {

constexpr std::string_view kDatabaseName = "compile_commands.json";

using namespace detail; // msvc_flags.hpp

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
    case CompileCommandsSource::Evaluated: return "evaluated";
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

Result<std::size_t> resolveConfigurationIndex(const project::SolutionModel& model, std::string_view configuration) {
    if (model.configurations().empty()) {
        return fail(ErrorCode::InvalidArgument, std::format("{} has no solution configurations", model.path()));
    }
    if (configuration.empty()) {
        return std::size_t{0};
    }
    const std::optional<std::size_t> found = model.findConfiguration(configuration);
    if (!found) {
        return fail(ErrorCode::NotFound, std::format("{} has no configuration '{}'", model.path(), configuration));
    }
    return *found;
}

std::vector<CompileCommand> synthesizeProjectCompileCommands(const project::SolutionModel& model, project::ProjectId id,
                                                             std::size_t configurationIndex,
                                                             const SynthesisOptions& options) {
    std::vector<CompileCommand> out;
    const project::ProjectInfo& info = model.project(id);
    if (!info.vcxproj) {
        return out;
    }
    const std::optional<project::ProjectConfigurationMapping>& mapping =
        model.projectConfiguration(id, configurationIndex);
    if (!mapping) {
        return out;
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
    return out;
}

Result<std::vector<CompileCommand>> synthesizeCompileCommands(const project::SolutionModel& model,
                                                              const SynthesisOptions& options) {
    auto configurationIndex = resolveConfigurationIndex(model, options.configuration);
    if (!configurationIndex) {
        return std::unexpected(std::move(configurationIndex.error()));
    }
    std::vector<CompileCommand> out;
    for (std::size_t p = 0; p < model.projects().size(); ++p) {
        std::vector<CompileCommand> project = synthesizeProjectCompileCommands(
            model, project::ProjectId{static_cast<std::uint32_t>(p)}, *configurationIndex, options);
        out.insert(out.end(), std::make_move_iterator(project.begin()), std::make_move_iterator(project.end()));
    }
    return out;
}

Result<std::optional<CompileCommandsLocation>> writeCompileCommandsLocation(io::IFileSystem& fs,
                                                                            std::string_view cacheDirectory,
                                                                            std::span<const CompileCommand> commands,
                                                                            CompileCommandsSource source) {
    return writeToCache(fs, cacheDirectory, commands, source);
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

Result<std::optional<CompileCommandsLocation>> FirstAvailableCompileCommandsProvider::locate() { return locate({}); }

Result<std::optional<CompileCommandsLocation>> FirstAvailableCompileCommandsProvider::locate(std::stop_token stop) {
    std::optional<Error> firstError;
    for (const std::shared_ptr<ICompileCommandsProvider>& provider : m_providers) {
        if (stop.stop_requested())
            return fail(ErrorCode::Cancelled, "compile commands lookup cancelled");
        if (!provider) {
            continue;
        }
        auto found = provider->locate(stop);
        if (stop.stop_requested() || (!found && found.error().code() == ErrorCode::Cancelled))
            return fail(ErrorCode::Cancelled, "compile commands lookup cancelled");
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
