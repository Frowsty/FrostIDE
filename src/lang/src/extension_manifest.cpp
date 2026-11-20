// SPDX-License-Identifier: Apache-2.0
// Strict versioned public contribution manifests and literal argument expansion; no package installer.
#include "frost/lang/extension_manifest.hpp"

#include <algorithm>
#include <set>
namespace frost::lang {
namespace {
bool id(std::string_view s) {
    return !s.empty() && s.size() <= 64 &&
           std::ranges::all_of(s, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}
bool text(std::string_view s, std::size_t limit = 160) {
    return !s.empty() && s.size() <= limit && s.find('\0') == s.npos;
}
Result<LanguageServerDefinition> tool(const Json& value) {
    auto manifest = Json::object().set("version", 1).set("servers", Json::array({value}));
    auto parsed = LanguageServerRegistry::parse(manifest.dump());
    if (!parsed)
        return std::unexpected(parsed.error());
    return parsed->definitions().front();
}
} // namespace
Result<ExtensionCatalog> ExtensionCatalog::parse(std::string_view manifest) {
    if (manifest.size() > 1024 * 1024)
        return fail(ErrorCode::OutOfRange, "Extension catalog exceeds 1 MiB");
    auto parsed = parseJson(manifest);
    if (!parsed)
        return std::unexpected(parsed.error());
    const auto* api = parsed->find("apiVersion");
    const auto* extensions = parsed->find("extensions");
    if (!api || api->kind() != Json::Kind::Integer || api->asUnsigned() != kExtensionApiVersion || !extensions ||
        !extensions->isArray() || extensions->size() > 32)
        return fail(ErrorCode::Unsupported, "Expected public extension API v1 and at most 32 contributions");
    ExtensionCatalog out;
    std::set<std::string> names;
    for (const auto& row : extensions->items()) {
        ExtensionContribution entry;
        entry.id = row.stringAt("id");
        entry.name = row.stringAt("name");
        entry.version = row.stringAt("version");
        auto enabled = row.boolAt("enabled");
        const auto* capabilities = row.find("capabilities");
        const auto* contributes = row.find("contributes");
        if (!id(entry.id) || !text(entry.name) || !text(entry.version, 128) || !enabled ||
            !names.insert(entry.id).second || !capabilities || !capabilities->isArray() || capabilities->size() > 3 ||
            !contributes || !contributes->isObject())
            return fail(ErrorCode::InvalidArgument,
                        "Invalid extension identity, capability grant or contribution object");
        entry.enabled = *enabled;
        std::set<std::string> grants;
        for (const auto& grant : capabilities->items()) {
            if (!grant.isString() ||
                (*grant.asString() != "languages" && *grant.asString() != "commands" &&
                 *grant.asString() != "debugAdapters") ||
                !grants.insert(*grant.asString()).second)
                return fail(ErrorCode::PermissionDenied,
                            "Unknown or duplicated extension capability; no implicit permissions");
        }
        for (const auto& key : contributes->keys())
            if (key != "languageServers" && key != "commands" && key != "debugAdapters")
                return fail(ErrorCode::Unsupported, "Unknown public contribution family: " + key);
        if (const auto* servers = contributes->find("languageServers")) {
            if (!grants.contains("languages") || !servers->isArray() || servers->size() > 16)
                return fail(ErrorCode::PermissionDenied,
                            "Language contributions require explicit languages capability and bounded list");
            auto validated =
                LanguageServerRegistry::parse(Json::object().set("version", 1).set("servers", *servers).dump());
            if (!validated)
                return std::unexpected(validated.error());
            entry.languageServers = *servers;
        }
        std::set<std::string> ids;
        if (const auto* commands = contributes->find("commands")) {
            if (!grants.contains("commands") || !commands->isArray() || commands->size() > 64)
                return fail(ErrorCode::PermissionDenied, "Command contributions require explicit commands capability");
            for (const auto& c : commands->items()) {
                ExtensionCommand command;
                command.id = c.stringAt("id");
                command.title = c.stringAt("title");
                command.serverId = c.stringAt("serverId");
                command.command = c.stringAt("command");
                const auto* arguments = c.find("arguments");
                if (!id(command.id) || !text(command.title) || !text(command.serverId, 80) ||
                    !text(command.command, 256) || !ids.insert("command/" + command.id).second || !arguments ||
                    !arguments->isArray())
                    return fail(ErrorCode::InvalidArgument, "Invalid advertised-server command contribution");
                command.arguments = *arguments;
                entry.commands.push_back(std::move(command));
            }
        }
        if (const auto* adapters = contributes->find("debugAdapters")) {
            if (!grants.contains("debugAdapters") || !adapters->isArray() || adapters->size() > 16)
                return fail(ErrorCode::PermissionDenied,
                            "Debugger contributions require explicit debugAdapters capability");
            for (const auto& a : adapters->items()) {
                ExtensionDebugAdapter adapter;
                adapter.id = a.stringAt("id");
                adapter.title = a.stringAt("title");
                adapter.adapterId = a.stringAt("adapterId");
                const auto* definition = a.find("tool");
                const auto* launch = a.find("launch");
                if (!id(adapter.id) || !text(adapter.title) || !text(adapter.adapterId, 80) ||
                    !ids.insert("debug/" + adapter.id).second || !definition || !launch || !launch->isObject() ||
                    launch->stringAt("request") == "attach")
                    return fail(ErrorCode::InvalidArgument, "Invalid launch-only debugger contribution");
                auto validated = tool(*definition);
                if (!validated)
                    return std::unexpected(validated.error());
                adapter.tool = std::move(*validated);
                adapter.launch = *launch;
                entry.debugAdapters.push_back(std::move(adapter));
            }
        }
        out.m_extensions.push_back(std::move(entry));
    }
    return out;
}
Result<std::string> ExtensionCatalog::composeLanguageRegistry(std::string_view base) const {
    auto parsed = parseJson(base);
    if (!parsed)
        return std::unexpected(parsed.error());
    auto validated = LanguageServerRegistry::parse(base);
    if (!validated)
        return std::unexpected(validated.error());
    auto servers = *parsed->find("servers");
    for (const auto& extension : m_extensions)
        if (extension.enabled)
            for (const auto& server : extension.languageServers.items())
                servers.push(server);
    auto combined = Json::object().set("version", 1).set("servers", std::move(servers)).dump();
    if (auto valid = LanguageServerRegistry::parse(combined); !valid)
        return std::unexpected(valid.error());
    return combined;
}
Result<Json> expandExtensionValues(const Json& value, std::string_view file, std::string_view workspace,
                                   unsigned depth) {
    if (depth > 32)
        return fail(ErrorCode::OutOfRange, "Extension template nesting exceeds 32");
    if (value.isString()) {
        auto s = *value.asString();
        if (s.size() > 32768)
            return fail(ErrorCode::OutOfRange, "Extension template string too large");
        std::size_t at = 0;
        while ((at = s.find("${", at)) != std::string::npos) {
            const auto end = s.find('}', at + 2);
            if (end == s.npos)
                return fail(ErrorCode::InvalidArgument, "Unterminated extension template");
            const auto name = s.substr(at, end - at + 1);
            std::string_view replacement;
            if (name == "${file}")
                replacement = file;
            else if (name == "${workspaceFolder}")
                replacement = workspace;
            else
                return fail(ErrorCode::PermissionDenied, "Unsupported extension template: " + name);
            s.replace(at, name.size(), replacement);
            at += replacement.size();
            if (s.size() > 32768)
                return fail(ErrorCode::OutOfRange, "Expanded extension value too large");
        }
        return Json(std::move(s));
    }
    if (value.isArray() || value.isObject()) {
        auto result = value.isArray() ? Json::array() : Json::object();
        for (std::size_t i = 0; i < value.size(); ++i) {
            auto item = expandExtensionValues(value.items()[i], file, workspace, depth + 1);
            if (!item)
                return std::unexpected(item.error());
            if (value.isArray())
                result.push(std::move(*item));
            else
                result.set(value.keys()[i], std::move(*item));
        }
        return result;
    }
    return value;
}
bool extensionCommandAdvertised(const ServerCapabilities& capabilities, std::string_view command) {
    const auto* provider = capabilities.raw.find("executeCommandProvider");
    const auto* commands = provider ? provider->find("commands") : nullptr;
    return commands && commands->isArray() && std::ranges::any_of(commands->items(), [&](const Json& value) {
               return value.isString() && *value.asString() == command;
           });
}
} // namespace frost::lang
