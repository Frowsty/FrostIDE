// SPDX-License-Identifier: Apache-2.0
// Original Unity snapshots only; no generated project or Unity process is consulted.
#pragma once
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace frost::lang {
struct UnityAssetSnapshot {
    std::string path, contents;
};
struct UnityAssetUsage {
    std::string path, guid, fileId;
    std::size_t offset = 0; // UTF-8 byte offset of the GUID in the ORIGINAL captured YAML.
};
struct UnityAssetTarget {
    std::string path, metaPath, guid;
};
// Bounded, conservative index of Unity's canonical single-line flow references.
// Unsupported YAML is never interpreted as a semantic reference. This is not a YAML serializer.
class UnityAssetIndex {
public:
    void reset(std::span<const UnityAssetSnapshot> snapshots);
    [[nodiscard]] std::optional<UnityAssetTarget> resolve(std::string_view guid) const;
    [[nodiscard]] std::vector<UnityAssetUsage> usages(std::string_view guid) const;
    [[nodiscard]] const std::vector<std::string>& diagnostics() const { return m_diagnostics; }
private:
    std::vector<UnityAssetTarget> m_targets;
    std::vector<UnityAssetUsage> m_usages;
    std::vector<std::string> m_diagnostics;
};
// Receipt adapter input: pair BOTH original files in one RefactoringPlan::renames;
// preserve these exact bytes, and revalidate trust, project generation, paths and buffers.
struct UnityAssetMove {
    std::string from, to, expectedBytes;
};
[[nodiscard]] std::optional<std::vector<UnityAssetMove>> planUnityAssetMove(
    std::span<const UnityAssetSnapshot> snapshots, std::string_view from, std::string_view to);
} // namespace frost::lang
