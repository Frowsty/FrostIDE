// SPDX-License-Identifier: Apache-2.0
// Bounded launchSettings decoding; execution intent never silently falls back to another command.
#include "frost/build/managed_launch_settings.hpp"

#include "json.hpp"

#include <algorithm>
#include <set>
namespace frost::build {
namespace {
Result<void> object(const detail::JsonValue& value) {
    if (value.kind != detail::JsonValue::Kind::Object)
        return fail(ErrorCode::Corrupt, "Launch settings requires an object");
    std::set<std::string> keys;
    for (const auto& key : value.memberNames)
        if (key.find('\0') != key.npos || !keys.insert(key).second)
            return fail(ErrorCode::Corrupt, "Duplicate or NUL launch setting key");
    return {};
}
// CommandLineArgs follows the .NET host's double-quote/backslash argument convention.
// No shell metacharacters or environment variables are evaluated; unbalanced quotes refuse.
Result<std::vector<std::string>> arguments(std::string_view text) {
    std::vector<std::string> result;
    std::string current;
    bool quoted = false, started = false;
    for (std::size_t i = 0; i < text.size();) {
        if (!quoted && (text[i] == ' ' || text[i] == '\t')) {
            if (started) {
                result.push_back(std::move(current));
                current.clear();
                started = false;
            }
            ++i;
            continue;
        }
        started = true;
        unsigned slashes = 0;
        while (i < text.size() && text[i] == '\\') {
            ++slashes;
            ++i;
        }
        if (i < text.size() && text[i] == '"') {
            current.append(slashes / 2, '\\');
            if (slashes % 2)
                current += '"';
            else
                quoted = !quoted;
            ++i;
        } else {
            current.append(slashes, '\\');
            if (i < text.size())
                current += text[i++];
        }
    }
    if (quoted)
        return fail(ErrorCode::Corrupt, "Unbalanced commandLineArgs quotes");
    if (started)
        result.push_back(std::move(current));
    if (result.size() > 4096)
        return fail(ErrorCode::OutOfRange, "Too many launch arguments");
    return result;
}
} // namespace
Result<std::vector<ManagedLaunchProfile>> parseManagedLaunchSettings(std::string_view wire) {
    if (wire.size() > 1024 * 1024)
        return fail(ErrorCode::OutOfRange, "Launch settings exceeds1MiB");
    auto json = detail::parseJson(wire);
    if (!json)
        return std::unexpected(json.error());
    if (auto valid = object(*json); !valid)
        return std::unexpected(valid.error());
    const auto* profiles = json->find("profiles");
    if (!profiles)
        return std::vector<ManagedLaunchProfile>{};
    if (auto valid = object(*profiles); !valid)
        return std::unexpected(valid.error());
    if (profiles->memberNames.size() > 256)
        return fail(ErrorCode::OutOfRange, "Too many launch profiles");
    std::vector<ManagedLaunchProfile> result;
    for (std::size_t index = 0; index < profiles->memberNames.size(); ++index) {
        ManagedLaunchProfile profile;
        profile.name = profiles->memberNames[index];
        const auto& value = profiles->memberValues[index];
        if (profile.name.empty())
            return fail(ErrorCode::Corrupt, "Empty launch profile name");
        if (auto valid = object(value); !valid)
            return std::unexpected(valid.error());
        for (const auto* key : {"commandName", "commandLineArgs", "workingDirectory", "applicationUrl", "launchUrl"}) {
            if (const auto* entry = value.find(key);
                entry && (entry->kind != detail::JsonValue::Kind::String || entry->text.find('\0') != entry->text.npos))
                return fail(ErrorCode::Corrupt, "Invalid launch profile string");
        }
        profile.commandName = value.stringMember("commandName");
        profile.workingDirectory = value.stringMember("workingDirectory");
        profile.applicationUrl = value.stringMember("applicationUrl");
        profile.launchUrl = value.stringMember("launchUrl");
        if (const auto* browser = value.find("launchBrowser")) {
            if (browser->kind != detail::JsonValue::Kind::Bool)
                return fail(ErrorCode::Corrupt, "Invalid launchBrowser");
            profile.launchBrowser = browser->boolean;
        }
        auto args = arguments(value.stringMember("commandLineArgs"));
        if (!args)
            return std::unexpected(args.error());
        profile.arguments = std::move(*args);
        if (const auto* environment = value.find("environmentVariables")) {
            if (auto valid = object(*environment); !valid)
                return std::unexpected(valid.error());
            for (std::size_t i = 0; i < environment->memberNames.size(); ++i) {
                const auto& key = environment->memberNames[i];
                const auto& data = environment->memberValues[i];
                if (key.empty() || key.find('=') != key.npos || data.kind != detail::JsonValue::Kind::String ||
                    data.text.find('\0') != data.text.npos)
                    return fail(ErrorCode::Corrupt, "Invalid launch environment");
                profile.environment.emplace(key, data.text);
            }
        }
        if (profile.commandName != "Project")
            profile.unavailable = "Unsupported launch command: " + profile.commandName;
        constexpr std::string_view supported[] = {"commandName",          "commandLineArgs",  "workingDirectory",
                                                  "applicationUrl",       "launchUrl",        "launchBrowser",
                                                  "environmentVariables", "dotnetRunMessages"};
        for (const auto& key : value.memberNames)
            if (std::ranges::find(supported, key) == std::end(supported))
                profile.unavailable = "Unsupported launch option: " + key;
        auto expansionFields = profile.arguments;
        expansionFields.insert(expansionFields.end(),
                               {profile.workingDirectory, profile.applicationUrl, profile.launchUrl});
        for (const auto& [name, value] : profile.environment)
            expansionFields.push_back(value);
        for (const auto& field : expansionFields)
            if (field.find('%') != field.npos || field.find("$(") != field.npos || field.find("${") != field.npos)
                profile.unavailable = "Launch setting expansion requires an external provider";
        result.push_back(std::move(profile));
    }
    return result;
}
} // namespace frost::build
