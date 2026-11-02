// SPDX-License-Identifier: Apache-2.0
//
// CMakeViewModel: the tree the Solution Explorer shows for an opened CMake folder (ADR 0046), built
// from a CMakeProject (cmake_file_api.hpp). Qt-free; frost::ui_cmake adapts it to a Qt item model.
//
/*
 * Shape:
 *
 *     <project name>                     Project (the folder name when project() has none)
 *       <target>                         Target, one per target that is not generator-provided,
 *         <folder>/<folder>/<file>       Folder and Source nodes: the target's sources by their
 *         Generated/<file>               directory below the source directory; generated sources
 *         External/<file>                (isGenerated) under "Generated", others outside the
 *                                        source directory under "External"
 *       CMake                            CMakeFiles: the project's CMakeLists.txt and *.cmake
 *         CMakeLists.txt, sub/CMakeLists.txt, cmake/x.cmake, CMakePresets.json
 *                                        (CMakeProject::projectInputs plus the presets files)
 *
 * Targets are sorted by name; inside a node folders come first, then files, each by label with
 * ASCII case ignored. Node 0 is the root; ids are dense and fixed for the model's lifetime.
 * Node::key is stable across reloads of the same project (built from target names and paths), so
 * a tree view can keep expansion and selection after a reconfigure.
 *
 * Thread safety: immutable after build(); every const member may be called from any thread.
 */
#pragma once

#include "frost/project/cmake_file_api.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::project {

enum class CMakeViewNodeKind : std::uint8_t {
    Project,    // The root.
    Target,     // A CMake target.
    Folder,     // A directory of a target's sources (or "Generated", "External").
    Source,     // One source file of a target.
    CMakeFiles, // The "CMake" group.
    CMakeFile,  // A CMakeLists.txt, *.cmake or presets file.
};

struct CMakeViewNode {
    CMakeViewNodeKind kind = CMakeViewNodeKind::Source;
    std::string label{};
    // Absolute: the file of Source and CMakeFile nodes, the directory of Folder nodes (empty for
    // "Generated" and "External") and the source directory for Project; "" otherwise.
    std::string path{};
    std::optional<std::size_t> target{}; // Index into CMakeProject::targets for Target, Folder and Source nodes.
    CMakeTargetType targetType = CMakeTargetType::Utility; // For Target nodes.
    bool generated = false; // Source nodes: produced by the build (the UI shows "Build to generate").
    std::optional<std::size_t> parent{};
    std::vector<std::size_t> children{};
    std::string key{}; // Stable across reloads (header comment); unique in the model.
};

class CMakeViewModel {
public:
    // An empty model (no project): a root node only, labelled `rootLabel`.
    explicit CMakeViewModel(std::string rootLabel = {});

    /*
     * The tree of `project` (header comment). `presetFiles` are added under "CMake" (absolute;
     * duplicates of inputs are dropped); `rootLabel` overrides the project name. O(n log n) in the
     * number of sources and inputs.
     */
    [[nodiscard]] static CMakeViewModel
    build(const CMakeProject& project, std::span<const std::string> presetFiles = {}, std::string_view rootLabel = {});

    [[nodiscard]] static constexpr std::size_t root() noexcept { return 0; }
    [[nodiscard]] std::span<const CMakeViewNode> nodes() const noexcept { return m_nodes; }
    // Precondition (FROST_PRE): id < nodes().size().
    [[nodiscard]] const CMakeViewNode& node(std::size_t id) const;
    // The node whose key is `key`, if any. O(log n).
    [[nodiscard]] std::optional<std::size_t> findKey(std::string_view key) const;
    // The Source and CMakeFile nodes showing the file at `path` (absolute; ASCII case ignored on
    // Windows). O(n).
    [[nodiscard]] std::vector<std::size_t> findPath(std::string_view path) const;

private:
    std::size_t add(CMakeViewNode node);

    std::vector<CMakeViewNode> m_nodes;
    std::vector<std::pair<std::string, std::size_t>> m_keys; // Sorted by key.
};

} // namespace frost::project
