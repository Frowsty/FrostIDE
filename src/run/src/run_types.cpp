// SPDX-License-Identifier: Apache-2.0
//
// The generated run configuration types (see frost/run/run_types.hpp): ids, the CMake and MSBuild
// generators, the CMake program resolution and the native type.

#include "frost/run/run_types.hpp"

#include "frost/io/path.hpp"
#include "frost/project/cmake_file_api.hpp"
#include "frost/project/solution_model.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace frost::run {

namespace {

constexpr std::size_t kMaxIdLength = 128;

bool idCharacter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-';
}

char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](char x, char y) { return lowerAscii(x) == lowerAscii(y); });
}

bool endsWithIgnoringCase(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && equalsIgnoringCase(text.substr(text.size() - suffix.size()), suffix);
}

// The last path component after either separator: artifacts are absolute native paths, but a reply
// read on the other OS keeps its own separators.
std::string_view lastComponent(std::string_view path) noexcept {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

bool hasId(std::span<const RunConfiguration> taken, std::string_view id) {
    return std::ranges::any_of(taken, [&](const RunConfiguration& configuration) { return configuration.id == id; });
}

// The ConfigurationType of `id` in solution configuration `configuration` (run_types.hpp, "MSBuild").
std::string_view configurationTypeOf(const project::SolutionModel& solution, project::ProjectId id,
                                     std::optional<std::size_t> configuration) {
    const project::ProjectInfo& info = solution.project(id);
    if (configuration && *configuration < solution.configurations().size()) {
        if (const auto& mapping = solution.projectConfiguration(id, *configuration)) {
            for (const project::ProjectConfiguration& config : info.vcxproj->configurations) {
                if (equalsIgnoringCase(config.configuration, mapping->configuration) &&
                    equalsIgnoringCase(config.platform, mapping->platform) && !config.configurationType.empty()) {
                    return config.configurationType;
                }
            }
        }
    }
    return info.vcxproj->configurationType;
}

} // namespace

std::string runConfigurationIdFor(std::string_view prefix, std::string_view name) {
    std::string id(prefix);
    id += '.';
    for (const char c : name) {
        id += idCharacter(c) ? c : '_';
    }
    id.resize(std::min(id.size(), kMaxIdLength));
    return id;
}

std::string uniqueRunConfigurationId(std::string_view base, std::span<const RunConfiguration> taken) {
    std::string candidate(base.substr(0, kMaxIdLength));
    for (std::size_t n = 2; hasId(taken, candidate); ++n) {
        const std::string suffix = std::format("-{}", n);
        candidate = std::string(base.substr(0, kMaxIdLength - suffix.size())) + suffix;
    }
    return candidate;
}

std::vector<RunConfiguration> cmakeRunConfigurations(const project::CMakeProject& model,
                                                     std::string_view workingDirectory) {
    std::vector<RunConfiguration> out;
    for (const project::CMakeTarget& target : model.targets) {
        if (target.type != project::CMakeTargetType::Executable) {
            continue;
        }
        RunConfiguration configuration;
        configuration.id = uniqueRunConfigurationId(runConfigurationIdFor(kCMakeIdPrefix, target.name), out);
        configuration.name = target.name;
        configuration.type = RunType::CMakeTarget;
        configuration.target = target.name;
        configuration.workingDirectory = std::string(workingDirectory);
        configuration.beforeLaunch = BeforeLaunch::Build;
        out.push_back(std::move(configuration));
    }
    return out;
}

Result<std::string> cmakeTargetProgram(const project::CMakeTarget& target) {
    if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
        const auto named = [&](auto predicate) { return std::ranges::find_if(target.artifacts, predicate); };
        auto executable =
            named([](const std::string& path) { return endsWithIgnoringCase(lastComponent(path), ".exe"); });
        if (executable == target.artifacts.end()) {
            executable =
                named([](const std::string& path) { return !endsWithIgnoringCase(lastComponent(path), ".pdb"); });
        }
        if (executable != target.artifacts.end()) {
            return *executable;
        }
    } else if (!target.artifacts.empty()) {
        return target.artifacts.front();
    }
    return fail(ErrorCode::NotFound, std::format("the CMake target '{}' has no executable artifact", target.name));
}

std::vector<RunConfiguration> msbuildRunConfigurations(const project::SolutionModel& solution,
                                                       std::optional<std::size_t> configuration) {
    std::vector<RunConfiguration> out;
    const auto projects = solution.projects();
    for (std::uint32_t i = 0; i < projects.size(); ++i) {
        const project::ProjectInfo& info = projects[i];
        // A loaded C++ project, as the startup project combo lists them (build::RunService).
        if (info.nodeKind != project::NodeKind::Project || !info.vcxproj || info.path.empty()) {
            continue;
        }
        if (!equalsIgnoringCase(configurationTypeOf(solution, project::ProjectId{i}, configuration), "Application")) {
            continue;
        }
        RunConfiguration entry;
        entry.id = uniqueRunConfigurationId(runConfigurationIdFor(kMSBuildIdPrefix, info.name), out);
        entry.name = info.name;
        entry.type = RunType::MSBuild;
        entry.target = info.path;
        entry.beforeLaunch = BeforeLaunch::Build;
        out.push_back(std::move(entry));
    }
    return out;
}

RunConfiguration nativeRunConfiguration(std::string_view name, std::string_view program,
                                        std::span<const RunConfiguration> taken) {
    RunConfiguration configuration;
    configuration.id = uniqueRunConfigurationId(runConfigurationIdFor(kNativeIdPrefix, name), taken);
    configuration.name = std::string(name);
    configuration.type = RunType::Native;
    configuration.program = std::string(program);
    return configuration;
}

} // namespace frost::run
