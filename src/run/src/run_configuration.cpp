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
    if (isFrostEngineRunType(configuration.type)) {
        member("engineProjectPath", rpc::Json(configuration.engineProjectPath).dump());
        if (configuration.engineDebugMode != EngineDebugMode::Lua)
            member("engineDebugMode", rpc::Json(toString(configuration.engineDebugMode)).dump());
    }
    if (isLuaRunType(configuration.type)) {
        std::string interpreterArguments;
        writeStringArray(interpreterArguments, configuration.interpreterArguments);
        member("interpreterArguments", interpreterArguments);
    }
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
    const bool tasks = !configuration.beforeLaunchTasks.empty();
    const bool members = !configuration.configurations.empty();
    member("beforeLaunch", rpc::Json(toString(configuration.beforeLaunch)).dump(), !tasks && !members);
    if (tasks) {
        rpc::Json list = rpc::Json::array();
        for (const BeforeLaunchTask& task : configuration.beforeLaunchTasks) {
            rpc::Json taskArguments = rpc::Json::array();
            for (const std::string& argument : task.arguments) {
                taskArguments.push(argument);
            }
            list.push(rpc::Json::object()
                          .set("program", task.program)
                          .set("arguments", std::move(taskArguments))
                          .set("workingDirectory", task.workingDirectory));
        }
        member("beforeLaunchTasks", list.dump(), !members);
    }
    if (members) {
        std::string ids;
        writeStringArray(ids, configuration.configurations);
        member("configurations", ids, true);
    }
    indent(out, depth);
    out += '}';
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

// An optional array of strings; absent is empty.
Result<std::vector<std::string>> stringArrayMember(const rpc::Json& object, std::string_view key, std::size_t index) {
    std::vector<std::string> out;
    const rpc::Json* value = object.find(key);
    if (value == nullptr) {
        return out;
    }
    if (!value->isArray()) {
        return corrupt(index, std::format("'{}' is not an array", key));
    }
    for (const rpc::Json& item : value->items()) {
        const std::string* text = item.asString();
        if (text == nullptr) {
            return corrupt(index, std::format("an entry of '{}' is not a string", key));
        }
        out.push_back(*text);
    }
    return out;
}

Result<std::vector<BeforeLaunchTask>> readTasks(const rpc::Json& object, std::size_t index) {
    std::vector<BeforeLaunchTask> tasks;
    const rpc::Json* list = object.find("beforeLaunchTasks");
    if (list == nullptr) {
        return tasks;
    }
    if (!list->isArray()) {
        return corrupt(index, "'beforeLaunchTasks' is not an array");
    }
    for (const rpc::Json& item : list->items()) {
        if (!item.isObject()) {
            return corrupt(index, "a before-launch task is not an object");
        }
        BeforeLaunchTask task;
        auto program = stringMember(item, "program", true, index);
        if (!program) {
            return std::unexpected(std::move(program.error()));
        }
        task.program = std::move(*program);
        auto arguments = stringArrayMember(item, "arguments", index);
        if (!arguments) {
            return std::unexpected(std::move(arguments.error()));
        }
        task.arguments = std::move(*arguments);
        auto directory = stringMember(item, "workingDirectory", false, index);
        if (!directory) {
            return std::unexpected(std::move(directory.error()));
        }
        task.workingDirectory = std::move(*directory);
        tasks.push_back(std::move(task));
    }
    return tasks;
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
                              std::pair{"workingDirectory", &configuration.workingDirectory},
                              std::pair{"engineProjectPath", &configuration.engineProjectPath}}) {
        auto value = stringMember(object, key, false, index);
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        *field = std::move(*value);
    }
    if (const auto* flags = object.find("interpreterArguments")) {
        if (!flags->isArray())
            return corrupt(index, "'interpreterArguments' is not an array");
        for (const auto& flag : flags->items()) {
            const auto* text = flag.asString();
            if (!text)
                return corrupt(index, "an interpreter argument is not a string");
            configuration.interpreterArguments.push_back(*text);
        }
    }
    if (const auto* mode = object.find("engineDebugMode")) {
        if (!mode->isString())
            return corrupt(index, "'engineDebugMode' is not a string");
        const auto parsed = parseEngineDebugMode(*mode->asString());
        if (!parsed)
            return corrupt(index, "unknown engine Debug mode");
        configuration.engineDebugMode = *parsed;
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
    auto tasks = readTasks(object, index);
    if (!tasks) {
        return std::unexpected(std::move(tasks.error()));
    }
    configuration.beforeLaunchTasks = std::move(*tasks);
    auto members = stringArrayMember(object, "configurations", index);
    if (!members) {
        return std::unexpected(std::move(members.error()));
    }
    configuration.configurations = std::move(*members);
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
    case RunType::LuaScript: return "lua-script";
    case RunType::LuaRepl: return "lua-repl";
    case RunType::CMakeTarget: return "cmake-target";
    case RunType::MSBuild: return "msbuild";
    case RunType::Compound: return "compound";
    case RunType::Attach: return "attach";
    case RunType::FrostEditor: return "frost-editor";
    case RunType::FrostPlayer: return "frost-player";
    }
    return "native";
}

std::optional<RunType> parseRunType(std::string_view text) noexcept {
    for (const RunType type :
         {RunType::Native, RunType::CMakeTarget, RunType::MSBuild, RunType::Compound, RunType::Attach,
          RunType::FrostEditor, RunType::FrostPlayer, RunType::LuaScript, RunType::LuaRepl}) {
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

std::string_view toString(EngineDebugMode mode) noexcept {
    switch (mode) {
    case EngineDebugMode::Lua: return "lua";
    case EngineDebugMode::NativeAndLua: return "native-and-lua";
    }
    return "unknown";
}
std::optional<EngineDebugMode> parseEngineDebugMode(std::string_view text) noexcept {
    if (text == "lua")
        return EngineDebugMode::Lua;
    if (text == "native-and-lua")
        return EngineDebugMode::NativeAndLua;
    return std::nullopt;
}

bool isLuaRunType(RunType type) noexcept { return type == RunType::LuaScript || type == RunType::LuaRepl; }

Result<void> validateRunConfiguration(const RunConfiguration& configuration) {
    if (configuration.id.empty() || configuration.id.size() > kMaxIdLength ||
        !std::ranges::all_of(configuration.id, idCharacter)) {
        return invalid(configuration, "the id must be 1 to 128 of the characters A-Z, a-z, 0-9, '.', '_' and '-'");
    }
    if (configuration.name.empty() || hasNul(configuration.name)) {
        return invalid(configuration, "the name is empty or contains NUL");
    }
    if (configuration.engineDebugMode != EngineDebugMode::Lua &&
        configuration.engineDebugMode != EngineDebugMode::NativeAndLua)
        return invalid(configuration, "unknown engine Debug mode");
    if (!isFrostEngineRunType(configuration.type) && configuration.engineDebugMode != EngineDebugMode::Lua)
        return invalid(configuration, "only a FrostEngine configuration has an engine Debug mode");
    if (isFrostEngineRunType(configuration.type)) {
        if (!io::normalizePath(configuration.engineProjectPath)) {
            return invalid(configuration,
                           "a FrostEngine configuration needs an absolute project directory or .frostproj");
        }
        // The typed project field is authoritative. Both hosts parse this CLI switch, so allowing
        // another copy in user arguments could silently run a different project.
        for (const std::string& argument : configuration.arguments) {
            if (argument == "--project" || argument.starts_with("--project=")) {
                return invalid(configuration, "set the engine project field instead of a --project argument");
            }
        }
    } else if (!configuration.engineProjectPath.empty()) {
        return invalid(configuration, "only a FrostEngine configuration has an engine project");
    }
    if (!isLuaRunType(configuration.type) && !configuration.interpreterArguments.empty())
        return invalid(configuration, "interpreterArguments belong only to standalone Lua");
    for (const auto& argument : configuration.interpreterArguments)
        if (hasNul(argument))
            return invalid(configuration, "interpreter argument contains NUL");
    switch (configuration.type) {
    case RunType::LuaScript:
    case RunType::LuaRepl:
        if (configuration.program.empty())
            return invalid(configuration, "select an absolute Lua interpreter executable");
        if (configuration.beforeLaunch != BeforeLaunch::None || !configuration.beforeLaunchTasks.empty())
            return invalid(configuration, "standalone Lua configurations do not support build/tasks before launch");
        if (configuration.type == RunType::LuaScript &&
            (!io::isAbsolutePath(configuration.target) || !io::normalizePath(configuration.target)))
            return invalid(configuration, "a Lua script configuration needs an absolute script path");
        if (configuration.type == RunType::LuaRepl &&
            (!configuration.target.empty() || !configuration.arguments.empty()))
            return invalid(configuration, "Lua REPL has no script or script arguments; use interpreterArguments");
        for (std::size_t i = 0; i < configuration.interpreterArguments.size(); ++i) {
            const auto& flag = configuration.interpreterArguments[i];
            if (flag == "-E" || flag == "-W" || flag == "-v")
                continue;
            if (flag == "-e" || flag == "-l") {
                if (++i == configuration.interpreterArguments.size())
                    return invalid(configuration, "Lua -e/-l requires its own argument");
                continue;
            }
            if ((flag.starts_with("-e") || flag.starts_with("-l")) && flag.size() > 2)
                continue;
            if (flag == "--" && configuration.type == RunType::LuaScript &&
                i + 1 == configuration.interpreterArguments.size())
                continue;
            return invalid(configuration, "use Lua 5.4 interpreter flags only; REPL adds -i automatically");
        }
        break;
    case RunType::FrostEditor:
    case RunType::FrostPlayer:
    case RunType::Native:
        if (configuration.program.empty()) {
            return invalid(configuration, "this executable configuration needs a program");
        }
        if (!configuration.target.empty()) {
            return invalid(configuration, "this executable configuration has no target");
        }
        if (configuration.beforeLaunch == BeforeLaunch::Build) {
            return invalid(configuration, "this executable configuration has nothing to build");
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
        if (!configuration.beforeLaunchTasks.empty()) {
            return invalid(configuration, "msbuild configurations cannot have before-launch tasks yet (the "
                                          "RunService builds and starts them; RUN-5)");
        }
        break;
    case RunType::Compound: {
        if (configuration.configurations.empty()) {
            return invalid(configuration, "a compound configuration needs at least one configuration to start");
        }
        std::set<std::string, std::less<>> seen;
        for (const std::string& member : configuration.configurations) {
            if (member.empty() || member.size() > kMaxIdLength || !std::ranges::all_of(member, idCharacter)) {
                return invalid(configuration, std::format("'{}' is not a run configuration id", member));
            }
            if (member == configuration.id) {
                return invalid(configuration, "a compound configuration cannot start itself");
            }
            if (!seen.insert(member).second) {
                return invalid(configuration, std::format("'{}' is listed twice", member));
            }
        }
        if (!configuration.target.empty() || !configuration.program.empty() || !configuration.arguments.empty() ||
            !configuration.workingDirectory.empty() || !configuration.environment.empty() ||
            configuration.beforeLaunch != BeforeLaunch::None || !configuration.beforeLaunchTasks.empty()) {
            return invalid(configuration, "a compound configuration only lists the configurations it starts");
        }
        break;
    }
    case RunType::Attach: {
        const std::string_view target = configuration.target;
        const bool fileName = !target.empty() && target.find_first_of("/\\:") == std::string_view::npos &&
                              target != "." && target != "..";
        if (hasNul(target) || (!fileName && !io::normalizePath(target))) {
            return invalid(configuration,
                           "an attach configuration needs the process's executable name or absolute path");
        }
        if (!configuration.program.empty() || !configuration.arguments.empty() ||
            !configuration.workingDirectory.empty() || !configuration.environment.empty() ||
            configuration.beforeLaunch != BeforeLaunch::None || !configuration.beforeLaunchTasks.empty()) {
            return invalid(configuration, "an attach configuration starts nothing: it only names the process");
        }
        break;
    }
    }
    if (configuration.type != RunType::Compound && !configuration.configurations.empty()) {
        return invalid(configuration, "only a compound configuration lists other configurations");
    }
    for (const BeforeLaunchTask& task : configuration.beforeLaunchTasks) {
        if (!io::normalizePath(task.program)) {
            return invalid(configuration,
                           std::format("the before-launch task '{}' is not an absolute program path", task.program));
        }
        if (!task.workingDirectory.empty() && !io::normalizePath(task.workingDirectory)) {
            return invalid(configuration, std::format("the before-launch task's working directory '{}' is not an "
                                                      "absolute path",
                                                      task.workingDirectory));
        }
        if (std::ranges::any_of(task.arguments, hasNul)) {
            return invalid(configuration, "a before-launch task argument contains NUL");
        }
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
        if (isLuaRunType(configuration->type) && *version < 5)
            return corrupt(index, "standalone Lua requires store version 5");
        if (configuration->engineDebugMode == EngineDebugMode::NativeAndLua && *version < 4)
            return corrupt(index, "native-and-lua Debug requires store version 4");
        if (!ids.insert(configuration->id).second) {
            return corrupt(index, std::format("the id '{}' is used twice", configuration->id));
        }
        configurations.push_back(std::move(*configuration));
    }
    return configurations;
}

bool isFrostEngineRunType(RunType type) noexcept {
    return type == RunType::FrostEditor || type == RunType::FrostPlayer;
}

bool usesRun5Features(const RunConfiguration& configuration) noexcept {
    return configuration.type == RunType::Compound || configuration.type == RunType::Attach ||
           !configuration.beforeLaunchTasks.empty() || !configuration.configurations.empty();
}

bool attachTargetMatches(std::string_view target, std::string_view executablePath) noexcept {
    if (target.empty() || executablePath.empty()) {
        return false;
    }
    const bool absolute = target.find_first_of("/\\") != std::string_view::npos;
    const std::string_view candidate = absolute ? executablePath : io::fileName(executablePath);
    if constexpr (io::kNativePathStyle == io::PathStyle::Windows) {
        // Windows file names ignore ASCII case, and either separator names the same path.
        const auto fold = [](char c) {
            c = c == '/' ? '\\' : c;
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        return std::ranges::equal(target, candidate, [&](char a, char b) { return fold(a) == fold(b); });
    } else {
        return target == candidate;
    }
}

std::string serializeRunConfigurations(std::span<const RunConfiguration> configurations) {
    const bool engine = std::ranges::any_of(
        configurations, [](const RunConfiguration& configuration) { return isFrostEngineRunType(configuration.type); });
    const bool mixed = std::ranges::any_of(
        configurations, [](const auto& c) { return c.engineDebugMode == EngineDebugMode::NativeAndLua; });
    const bool lua = std::ranges::any_of(configurations, [](const auto& c) { return isLuaRunType(c.type); });
    const int version = lua      ? 5
                        : mixed  ? 4
                        : engine ? 3
                                 : (std::ranges::any_of(configurations, usesRun5Features) ? 2 : 1);
    std::string out = std::format("{{\n  \"version\": {},\n  \"configurations\": [", version);
    if (configurations.empty()) {
        out += "]\n}\n";
        return out;
    }
    out += '\n';
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
