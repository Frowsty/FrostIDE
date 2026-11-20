// SPDX-License-Identifier: Apache-2.0
#include "frost/project/unreal_project.hpp"
#include "frost/io/path.hpp"
#include "frost/io/file_system.hpp"
#include "json.hpp"
#include <algorithm>
#include <set>
namespace frost::project {
namespace {
using detail::JsonNode;
bool uniqueKeys(const JsonNode& node) {
    std::set<std::string> keys;
    for (const auto& key : node.keys) if (!keys.insert(key).second) return false;
    return std::ranges::all_of(node.items, uniqueKeys);
}
bool identifier(std::string_view name) {
    if (name.empty() || name.size() > 256) return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}
Result<JsonNode> descriptor(std::string_view text) {
    if (text.size() > 4 * 1024 * 1024) return fail(ErrorCode::InvalidArgument, "Unreal descriptor exceeds 4 MiB");
    auto node = detail::parseJson(text);
    if (!node) return std::unexpected(node.error());
    if (!node->isObject() || !uniqueKeys(*node)) return fail(ErrorCode::Corrupt, "duplicate or invalid Unreal descriptor object");
    const auto version = node->integerAt("FileVersion");
    if (!version || *version < 1) return fail(ErrorCode::Corrupt, "missing Unreal descriptor FileVersion");
    if (*version > 3) return fail(ErrorCode::Unsupported, "newer Unreal descriptor FileVersion");
    return node;
}
bool stringField(const JsonNode& node, std::string_view key) {
    const auto* value = node.find(key);
    return !value || (value->isString() && value->text.find('\0') == std::string::npos);
}
bool boolField(const JsonNode& node, std::string_view key) {
    const auto* value = node.find(key);
    return !value || value->isBool();
}
Result<std::vector<UnrealModule>> modules(const JsonNode& root) {
    std::vector<UnrealModule> result;
    const auto* rows = root.find("Modules");
    if (!rows) return result;
    if (!rows->isArray()) return fail(ErrorCode::Corrupt, "Unreal Modules must be an array");
    std::set<std::string> names;
    for (const auto& row : rows->items) {
        const auto name = row.stringAt("Name");
        const auto type = row.stringAt("Type");
        if (!row.isObject() || !identifier(name) || !identifier(type) || !stringField(row, "LoadingPhase") || !names.insert(name).second)
            return fail(ErrorCode::Corrupt, "invalid or duplicate Unreal module");
        result.push_back({name, type, row.stringAt("LoadingPhase")});
    }
    return result;
}
}
Result<UnrealProject> readUnrealProject(io::IFileSystem& fs, std::string_view absoluteProject) {
    auto reader = fs.openRandomAccess(absoluteProject);
    if (!reader) return std::unexpected(reader.error());
    if ((*reader)->size() > 4 * 1024 * 1024) return fail(ErrorCode::InvalidArgument, "Unreal descriptor exceeds 4 MiB");
    std::string bytes;
    while (bytes.size() < (*reader)->size()) {
        auto chunk = (*reader)->read(bytes.size(), static_cast<std::size_t>(std::min<std::uint64_t>(
            io::IRandomAccessReader::kMaximumReadBytes, (*reader)->size() - bytes.size())));
        if (!chunk) return std::unexpected(chunk.error());
        if (chunk->empty()) return fail(ErrorCode::Busy, "Unreal descriptor changed during read");
        bytes += *chunk;
    }
    return parseUnrealProject(bytes, absoluteProject);
}
Result<UnrealProject> parseUnrealProject(std::string_view json, std::string_view absoluteProject) {
    if (!io::isAbsolutePath(absoluteProject) || !absoluteProject.ends_with(".uproject") || absoluteProject.find('\0') != std::string_view::npos)
        return fail(ErrorCode::InvalidArgument, "expected an absolute .uproject path");
    auto root = descriptor(json);
    if (!root) return std::unexpected(root.error());
    if (!stringField(*root, "EngineAssociation") || !stringField(*root, "Description")) return fail(ErrorCode::Corrupt, "invalid Unreal project string");
    auto moduleRows = modules(*root);
    if (!moduleRows) return std::unexpected(moduleRows.error());
    UnrealProject result{std::string(absoluteProject), root->stringAt("EngineAssociation"), root->stringAt("Description"), std::move(*moduleRows), {}};
    const auto* plugins = root->find("Plugins");
    if (plugins) {
        if (!plugins->isArray()) return fail(ErrorCode::Corrupt, "Unreal Plugins must be an array");
        std::set<std::string> names;
        for (const auto& row : plugins->items) {
            auto name = row.stringAt("Name");
            if (!row.isObject() || !identifier(name) || !row.find("Enabled") || !boolField(row, "Enabled") || !boolField(row, "Optional") || !names.insert(name).second)
                return fail(ErrorCode::Corrupt, "invalid or duplicate Unreal plugin reference");
            result.plugins.push_back({std::move(name), row.boolAt("Enabled"), row.boolAt("Optional")});
        }
    }
    return result;
}
Result<UnrealPluginDescriptor> parseUnrealPluginDescriptor(std::string_view json) {
    auto root = descriptor(json);
    if (!root) return std::unexpected(root.error());
    if (!stringField(*root, "FriendlyName")) return fail(ErrorCode::Corrupt, "invalid Unreal plugin FriendlyName");
    auto rows = modules(*root);
    if (!rows) return std::unexpected(rows.error());
    return UnrealPluginDescriptor{root->stringAt("FriendlyName"), std::move(*rows)};
}
Result<UnrealInstallation> resolveUnrealInstallation(const UnrealProject& project, const std::vector<UnrealInstallation>& installations) {
    if (project.engineAssociation.empty()) return fail(ErrorCode::NotFound, "select an Unreal installation explicitly");
    const UnrealInstallation* result = nullptr;
    for (const auto& installation : installations) {
        if (installation.association != project.engineAssociation) continue;
        if (!io::isAbsolutePath(installation.root) || installation.root.find('\0') != std::string::npos)
            return fail(ErrorCode::InvalidArgument, "invalid selected Unreal installation path");
        if (result) return fail(ErrorCode::InvalidArgument, "ambiguous Unreal engine association");
        result = &installation;
    }
    if (!result) return fail(ErrorCode::NotFound, "Unreal engine association is not installed in supplied metadata");
    return *result;
}
} // namespace frost::project
