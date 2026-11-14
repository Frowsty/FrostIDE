// SPDX-License-Identifier: Apache-2.0
// Sources: Microsoft launch-vs-schema-reference-cpp / tasks-vs-json-schema-reference-cpp.
#include "frost/run/vs_import.hpp"

#include "frost/io/path.hpp"
#include "frost/run/run_types.hpp"

#include <algorithm>
namespace frost::run {
namespace {
using rpc::Json;
Result<std::string> expand(std::string value, std::string_view root) {
    constexpr std::string_view macro = "${workspaceRoot}";
    std::size_t at = 0;
    while ((at = value.find(macro, at)) != std::string::npos) {
        value.replace(at, macro.size(), root);
        at += root.size();
    }
    if (value.find("${") != std::string::npos || value.find("%") != std::string::npos)
        return fail(ErrorCode::Unsupported, "Only ${workspaceRoot} substitution is supported");
    return value;
}
Result<std::string> absolute(const Json& item, std::string_view key, std::string_view root, bool required) {
    const auto* member = item.find(key);
    if (!member) {
        if (required)
            return fail(ErrorCode::Unsupported, "An explicit " + std::string(key) + " is required");
        return std::string{};
    }
    if (!member->isString())
        return fail(ErrorCode::Corrupt, std::string(key) + " must be a string");
    auto value = expand(*member->asString(), root);
    if (!value)
        return std::unexpected(value.error());
    if (value->empty())
        return fail(ErrorCode::InvalidArgument, std::string(key) + " cannot be empty");
    return io::isAbsolutePath(*value) ? io::normalizePath(*value) : io::joinPath(root, *value);
}
Result<RunConfiguration> convert(const Json& item, bool task, std::string_view root, std::size_t index) {
    if (!item.isObject())
        return fail(ErrorCode::Corrupt, "An entry must be an object");
    const auto allowed =
        task
            ? std::vector<std::string_view>{"taskLabel",        "type", "appliesTo",   "contextType", "command", "args",
                                            "workingDirectory", "env",  "launchOption"}
            : std::vector<std::string_view>{"name", "type",        "program",   "args",    "currentDir",
                                            "env",  "environment", "debugType", "project", "projectTarget"};
    for (const auto& key : item.keys())
        if (!std::ranges::contains(allowed, key))
            return fail(ErrorCode::Unsupported, "Unsupported execution setting: " + key);
    for (const auto key : {"type", "debugType", "project", "projectTarget", "launchOption", "appliesTo", "contextType"})
        if (const auto* value = item.find(key); value && !value->isString())
            return fail(ErrorCode::Corrupt, std::string(key) + " must be a string");
    const auto type = item.stringAt("type", task ? "launch" : "exe");
    if ((task && type != "launch") || (!task && type != "exe" && type != "cppvsdbg"))
        return fail(ErrorCode::Unsupported, "Only local native executable/launch entries are imported");
    if (item.find("debugType") && item.stringAt("debugType") != "native")
        return fail(ErrorCode::Unsupported, "Managed/mixed debug modes require explicit native configuration");
    if (item.find("projectTarget") && !item.stringAt("projectTarget").empty())
        return fail(ErrorCode::Unsupported, "Project target resolution is unavailable; supply an explicit executable");
    if (item.find("launchOption") && item.stringAt("launchOption") != "None")
        return fail(ErrorCode::Unsupported, "ContinueOnError/IgnoreError execution cannot be represented");
    RunConfiguration config;
    config.id = std::string(task ? "vs.task." : "vs.launch.") + std::to_string(index + 1);
    config.name = std::string(item.stringAt(task ? "taskLabel" : "name"));
    auto program = absolute(item, task ? "command" : "program", root, true);
    if (!program)
        return std::unexpected(program.error());
    if (task && !io::isAbsolutePath(item.stringAt("command")) &&
        item.stringAt("command").find("${workspaceRoot}") == std::string_view::npos)
        return fail(ErrorCode::Unsupported,
                    "Task commands need an absolute path or ${workspaceRoot}; PATH lookup is not imported");
    config.program = std::move(*program);
    auto directory = absolute(item, task ? "workingDirectory" : "currentDir", root, false);
    if (!directory)
        return std::unexpected(directory.error());
    config.workingDirectory = directory->empty() ? std::string(root) : std::move(*directory);
    if (const auto* args = item.find("args")) {
        if (!args->isArray())
            return fail(ErrorCode::Corrupt, "args must be an array");
        for (const auto& arg : args->items()) {
            if (!arg.isString())
                return fail(ErrorCode::Corrupt, "Each argument must be a string");
            auto value = expand(*arg.asString(), root);
            if (!value)
                return std::unexpected(value.error());
            config.arguments.push_back(std::move(*value));
        }
    }
    if (item.find("env") && item.find("environment"))
        return fail(ErrorCode::Unsupported, "Conflicting env/environment representations");
    if (const auto* env = item.find("env")) {
        if (!env->isObject())
            return fail(ErrorCode::Corrupt, "env must be an object");
        for (std::size_t i = 0; i < env->size(); ++i) {
            const auto& value = env->items()[i];
            if (!value.isString() && !value.isNull())
                return fail(ErrorCode::Corrupt, "Environment values must be strings or null");
            if (value.isNull())
                config.environment.push_back({env->keys()[i], std::nullopt});
            else {
                auto expanded = expand(*value.asString(), root);
                if (!expanded)
                    return std::unexpected(expanded.error());
                config.environment.push_back({env->keys()[i], std::move(*expanded)});
            }
        }
    }
    if (const auto* env = item.find("environment")) {
        if (!env->isArray())
            return fail(ErrorCode::Corrupt, "environment must be an array");
        for (const auto& variable : env->items()) {
            const auto* value = variable.find("value");
            if (variable.stringAt("name").empty() || !value || !value->isString())
                return fail(ErrorCode::Corrupt, "Environment entries need string name and value");
            auto expanded = expand(*value->asString(), root);
            if (!expanded)
                return std::unexpected(expanded.error());
            config.environment.push_back({std::string(variable.stringAt("name")), std::move(*expanded)});
        }
    }
    if (auto valid = validateRunConfiguration(config); !valid)
        return std::unexpected(valid.error());
    return config;
}
} // namespace
Result<RunImportReport> importVisualStudioConfigurations(std::string_view json, std::string_view fileName,
                                                         std::string_view workspace,
                                                         std::span<const RunConfiguration> existing) {
    if (!io::isAbsolutePath(workspace))
        return fail(ErrorCode::InvalidArgument, "Import needs an absolute workspace");
    if (json.size() > 4U * 1024U * 1024U)
        return fail(ErrorCode::OutOfRange, "Import exceeds 4 MiB");
    const bool task = fileName == "tasks.vs.json";
    if (!task && fileName != "launch.vs.json")
        return fail(ErrorCode::Unsupported, "Select launch.vs.json or tasks.vs.json");
    auto normalized = rpc::normalizeJsonComments(json);
    if (!normalized)
        return std::unexpected(normalized.error());
    auto document = rpc::parseJson(*normalized);
    if (!document)
        return std::unexpected(document.error());
    if (!document->isObject())
        return fail(ErrorCode::Corrupt, "Import root must be an object");
    for (const auto& key : document->keys())
        if (key != "version" && key != "$schema" && key != (task ? "tasks" : "configurations"))
            return fail(ErrorCode::Unsupported, "Unsupported import root setting: " + key);
    if (const auto* version = document->find("version");
        version && (!version->isString() || document->stringAt("version") != "0.2.1"))
        return fail(ErrorCode::Unsupported, "Only Visual Studio schema version 0.2.1 is supported");
    const auto* entries = document->find(task ? "tasks" : "configurations");
    if (!entries || !entries->isArray())
        return fail(ErrorCode::Corrupt, "Import needs its configuration/task array");
    if (entries->size() > 1024)
        return fail(ErrorCode::OutOfRange, "Import exceeds 1024 entries");
    RunImportReport report;
    std::vector<RunConfiguration> used(existing.begin(), existing.end());
    for (std::size_t i = 0; i < entries->size(); ++i) {
        const auto& item = entries->items()[i];
        auto config = convert(item, task, workspace, i);
        if (!config) {
            report.skipped.push_back(
                {std::string(item.stringAt(task ? "taskLabel" : "name", "entry " + std::to_string(i + 1))),
                 config.error().message()});
            continue;
        }
        config->id = uniqueRunConfigurationId(config->id, used);
        used.push_back(*config);
        report.configurations.push_back(std::move(*config));
    }
    return report;
}
} // namespace frost::run
