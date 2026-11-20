// SPDX-License-Identifier: Apache-2.0
// Real assets.list and assets.find-references contracts (FrostEngine AssetFileCommands.cpp).
#include "frost/engine/asset_index.hpp"

#include "frost/io/path.hpp"
#include "frost/lang/uri.hpp"

#include <limits>
#include <set>
namespace frost::engine {
namespace {
bool safePath(std::string_view path) {
    if (path.empty() || io::isAbsolutePath(path) || path.find_first_of("\\:\r\n") != path.npos)
        return false;
    for (std::size_t at = 0; at < path.size();) {
        auto end = path.find('/', at);
        auto part = path.substr(at, end == path.npos ? path.size() - at : end - at);
        if (part.empty() || part == "." || part == "..")
            return false;
        for (unsigned char c : part)
            if (c < 32 || c == 127)
                return false;
        if (end == path.npos)
            return true;
        at = end + 1;
        if (at == path.size())
            return false;
    }
    return false;
}
std::optional<std::uint64_t> count(const rpc::Json& o, std::string_view key) {
    auto* n = o.find(key);
    return n && n->kind() == rpc::Json::Kind::Integer ? n->asUnsigned() : std::nullopt;
}
} // namespace
Result<AssetIndex> AssetIndex::create(ProjectContent content) {
    if (!io::isAbsolutePath(content.directory) || content.assets.size() > 1000000)
        return fail(ErrorCode::Corrupt, "Invalid engine asset index snapshot");
    AssetIndex out;
    out.m_content = std::move(content);
    for (std::size_t i = 0; i < out.m_content.assets.size(); ++i) {
        const auto& a = out.m_content.assets[i];
        if (!a.handle.value || !a.path.starts_with("Assets/") || !safePath(a.path) || a.type.empty() ||
            !out.m_handles.emplace(a.handle.value, i).second ||
            !out.m_paths.emplace(lang::documentKey(a.path), i).second)
            return fail(ErrorCode::Corrupt, "Invalid or duplicate engine asset identity");
    }
    return out;
}
const ProjectAsset* AssetIndex::find(EngineHandle handle) const noexcept {
    auto it = m_handles.find(handle.value);
    return it == m_handles.end() ? nullptr : &m_content.assets[it->second];
}
const ProjectAsset* AssetIndex::find(std::string_view path) const {
    auto it = m_paths.find(lang::documentKey(path));
    return it == m_paths.end() ? nullptr : &m_content.assets[it->second];
}
Result<AssetUsages> parseAssetUsages(const AssetIndex& index, EngineHandle target, const rpc::Json& reply) {
    const auto* asset = index.find(target);
    if (!asset || reply.stringAt("asset") != asset->path)
        return fail(ErrorCode::Corrupt, "Asset references do not match the requested indexed asset");
    auto* targets = reply.find("targets");
    auto* refs = reply.find("references");
    auto* unreadable = reply.find("unreadable");
    auto total = count(reply, "count");
    if (!targets || !targets->isArray() || targets->size() != 1 || !refs || !refs->isArray() ||
        refs->size() > 1000000 || !unreadable || !unreadable->isArray() || !total || *total != refs->size())
        return fail(ErrorCode::Corrupt, "Invalid engine reference arrays or row count");
    const auto& actual = targets->items()[0];
    if (engineHandleAt(actual, "handle") != std::optional(target) || actual.stringAt("path") != asset->path ||
        actual.stringAt("type") != asset->type)
        return fail(ErrorCode::Corrupt, "Reference target identity changed since asset listing");
    AssetUsages out;
    out.target = *asset;
    std::set<std::pair<std::string, std::string>> rows;
    for (const auto& node : refs->items()) {
        AssetUsage row;
        row.path = node.stringAt("path");
        row.type = node.stringAt("type");
        row.setting = node.stringAt("setting");
        row.handle = engineHandleAt(node, "handle");
        auto occurrences = count(node, "count");
        auto unsaved = node.boolAt("unsaved");
        auto* names = node.find("targets");
        if (!safePath(row.path) || !occurrences || !*occurrences || !unsaved || !names || !names->isArray() ||
            names->size() != 1 || !names->items()[0].isString() || *names->items()[0].asString() != asset->path ||
            !rows.emplace(row.path, row.setting).second)
            return fail(ErrorCode::Corrupt, "Invalid engine reference record");
        if (row.type == "ProjectSettings") {
            if (row.path != io::fileName(index.content().projectFile) || row.handle || row.setting.empty() || *unsaved)
                return fail(ErrorCode::Corrupt, "Invalid project-settings asset reference");
        } else {
            const auto* source = index.find(row.path);
            if (!source && row.type == "Scene" && *unsaved && !row.handle)
                row.indexed = false;
            else if (!source || source->type != row.type || !row.handle || *row.handle != source->handle)
                return fail(ErrorCode::Corrupt, "Reference source identity differs from the asset index");
        }
        if (*occurrences > std::numeric_limits<std::uint64_t>::max() - out.occurrences)
            return fail(ErrorCode::OutOfRange, "Asset reference count overflow");
        row.count = *occurrences;
        row.unsaved = *unsaved;
        row.targets.push_back(asset->path);
        out.occurrences += row.count;
        out.references.push_back(std::move(row));
    }
    std::set<std::string> unknown;
    for (const auto& file : unreadable->items()) {
        if (!file.isString() || !safePath(*file.asString()) || !unknown.insert(*file.asString()).second)
            return fail(ErrorCode::Corrupt, "Invalid unreadable reference source");
        out.unreadable.push_back(*file.asString());
    }
    return out;
}
} // namespace frost::engine
