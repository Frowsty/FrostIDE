// SPDX-License-Identifier: Apache-2.0
#include "frost/lang/unity_assets.hpp"
#include <algorithm>
#include <regex>
namespace frost::lang {
namespace {
std::string normalizedGuid(std::string_view value) {
    if (value.size() != 32) return {};
    std::string result(value);
    for (auto& c : result) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        else if (!(c >= 'a' && c <= 'f') && !(c >= '0' && c <= '9')) return {};
    }
    return result;
}
bool yamlAsset(std::string_view path) {
    return path.ends_with(".unity") || path.ends_with(".prefab") || path.ends_with(".asset")
        || path.ends_with(".mat") || path.ends_with(".controller") || path.ends_with(".anim");
}
}
void UnityAssetIndex::reset(std::span<const UnityAssetSnapshot> snapshots) {
    m_targets.clear(); m_usages.clear(); m_diagnostics.clear();
    const std::regex metaGuid(R"(^guid: ([0-9a-fA-F]{32})\r?$)");
    const std::regex reference(R"(\{fileID: (-?[0-9]+), guid: ([0-9a-fA-F]{32}), type: [0-9]+\})");
    for (const auto& snapshot : snapshots) {
        if (snapshot.contents.size() > 16 * 1024 * 1024) {
            m_diagnostics.push_back(snapshot.path + ": snapshot exceeds index limit"); continue;
        }
        const bool meta = snapshot.path.ends_with(".meta");
        if (!meta && (!yamlAsset(snapshot.path) || !snapshot.contents.starts_with("%YAML 1.1"))) continue;
        std::vector<std::string> guids;
        std::size_t begin = 0;
        std::optional<std::size_t> scalarIndent;
        while (begin < snapshot.contents.size()) {
            const auto end = snapshot.contents.find('\n', begin);
            const auto count = end == std::string::npos ? snapshot.contents.size() - begin : end - begin;
            const std::string line = snapshot.contents.substr(begin, count);
            const auto indent = line.find_first_not_of(' ');
            const bool blank = indent == std::string::npos || line[indent] == '\r';
            if (scalarIndent && !blank && indent <= *scalarIndent) scalarIndent.reset();
            const bool scalarBody = scalarIndent.has_value();
            const auto scalarColon = line.find(':');
            if (!meta && !scalarBody && scalarColon != std::string::npos) {
                const auto value = line.find_first_not_of(' ', scalarColon + 1);
                if (value != std::string::npos && (line[value] == '|' || line[value] == '>')) scalarIndent = indent;
            }
            std::smatch match;
            if (meta) {
                if (std::regex_match(line, match, metaGuid)) guids.push_back(normalizedGuid(match[1].str()));
            } else {
                // Quoted scalar/comment/block contents are not reference evidence. Only canonical
                // property lines (including sequence properties) are admitted.
                const auto colon = line.find(':');
                const auto brace = line.find('{');
                const auto key = colon == std::string::npos ? std::string{} : line.substr(0, colon);
                const bool plainKey = !key.empty() && key.find_first_not_of(" abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-[]") == std::string::npos;
                if (!scalarBody && plainKey && brace != std::string::npos && colon < brace &&
                    line.substr(colon + 1, brace - colon - 1).find_first_not_of(' ') == std::string::npos &&
                    std::regex_search(line, match, reference) && static_cast<std::size_t>(match.position()) == brace &&
                    line.substr(brace + match.length()).find_first_not_of(" \r") == std::string::npos) {
                    const auto guid = normalizedGuid(match[2].str());
                    if (guid != std::string(32, '0')) m_usages.push_back({snapshot.path, guid, match[1].str(), begin + static_cast<std::size_t>(match.position(2))});
                }
            }
            if (end == std::string::npos) break;
            begin = end + 1;
        }
        if (meta) {
            if (guids.size() == 1 && guids.front() != std::string(32, '0'))
                m_targets.push_back({snapshot.path.substr(0, snapshot.path.size() - 5), snapshot.path, guids.front()});
            else m_diagnostics.push_back(snapshot.path + ": missing or ambiguous GUID");
        }
    }
}
std::optional<UnityAssetTarget> UnityAssetIndex::resolve(std::string_view guid) const {
    const auto key = normalizedGuid(guid);
    std::optional<UnityAssetTarget> result;
    for (const auto& target : m_targets) if (target.guid == key) {
        if (result) return std::nullopt;
        result = target;
    }
    return result;
}
std::vector<UnityAssetUsage> UnityAssetIndex::usages(std::string_view guid) const {
    const auto key = normalizedGuid(guid);
    std::vector<UnityAssetUsage> result;
    for (const auto& usage : m_usages) if (usage.guid == key) result.push_back(usage);
    return result;
}
std::optional<std::vector<UnityAssetMove>> planUnityAssetMove(
    std::span<const UnityAssetSnapshot> snapshots, std::string_view from, std::string_view to) {
    if (from.empty() || to.empty() || from == to || from.ends_with(".meta") || to.ends_with(".meta")) return std::nullopt;
    const std::string sidecar = std::string(from) + ".meta", targetMeta = std::string(to) + ".meta";
    const UnityAssetSnapshot* asset = nullptr; const UnityAssetSnapshot* meta = nullptr;
    for (const auto& file : snapshots) {
        if (file.path == to || file.path == targetMeta) return std::nullopt;
        if (file.path == from) { if (asset) return std::nullopt; asset = &file; }
        if (file.path == sidecar) { if (meta) return std::nullopt; meta = &file; }
    }
    if (!asset || !meta) return std::nullopt;
    UnityAssetIndex index; index.reset(std::span(meta, 1));
    if (!index.diagnostics().empty()) return std::nullopt;
    return std::vector<UnityAssetMove>{{asset->path, std::string(to), asset->contents}, {meta->path, targetMeta, meta->contents}};
}
} // namespace frost::lang
