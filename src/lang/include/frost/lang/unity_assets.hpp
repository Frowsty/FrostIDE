// SPDX-License-Identifier: Apache-2.0
// Original Unity snapshots only; no generated project or Unity process is consulted.
#pragma once
#include "frost/io/file_system.hpp"
#include "frost/lang/navigation.hpp"
#include <stop_token>
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
// Bounded, conservative index of Unity's canonical single-line flow references (including array elements).
// Unsupported YAML is never interpreted as a semantic reference. This is not a YAML serializer.
class UnityAssetIndex {
public:
    void reset(std::span<const UnityAssetSnapshot> snapshots);
    [[nodiscard]] std::optional<UnityAssetTarget> resolve(std::string_view guid) const;
    [[nodiscard]] std::vector<UnityAssetUsage> usages(std::string_view guid) const;
    [[nodiscard]] std::optional<UnityAssetUsage> usageAt(std::string_view path, std::size_t offset) const;
    [[nodiscard]] std::optional<UnityAssetTarget> asset(std::string_view path) const;
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
struct UnityAssetQuery {
    bool recognized = false;
    std::vector<Location> declarations;
    std::vector<UsageGroup> usages;
    std::vector<std::string> diagnostics;
};
// Runs on the language worker with the existing project source-index inventory and captured
// dirty editor overlays. Never scans the project, starts Unity or reads generated project files.
[[nodiscard]] UnityAssetQuery queryUnityAssets(io::IFileSystem& fs, std::span<const std::string> projectFiles,
    std::span<const UnityAssetSnapshot> overlays, std::string_view path, std::size_t byteOffset,
    std::stop_token stop = {});
} // namespace frost::lang
