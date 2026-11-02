// SPDX-License-Identifier: Apache-2.0
//
// CMake profiles (frost/project/cmake_profile.hpp): defaults, presets and settings records turned
// into profiles, the build-directory trust rule and the cmake command lines.

#include "frost/project/cmake_profile.hpp"

#include "frost/io/path.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <vector>

namespace frost::project {

namespace {

constexpr char lowerAscii(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), lowerAscii);
    return out;
}

// <sourceDir>/cmake-build-<name lower-cased, spaces as '-'>, CLion's layout.
std::string defaultBuildDirectory(std::string_view sourceDir, std::string_view name) {
    std::string leaf = "cmake-build-" + lowered(name);
    std::ranges::replace(leaf, ' ', '-');
    auto joined = io::joinPath(sourceDir, leaf);
    return joined ? *joined : std::string(sourceDir);
}

// `path` made absolute against `base` and normalised; "" when invalid.
std::string absoluteAgainst(std::string_view base, std::string_view path) {
    std::string joined;
    if (io::isAbsolutePath(path)) {
        joined = std::string(path);
    } else {
        auto relative = io::joinPath(base, path);
        if (!relative) {
            return {};
        }
        joined = std::move(*relative);
    }
    return collapseCMakePath(joined);
}

// "NAME[:TYPE]=VALUE" as a cache variable; nullopt without '=' or a name.
std::optional<CMakeCacheVariable> parseCacheVariable(std::string_view text) {
    const std::size_t equals = text.find('=');
    if (equals == std::string_view::npos || equals == 0) {
        return std::nullopt;
    }
    std::string_view name = text.substr(0, equals);
    std::string type;
    if (const std::size_t colon = name.find(':'); colon != std::string_view::npos) {
        type = std::string(name.substr(colon + 1));
        name = name.substr(0, colon);
    }
    if (name.empty()) {
        return std::nullopt;
    }
    return CMakeCacheVariable{
        .name = std::string(name), .type = std::move(type), .value = std::string(text.substr(equals + 1))};
}

std::string defineSwitch(std::string_view name, std::string_view type, std::string_view value) {
    return type.empty() ? std::format("-D{}={}", name, value) : std::format("-D{}:{}={}", name, type, value);
}

bool hasCacheVariable(const CMakeProfile& profile, std::string_view name) {
    return std::ranges::any_of(profile.cacheVariables, [&](const CMakeCacheVariable& v) { return v.name == name; });
}

} // namespace

std::string_view toString(CMakeProfileSource source) noexcept {
    switch (source) {
    case CMakeProfileSource::Default: return "default";
    case CMakeProfileSource::ProjectSettings: return "project settings";
    case CMakeProfileSource::ApplicationSettings: return "application settings";
    case CMakeProfileSource::Preset: return "preset";
    }
    return "default";
}

std::string CMakeProfile::effectiveConfiguration() const {
    if (!buildConfiguration.empty()) {
        return buildConfiguration;
    }
    return buildType.empty() ? std::string("Debug") : buildType;
}

bool isMultiConfigGenerator(std::string_view generator) noexcept {
    return generator == "Ninja Multi-Config" || generator.starts_with("Visual Studio") || generator == "Xcode";
}

bool isNinjaGenerator(std::string_view generator) noexcept { return generator.starts_with("Ninja"); }

bool isVisualStudioGenerator(std::string_view generator) noexcept { return generator.starts_with("Visual Studio"); }

std::string collapseCMakePath(std::string_view path) {
    auto normalized = io::normalizePath(path);
    if (!normalized || !io::isAbsolutePath(*normalized)) {
        return {};
    }
    if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
        return *normalized; // normalizePath already resolves ".." lexically there.
    } else {
        std::vector<std::string_view> parts;
        std::string_view rest = std::string_view(*normalized).substr(1);
        while (!rest.empty()) {
            const std::size_t slash = rest.find('/');
            const std::string_view part = rest.substr(0, slash);
            rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
            if (part == "..") {
                if (!parts.empty()) {
                    parts.pop_back();
                }
            } else if (!part.empty() && part != ".") {
                parts.push_back(part);
            }
        }
        std::string out;
        for (const std::string_view part : parts) {
            out += '/';
            out += part;
        }
        return out.empty() ? std::string("/") : out;
    }
}

bool isPathWithin(std::string_view root, std::string_view path) {
    std::string base = collapseCMakePath(root);
    std::string candidate = collapseCMakePath(path);
    if (base.empty() || candidate.empty()) {
        return false;
    }
    if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
        base = lowered(base);
        candidate = lowered(candidate);
    }
    if (candidate == base) {
        return true;
    }
    const char separator = io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';
    if (!base.empty() && base.back() != separator) {
        base += separator;
    }
    return candidate.starts_with(base);
}

CMakeProfile defaultCMakeProfile(std::string_view sourceDir) {
    CMakeProfile profile;
    profile.name = "Debug";
    profile.displayName = "Debug";
    profile.source = CMakeProfileSource::Default;
    profile.buildType = "Debug";
    profile.generator = "Ninja";
    profile.buildDirectory = defaultBuildDirectory(sourceDir, "Debug");
    return profile;
}

CMakeProfile cmakeProfileFromPreset(std::string_view sourceDir, const CMakeConfigurePreset& preset,
                                    const CMakeBuildPreset* buildPreset) {
    CMakeProfile profile;
    profile.name = preset.name;
    profile.displayName = preset.label();
    profile.source = CMakeProfileSource::Preset;
    profile.generator = preset.generator;
    profile.buildDirectory =
        preset.binaryDir.empty() ? defaultBuildDirectory(sourceDir, preset.name) : preset.binaryDir;
    profile.toolchainFile = preset.toolchainFile;
    profile.installDir = preset.installDir;
    profile.toolset = preset.toolset;
    profile.architecture = preset.architecture;
    profile.architectureExternal = preset.architectureExternal;
    profile.environment = preset.environment;
    profile.configurePreset = preset.name;
    for (const CMakeCacheVariable& variable : preset.cacheVariables) {
        if (variable.name == "CMAKE_BUILD_TYPE") {
            profile.buildType = variable.value;
        } else {
            profile.cacheVariables.push_back(variable);
        }
    }
    if (buildPreset != nullptr) {
        profile.buildPreset = buildPreset->name;
        profile.buildConfiguration = buildPreset->configuration;
        profile.buildTargets = buildPreset->targets;
        profile.buildJobs = buildPreset->jobs.value_or(0);
        profile.buildToolOptions = buildPreset->nativeToolOptions;
        profile.buildEnvironment = buildPreset->environment;
    }
    // The trust rule: presets are repository files, so their build directory stays in the folder.
    if (!isPathWithin(sourceDir, profile.buildDirectory)) {
        profile.problem = std::format("the preset's build directory {} is outside the project folder; choose a "
                                      "build directory outside the folder in an application-scope CMake profile",
                                      profile.buildDirectory);
    }
    return profile;
}

std::vector<CMakeProfile> cmakeProfilesFromRecords(std::string_view sourceDir,
                                                   std::span<const CMakeProfileRecord> records,
                                                   CMakeProfileSource source) {
    std::vector<CMakeProfile> profiles;
    for (const CMakeProfileRecord& record : records) {
        if (record.name.empty() ||
            std::ranges::any_of(profiles, [&](const CMakeProfile& p) { return p.name == record.name; })) {
            continue;
        }
        const bool forThisRoot =
            !record.root.empty() && isPathWithin(record.root, sourceDir) && isPathWithin(sourceDir, record.root);
        if (!record.root.empty() && !forThisRoot) {
            continue; // A record for another folder.
        }
        CMakeProfile profile;
        profile.name = record.name;
        profile.displayName = record.name;
        profile.source = source;
        profile.buildType = record.buildType.empty() ? std::string("Debug") : record.buildType;
        profile.generator = record.generator.empty() ? std::string("Ninja") : record.generator;
        profile.buildDirectory = record.buildDirectory.empty() ? defaultBuildDirectory(sourceDir, record.name)
                                                               : absoluteAgainst(sourceDir, record.buildDirectory);
        for (const std::string& text : record.cacheVariables) {
            if (auto variable = parseCacheVariable(text)) {
                if (variable->name == "CMAKE_BUILD_TYPE") {
                    profile.buildType = variable->value;
                } else {
                    profile.cacheVariables.push_back(std::move(*variable));
                }
            }
        }
        for (const std::string& text : record.environment) {
            const std::size_t equals = text.find('=');
            if (equals == 0 || text.empty()) {
                continue;
            }
            if (equals == std::string::npos) {
                profile.environment.emplace_back(text, std::nullopt);
            } else {
                profile.environment.emplace_back(text.substr(0, equals), text.substr(equals + 1));
            }
        }
        profile.buildToolOptions = record.buildOptions;
        profile.buildJobs = record.jobs;
        if (profile.buildDirectory.empty()) {
            profile.problem = std::format("the build directory '{}' is not a valid path", record.buildDirectory);
        } else if (!isPathWithin(sourceDir, profile.buildDirectory) &&
                   !(source == CMakeProfileSource::ApplicationSettings && forThisRoot)) {
            profile.problem = std::format("the build directory {} is outside the project folder; only an "
                                          "application-scope profile whose root is this folder may use it",
                                          profile.buildDirectory);
        }
        profiles.push_back(std::move(profile));
    }
    return profiles;
}

std::vector<CMakeProfile> cmakeProfilesFor(std::string_view sourceDir, const std::optional<CMakePresets>& presets,
                                           std::span<const CMakeProfileRecord> records,
                                           CMakeProfileSource recordSource) {
    std::vector<CMakeProfile> profiles;
    if (presets) {
        for (const CMakeConfigurePreset& preset : presets->configurePresets) {
            if (preset.hidden || !preset.enabled) {
                continue;
            }
            const auto builds = presets->buildPresetsFor(preset.name);
            profiles.push_back(cmakeProfileFromPreset(sourceDir, preset, builds.empty() ? nullptr : builds.front()));
        }
    }
    if (profiles.empty()) {
        profiles = cmakeProfilesFromRecords(sourceDir, records, recordSource);
    }
    if (profiles.empty()) {
        profiles.push_back(defaultCMakeProfile(sourceDir));
    }
    return profiles;
}

Result<std::vector<std::string>> cmakeConfigureArguments(const CMakeProfile& profile, std::string_view sourceDir,
                                                         std::string_view makeProgram) {
    if (!profile.usable()) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("profile {} cannot be used: {}", profile.name, profile.problem));
    }
    if (!io::isAbsolutePath(sourceDir) || !io::isAbsolutePath(profile.buildDirectory)) {
        return fail(ErrorCode::InvalidArgument, "the source and build directories must be absolute");
    }
    std::vector<std::string> arguments{"-S", std::string(sourceDir), "-B", profile.buildDirectory};
    if (!profile.generator.empty()) {
        arguments.insert(arguments.end(), {"-G", profile.generator});
    }
    if (!profile.toolset.empty()) {
        arguments.insert(arguments.end(), {"-T", profile.toolset});
    }
    if (!profile.architecture.empty() && !profile.architectureExternal) {
        arguments.insert(arguments.end(), {"-A", profile.architecture});
    }
    if (!profile.buildType.empty() && !hasCacheVariable(profile, "CMAKE_BUILD_TYPE")) {
        arguments.push_back(defineSwitch("CMAKE_BUILD_TYPE", "", profile.buildType));
    }
    if (!profile.toolchainFile.empty()) {
        arguments.push_back(defineSwitch("CMAKE_TOOLCHAIN_FILE", "FILEPATH", profile.toolchainFile));
    }
    if (!profile.installDir.empty()) {
        arguments.push_back(defineSwitch("CMAKE_INSTALL_PREFIX", "PATH", profile.installDir));
    }
    if (!makeProgram.empty() && isNinjaGenerator(profile.generator) &&
        !hasCacheVariable(profile, "CMAKE_MAKE_PROGRAM")) {
        arguments.push_back(defineSwitch("CMAKE_MAKE_PROGRAM", "FILEPATH", makeProgram));
    }
    for (const CMakeCacheVariable& variable : profile.cacheVariables) {
        if (variable.name == "CMAKE_EXPORT_COMPILE_COMMANDS") {
            continue; // Always ON (below).
        }
        arguments.push_back(defineSwitch(variable.name, variable.type, variable.value));
    }
    arguments.push_back("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");
    return arguments;
}

std::vector<std::string> cmakeBuildArguments(const CMakeProfile& profile, std::span<const std::string> targets,
                                             bool clean, int jobs) {
    std::vector<std::string> arguments{"--build", profile.buildDirectory};
    if (isMultiConfigGenerator(profile.generator)) {
        arguments.insert(arguments.end(), {"--config", profile.effectiveConfiguration()});
    }
    if (clean) {
        arguments.insert(arguments.end(), {"--target", "clean"});
    } else {
        const std::span<const std::string> chosen =
            targets.empty() ? std::span<const std::string>(profile.buildTargets) : targets;
        if (!chosen.empty()) {
            arguments.push_back("--target");
            arguments.insert(arguments.end(), chosen.begin(), chosen.end());
        }
    }
    const int effectiveJobs = jobs > 0 ? jobs : profile.buildJobs;
    if (effectiveJobs > 0) {
        arguments.insert(arguments.end(), {"-j", std::to_string(effectiveJobs)});
    }
    if (!clean && !profile.buildToolOptions.empty()) {
        arguments.push_back("--");
        arguments.insert(arguments.end(), profile.buildToolOptions.begin(), profile.buildToolOptions.end());
    }
    return arguments;
}

} // namespace frost::project
