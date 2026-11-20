// SPDX-License-Identifier: Apache-2.0
// FEC-4/5: immutable engine listing index and actual find-references observations.
#pragma once
#include "frost/engine/project_content.hpp"

#include <map>
namespace frost::engine {
class AssetIndex {
public:
    // Owns the actual validated CLI snapshot; no filesystem scan or fabricated asset entries.
    // Pure/thread-safe after construction, O(n log n), bounded to 1M listed assets.
    [[nodiscard]] static Result<AssetIndex> create(ProjectContent content);
    [[nodiscard]] const ProjectContent& content() const noexcept { return m_content; }
    [[nodiscard]] const ProjectAsset* find(EngineHandle handle) const noexcept;
    [[nodiscard]] const ProjectAsset* find(std::string_view relativePath) const;

private:
    ProjectContent m_content;
    std::map<std::uint64_t, std::size_t> m_handles;
    std::map<std::string, std::size_t, std::less<>> m_paths;
};
struct AssetUsage {
    std::string path, type, setting;
    std::optional<EngineHandle> handle;
    std::uint64_t count{};
    bool unsaved{};
    bool indexed = true; // False only for an engine-reported unregistered unsaved Scene.
    std::vector<std::string> targets;
};
struct AssetUsages {
    ProjectAsset target;
    std::vector<AssetUsage> references;
    std::vector<std::string> unreadable;
    std::uint64_t occurrences{};
    // A partial search has no complete Code Vision count (never a plausible zero).
    [[nodiscard]] std::optional<std::uint64_t> codeVisionCount() const noexcept {
        return unreadable.empty() ? std::optional<std::uint64_t>(references.size()) : std::nullopt;
    }
};
// Parse the real assets.find-references result, bound to one actual index asset. Strict exact
// handle/path/type, row counters and target sets; unsaved scenes and unreadable files survive.
// Pure/thread-safe, O(rows log n). Locations are documents, not invented source-line matches.
[[nodiscard]] Result<AssetUsages> parseAssetUsages(const AssetIndex& index, EngineHandle target,
                                                   const rpc::Json& reply);
} // namespace frost::engine
