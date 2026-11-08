// SPDX-License-Identifier: Apache-2.0
//
// RunConfiguration (see frost/run/run_configuration.hpp): the type spellings, validation, the JSON
// reader and writer of the per-workspace store, and its load and save.

#include "frost/run/run_configuration.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/rpc/json.hpp"
#include "frost/settings/project_layout.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <utility>

namespace frost::run {

namespace {

constexpr std::size_t kMaxIdLength = 128;
constexpr std::string_view kStoreFileName = "configurations.json";

bool idCharacter(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-';
}

bool hasNul(std::string_view text) noexcept { return text.find('\0') != std::string_view::npos; }

// "run configuration 'name' (id)" for error messages.
std::string describe(const RunConfiguration& configuration) {
    return std::format("run configuration '{}' ({})", configuration.name, configuration.id);
}

std::unexpected<Error> invalid(const RunConfiguration& configuration, std::string_view what) {
    return fail(ErrorCode::InvalidArgument, std::format("{}: {}", describe(configuration), what));
}

// ---- The writer: indented JSON, members in a fixed order (scalars through rpc::Json::dump).

void indent(std::string& out, int depth) { out.append(static_cast<std::size_t>(depth) * 2, ' '); }

void writeStringArray(std::string& out, const std::vector<std::string>& items) {
    rpc::Json array = rpc::Json::array();
    for (const std::string& item : items) {
        array.push(item);
    }
    out += array.dump();
}

void writeConfiguration(std::string& out, const RunConfiguration& configuration, int depth) {
    const auto member = [&](std::string_view key, const std::string& valueText, bool last = false) {
        indent(out, depth + 1);
        out += rpc::Json(key).dump();
        out += ": ";
        out += valueText;
        out += last ? "\n" : ",\n";
    };
    indent(out, depth);
    out += "{\n";
    member("id", rpc::Json(configuration.id).dump());
    member("name", rpc::Json(configuration.name).dump());
    member("type", rpc::Json(toString(configuration.type)).dump());
    member("target", rpc::Json(configuration.target).dump());
    member("program", rpc::Json(configuration.program).dump());
    std::string arguments;
    writeStringArray(arguments, configuration.arguments);
    member("arguments", arguments);
    member("workingDirectory", rpc::Json(configuration.workingDirectory).dump());
    rpc::Json environment = rpc::Json::array();
    for (const EnvironmentEntry& entry : configuration.environment) {
        rpc::Json item = rpc::Json::object().set("name", entry.name);
        if (entry.value) {
            item.set("value", *entry.value);
        } else {
            item.set("unset", true);
        }
        environment.push(std::move(item));
    }
    member("environment", environment.dump());
    member("beforeLaunch", rpc::Json(toString(configuration.beforeLaunch)).dump(), true);
    indent(out, depth);
    out += "}";
}

// ---- The reader.

std::unexpected<Error> corrupt(std::size_t index, std::string_view what) {
    return fail(ErrorCode::Corrupt, std::format("run configuration #{}: {}", index + 1, what));
}

// A string member: required ones must be present; optional ones default to "".
Result<std::string> stringMember(const rpc::Json& object, std::string_view key, bool required, std::size_t index) {
    const rpc::Json* value = object.find(key);
    if (value == nullptr) {
        if (required) {
            return corrupt(index, std::format("'{}' is missing", key));
        }
        return std::string();
    }
    const std::string* text = value->asString();
    if (text == nullptr) {
        return corrupt(index, std::format("'{}' is not a string", key));
    }
    return *text;
}

Result<RunConfiguration> readConfiguration(const rpc::Json& object, std::size_t index) {
    if (!object.isObject()) {
        return corrupt(index, "not an object");
    }
    RunConfiguration configuration;
    auto id = stringMember(object, "id", true, index);
    if (!id) {
        return std::unexpected(std::move(id.error()));
    }
    configuration.id = std::move(*id);
    auto name = stringMember(object, "name", true, index);
    if (!name) {
        return std::unexpected(std::move(name.error()));
    }
    configuration.name = std::move(*name);
    auto type = stringMember(object, "type", true, index);
    if (!type) {
        return std::unexpected(std::move(type.error()));
    }
    const auto parsedType = parseRunType(*type);
    if (!parsedType) {
        return corrupt(index, std::format("unknown type '{}'", *type));
    }
    configuration.type = *parsedType;
    for (auto [key, field] : {std::pair{"target", &configuration.target}, std::pair{"program", &configuration.program},
                              std::pair{"workingDirectory", &configuration.workingDirectory}}) {
        auto value = stringMember(object, key, false, index);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        *field = std::move(*value);
    }
    if (const rpc::Json* arguments = object.find("arguments"); arguments != nullptr) {
        if (!arguments->isArray()) {
            return corrupt(index, "'arguments' is not an array");
        }
        for (const rpc::Json& argument : arguments->items()) {
            const std::string* text = argument.asString();
            if (text == nullptr) {
                return corrupt(index, "an argument is not a string");
            }
            configuration.arguments.push_back(*text);
        }
    }
    if (const rpc::Json* environment = object.find("environment"); environment != nullptr) {
        if (!environment->isArray()) {
            return corrupt(index, "'environment' is not an array");
        }
        for (const rpc::Json& item : environment->items()) {
            if (!item.isObject()) {
                return corrupt(index, "an environment entry is not an object");
            }
            EnvironmentEntry entry;
            auto entryName = stringMember(item, "name", true, index);
            if (!entryName) {
                return std::unexpected(std::move(entryName.error()));
            }
            entry.name = std::move(*entryName);
            const rpc::Json* value = item.find("value");
            const rpc::Json* unset = item.find("unset");
            if (unset != nullptr && !unset->isBool()) {
                return corrupt(index, std::format("environment entry '{}': 'unset' is not a boolean", entry.name));
            }
            const bool removes = unset != nullptr && unset->asBool().value_or(false);
            if (value != nullptr && removes) {
                return corrupt(index, std::format("environment entry '{}' has both a value and unset", entry.name));
            }
            if (value != nullptr) {
                const std::string* text = value->asString();
                if (text == nullptr) {
                    return corrupt(index, std::format("environment entry '{}': 'value' is not a string", entry.name));
                }
                entry.value = *text;
            } else if (!removes) {
                return corrupt(index, std::format("environment entry '{}' has neither a value nor unset", entry.name));
            }
            configuration.environment.push_back(std::move(entry));
        }
    }
    auto step = stringMember(object, "beforeLaunch", false, index);
    if (!step) {
        return std::unexpected(std::move(step.error()));
    }
    if (!step->empty()) {
        const auto parsedStep = parseBeforeLaunch(*step);
        if (!parsedStep) {
            return corrupt(index, std::format("unknown beforeLaunch '{}'", *step));
        }
        configuration.beforeLaunch = *parsedStep;
    }
    if (auto valid = validateRunConfiguration(configuration); !valid) {
        return corrupt(index, valid.error().message());
    }
    return configuration;
}

// What a save requires: every configuration valid, ids unique.
Result<void> checkForSave(std::span<const RunConfiguration> configurations) {
    std::set<std::string, std::less<>> ids;
    for (const RunConfiguration& configuration : configurations) {
        if (auto valid = validateRunConfiguration(configuration); !valid) {
            return std::unexpected(std::move(valid.error()));
        }
        if (!ids.insert(configuration.id).second) {
            return fail(ErrorCode::InvalidArgument,
                        std::format("two run configurations share the id '{}'", configuration.id));
        }
    }
    return {};
}

} // namespace

std::string_view toString(RunType type) noexcept {
    switch (type) {
    case RunType::Native: return "native";
    case RunType::CMakeTarget: return "cmake-target";
    case RunType::MSBuild: return "msbuild";
    }
    return "native";
}

std::optional<RunType> parseRunType(std::string_view text) noexcept {
    for (const RunType type : {RunType::Native, RunType::CMakeTarget, RunType::MSBuild}) {
        if (toString(type) == text) {
            return type;
        }
    }
    return std::nullopt;
}

std::string_view toString(BeforeLaunch step) noexcept { return step == BeforeLaunch::Build ? "build" : "none"; }

std::optional<BeforeLaunch> parseBeforeLaunch(std::string_view text) noexcept {
    if (text == "none") {
        return BeforeLaunch::None;
    }
    if (text == "build") {
        return BeforeLaunch::Build;
    }
    return std::nullopt;
}

Result<void> validateRunConfiguration(const RunConfiguration& configuration) {
    if (configuration.id.empty() || configuration.id.size() > kMaxIdLength ||
        !std::ranges::all_of(configuration.id, idCharacter)) {
        return invalid(configuration, "the id must be 1 to 128 of the characters A-Z, a-z, 0-9, '.', '_' and '-'");
    }
    if (configuration.name.empty() || hasNul(configuration.name)) {
        return invalid(configuration, "the name is empty or contains NUL");
    }
    switch (configuration.type) {
    case RunType::Native:
        if (configuration.program.empty()) {
            return invalid(configuration, "a native configuration needs a program");
        }
        if (!configuration.target.empty()) {
            return invalid(configuration, "a native configuration has no target");
        }
        if (configuration.beforeLaunch == BeforeLaunch::Build) {
            return invalid(configuration, "a native configuration has nothing to build");
        }
        break;
    case RunType::CMakeTarget:
        if (configuration.target.empty() || hasNul(configuration.target)) {
            return invalid(configuration, "a cmake-target configuration needs a target name");
        }
        break;
    case RunType::MSBuild:
        if (!io::normalizePath(configuration.target)) {
            return invalid(configuration, "an msbuild configuration needs the absolute path of its project file");
        }
        break;
    }
    if (!configuration.program.empty() && !io::normalizePath(configuration.program)) {
        return invalid(configuration, std::format("the program '{}' is not an absolute path", configuration.program));
    }
    if (!configuration.workingDirectory.empty() && !io::normalizePath(configuration.workingDirectory)) {
        return invalid(configuration, std::format("the working directory '{}' is not an absolute path",
                                                  configuration.workingDirectory));
    }
    if (std::ranges::any_of(configuration.arguments, hasNul)) {
        return invalid(configuration, "an argument contains NUL");
    }
    for (const EnvironmentEntry& entry : configuration.environment) {
        if (entry.name.empty() || entry.name.find('=') != std::string::npos || hasNul(entry.name)) {
            return invalid(configuration,
                           std::format("the environment name '{}' is empty or holds '=' or NUL", entry.name));
        }
        if (entry.value && hasNul(*entry.value)) {
            return invalid(configuration, std::format("the environment value of '{}' contains NUL", entry.name));
        }
    }
    return {};
}

Result<std::vector<RunConfiguration>> parseRunConfigurations(std::string_view json) {
    if (json.starts_with("\xEF\xBB\xBF")) {
        json.remove_prefix(3);
    }
    auto document = rpc::parseJson(json);
    if (!document) {
        return fail(ErrorCode::Corrupt, std::format("run configurations: {}", document.error().message()));
    }
    if (!document->isObject()) {
        return fail(ErrorCode::Corrupt, "run configurations: the file is not a JSON object");
    }
    const auto version = document->integerAt("version");
    if (!version) {
        return fail(ErrorCode::Corrupt, "run configurations: 'version' is missing or not an integer");
    }
    if (*version > kRunConfigurationsVersion) {
        return fail(ErrorCode::Unsupported,
                    std::format("run configurations: version {} is newer than this FrostIDE reads ({})", *version,
                                kRunConfigurationsVersion));
    }
    if (*version < 1) {
        return fail(ErrorCode::Corrupt, std::format("run configurations: version {} is not valid", *version));
    }
    std::vector<RunConfiguration> configurations;
    const rpc::Json* list = document->find("configurations");
    if (list == nullptr) {
        return configurations;
    }
    if (!list->isArray()) {
        return fail(ErrorCode::Corrupt, "run configurations: 'configurations' is not an array");
    }
    std::set<std::string, std::less<>> ids;
    for (std::size_t index = 0; index < list->size(); ++index) {
        auto configuration = readConfiguration(list->items()[index], index);
        if (!configuration) {
            return std::unexpected(std::move(configuration.error()));
        }
        if (!ids.insert(configuration->id).second) {
            return corrupt(index, std::format("the id '{}' is used twice", configuration->id));
        }
        configurations.push_back(std::move(*configuration));
    }
    return configurations;
}

std::string serializeRunConfigurations(std::span<const RunConfiguration> configurations) {
    std::string out = std::format("{{\n  \"version\": {},\n  \"configurations\": [", kRunConfigurationsVersion);
    if (configurations.empty()) {
        out += "]\n}\n";
        return out;
    }
    out += "\n";
    for (std::size_t i = 0; i < configurations.size(); ++i) {
        writeConfiguration(out, configurations[i], 2);
        out += i + 1 < configurations.size() ? ",\n" : "\n";
    }
    out += "  ]\n}\n";
    return out;
}

Result<std::string> workspaceRunConfigurationsPath(std::string_view workspaceRoot) {
    auto layout = settings::ProjectLayout::forRoot(workspaceRoot);
    if (!layout) {
        return std::unexpected(std::move(layout.error()));
    }
    return io::joinPath(layout->runConfigurations, kStoreFileName);
}

Result<std::vector<RunConfiguration>> loadRunConfigurations(io::IFileSystem& fs, std::string_view path) {
    auto text = fs.readFile(path);
    if (!text) {
        if (text.error().code() == ErrorCode::NotFound) {
            return std::vector<RunConfiguration>{};
        }
        return std::unexpected(std::move(text.error().withContext(std::format("reading {}", path))));
    }
    auto parsed = parseRunConfigurations(*text);
    if (!parsed) {
        return std::unexpected(std::move(parsed.error().withContext(std::format("reading {}", path))));
    }
    return parsed;
}

Result<void> saveRunConfigurations(io::IFileSystem& fs, std::string_view path,
                                   std::span<const RunConfiguration> configurations) {
    if (auto checked = checkForSave(configurations); !checked) {
        return checked;
    }
    auto normalized = io::normalizePath(path);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    if (auto created = fs.createDirectories(io::parentPath(*normalized)); !created) {
        return std::unexpected(std::move(created.error()));
    }
    return fs.writeFileAtomic(*normalized, serializeRunConfigurations(configurations));
}

Result<void> saveWorkspaceRunConfigurations(io::IFileSystem& fs, std::string_view workspaceRoot,
                                            std::span<const RunConfiguration> configurations) {
    auto layout = settings::ProjectLayout::forRoot(workspaceRoot);
    if (!layout) {
        return std::unexpected(std::move(layout.error()));
    }
    // Checked before .frost/ is created, so a refused save leaves nothing behind.
    if (auto checked = checkForSave(configurations); !checked) {
        return checked;
    }
    if (auto ensured = settings::ensureProjectLayout(fs, *layout); !ensured) {
        return std::unexpected(std::move(ensured.error()));
    }
    auto path = io::joinPath(layout->runConfigurations, kStoreFileName);
    if (!path) {
        return std::unexpected(std::move(path.error()));
    }
    return saveRunConfigurations(fs, *path, configurations);
}

} // namespace frost::run
