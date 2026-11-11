// SPDX-License-Identifier: Apache-2.0
//
// ProjectFiles: the files of the open project as the File System view and Go to File see them (ADR
// 0042): the gitignore-aware folder scan of the project root (frost::io::scanTree with .gitignore and
// .ignore files, the default excludes and skip names) as a tree, plus the open solution's files
// (SolutionModel), merged into Go to File's candidate list. M2's index replaces the candidate source.
/*
 * Tree. Node 0 is the root; children are sorted directories first, then by name (ASCII case
 * ignored, then byte order). Paths are absolute in the scan's syntax; `relative` is '/'-separated.
 * Candidates: every scanned regular file, then every solution file the scan did not list (outside
 * the folder, or excluded), without duplicates (ASCII case ignored on the Windows style).
 * Thread safety: a plain value; build it anywhere, read it on one thread.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/tree_scan.hpp"
#include "frost/sched/scheduler.hpp"
#include "frost/sched/task.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace frost::project {
class DirectoryRoles;
class SolutionModel;
} // namespace frost::project

namespace frost::ui::nav {

/*
 * The scan options of a project tree: .gitignore and .ignore files, defaultProjectSkipNames(),
 * recursive; and either the project's directory roles (UX-23, frost/project/directory_roles.hpp:
 * a Generated or Excluded folder is listed in its parent but never descended, so the tree shows it
 * empty and the user can mark it again) or, without roles, defaultProjectExcludes() at the bottom
 * of the ignore stack.
 */
[[nodiscard]] io::ScanOptions projectScanOptions(std::shared_ptr<const project::DirectoryRoles> roles = nullptr);

// scanTree(fs, scheduler, root, projectScanOptions(roles), token). Errors: scanTree's.
[[nodiscard]] sched::Task<Result<io::TreeScan>>
scanProject(io::IFileSystem& fs, sched::IScheduler& scheduler, std::string root, std::stop_token token = {},
            std::shared_ptr<const project::DirectoryRoles> roles = nullptr);

// The absolute paths of the solution's file nodes (project items and solution items), in node order.
[[nodiscard]] std::vector<std::string> solutionFiles(const project::SolutionModel& solution);

class ProjectFiles {
public:
    struct Node {
        std::string name;
        std::string path;     // Absolute.
        std::string relative; // '/'-separated; "" for the root.
        bool directory = false;
        std::optional<std::size_t> parent;
        std::vector<std::size_t> children;
        std::size_t row = 0; // Position among the parent's sorted children.
    };

    ProjectFiles() = default;
    // An empty tree at `root` (absolute).
    explicit ProjectFiles(std::string root);

    // Replaces the tree with `scan` (whose root is this root). O(n log n).
    void setScan(const io::TreeScan& scan);
    // Like setScan, but can abandon preparation when stopped; the value is unspecified on cancellation.
    bool setScanCancellable(const io::TreeScan& scan, const std::stop_token& token);
    // Replaces the solution's files (absolute paths). O(n log n).
    void setSolutionFiles(std::vector<std::string> files);
    // Returns false if stopped; the value is unspecified on cancellation.
    bool setSolutionFilesCancellable(std::vector<std::string> files, const std::stop_token& token);

    [[nodiscard]] const std::string& root() const noexcept { return m_root; }
    [[nodiscard]] const std::vector<Node>& nodes() const noexcept { return m_nodes; }
    [[nodiscard]] const Node& node(std::size_t index) const { return m_nodes.at(index); }
    // The node at `path` (absolute), if the tree has it. O(log n).
    [[nodiscard]] std::optional<std::size_t> find(std::string_view path) const;
    // Go to File's candidates (absolute paths) and their display forms (relative to the root when
    // below it), index-aligned.
    [[nodiscard]] const std::vector<std::string>& candidates() const noexcept { return m_candidates; }
    [[nodiscard]] const std::vector<std::string>& displayCandidates() const noexcept { return m_display; }
    [[nodiscard]] std::size_t fileCount() const noexcept { return m_fileCount; }

private:
    bool rebuildCandidates(const std::stop_token& token = {});

    std::string m_root;
    std::vector<Node> m_nodes;
    std::vector<std::pair<std::string, std::size_t>> m_byPath; // Sorted by path.
    std::vector<std::string> m_solutionFiles;
    std::vector<std::string> m_candidates;
    std::vector<std::string> m_display;
    std::size_t m_fileCount = 0;
};

} // namespace frost::ui::nav
