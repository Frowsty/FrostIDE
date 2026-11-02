// SPDX-License-Identifier: Apache-2.0
//
// The VCS status seam the UI colours from (M5, ADR 0048): IVcsStatusProvider, which the Solution
// Explorer, the File System view and the editor tabs query per file, and VcsStatusMap, the
// immutable per-file map one status refresh of one work tree produces. Also the path helpers that
// convert between git's relative '/' paths and FrostIDE's absolute native ones.
/*
 * Keys. Paths are compared by vcsPathKey: normalizePath's spelling, ASCII-lowercased on Windows
 * (NTFS and git for Windows' core.ignorecase compare names without case). Any absolute spelling
 * normalizePath accepts finds its file ("C:/repo/a.cpp" and "c:\repo\a.cpp" are one file there).
 */
#pragma once

#include "frost/vcs/git_status.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace frost::vcs {

// The comparison key of an absolute path (header comment); "" for a path normalizePath refuses.
[[nodiscard]] std::string vcsPathKey(std::string_view absolutePath);

// `relativePath` ('/' separated, as git writes it) below `workTree`, as a normalised absolute native
// path; "" when the result does not normalise.
[[nodiscard]] std::string absoluteGitPath(std::string_view workTree, std::string_view relativePath);

// The '/' separated path of `absolutePath` relative to `workTree`, keeping the absolute path's case;
// nullopt when it is not strictly below the work tree (or does not normalise).
[[nodiscard]] std::optional<std::string> relativeGitPath(std::string_view workTree, std::string_view absolutePath);

/*
 * What the UI models ask per file. Implementations are thread-safe for queries; the GUI-thread
 * controller (frost::ui's GitController) tells the models when generation() moved.
 */
class IVcsStatusProvider {
public:
    /*
     * The status of the file at `absolutePath`: nullopt when the path is in no work tree the provider
     * knows; Unmodified when it is in one and git reports nothing for it. O(path) average.
     */
    [[nodiscard]] virtual std::optional<VcsFileStatus> statusOf(std::string_view absolutePath) const = 0;
    // Increases whenever any status may have changed.
    [[nodiscard]] virtual std::uint64_t generation() const noexcept = 0;

protected:
    IVcsStatusProvider() = default;
    IVcsStatusProvider(const IVcsStatusProvider&) = default;
    IVcsStatusProvider& operator=(const IVcsStatusProvider&) = default;
    ~IVcsStatusProvider() = default;
};

/*
 * The per-file status of one work tree from one GitStatus: each entry's path (a rename's new path)
 * with its VcsFileStatus. Immutable after construction; every query is thread-safe.
 */
class VcsStatusMap final : public IVcsStatusProvider {
public:
    // An empty map that knows no work tree (statusOf is always nullopt).
    VcsStatusMap() = default;
    // The map of `status` for the work tree at `workTree` (absolute). O(entries).
    VcsStatusMap(std::string workTree, const GitStatus& status, std::uint64_t generation);

    [[nodiscard]] std::optional<VcsFileStatus> statusOf(std::string_view absolutePath) const override;
    [[nodiscard]] std::uint64_t generation() const noexcept override { return m_generation; }

    [[nodiscard]] const std::string& workTree() const noexcept { return m_workTree; }
    // The changed files (absolute paths) and their status, sorted by path.
    [[nodiscard]] std::vector<std::pair<std::string, VcsFileStatus>> entries() const;
    [[nodiscard]] std::size_t size() const noexcept { return m_byKey.size(); }

private:
    std::string m_workTree;
    std::string m_workTreeKey;
    std::unordered_map<std::string, std::pair<std::string, VcsFileStatus>> m_byKey; // key -> (path, status)
    std::uint64_t m_generation = 0;
};

} // namespace frost::vcs
