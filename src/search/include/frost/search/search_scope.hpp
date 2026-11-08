// SPDX-License-Identifier: Apache-2.0
//
// The scopes of Find and Replace in Files (ADR 0045): Solution (every file of the open solution,
// Visual Studio's "Entire solution"; the default while one is open), Project (the project of the
// current file), Directory (a folder, recursively, without the default build and tool folders) and
// Open files. resolveScope turns a scope into a ScopeTarget: a file list, or a directory that the
// search engine walks (find_in_files.hpp).
/*
 * Solution and Project come from project::SolutionModel's file nodes (project items and solution
 * items), sorted by path with duplicates removed (a file shared by two projects is searched once).
 * Paths compare with ASCII case ignored on Windows, as the model's lookups do.
 * Directory initially excludes the names in SearchScope::excludes at any depth (ASCII case
 * ignored); .gitignore and .ignore rules can re-include them. Directory walks skip version-control
 * directories, like the project tree. With SearchScope::roles (the open project's directory roles,
 * UX-23, frost/project/directory_roles.hpp) a directory at or below the roles' root is walked as the
 * roles say instead: Generated and Excluded folders are never listed and `excludes` does not apply
 * (a name-only list hid tracked sources such as FrostEngine's Renderer/Debug); a directory outside
 * that root keeps `excludes`.
 * Thread safety: resolveScope only reads its arguments (a loaded model is immutable); any thread.
 */
#pragma once

#include "frost/core/error.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::project {
class DirectoryRoles;
class SolutionModel;
} // namespace frost::project

namespace frost::search {

enum class ScopeKind : std::uint8_t {
    Solution,  // Every file of the open solution.
    Project,   // The files of the project that owns SearchScope::currentFile.
    Directory, // SearchScope::directory and everything below it.
    OpenFiles, // The documents open in editors.
};

// "Solution", "Project", "Directory", "Open files" (English source text for the UI).
[[nodiscard]] std::string_view scopeName(ScopeKind kind) noexcept;

// The directory names a Directory scope skips by default (header comment).
inline constexpr std::array<std::string_view, 10> kDefaultDirectoryExcludes{
    ".git", ".vs", "bin", "obj", "x64", "Debug", "Release", "build", "out", "node_modules"};

struct SearchScope {
    ScopeKind kind = ScopeKind::Solution;
    // Directory: the absolute folder to search.
    std::string directory;
    // Directory: directory names skipped at any depth (ASCII case ignored).
    std::vector<std::string> excludes{kDefaultDirectoryExcludes.begin(), kDefaultDirectoryExcludes.end()};
    // Project: the file whose project is searched (the active editor's).
    std::string currentFile;
    // Optional enclosing project root: apply ancestor ignore files to a nested directory.
    std::string ignoreRoot;
    // Directory: the open project's directory roles (header comment); null: `excludes` only.
    std::shared_ptr<const project::DirectoryRoles> roles;
};

// What a search walks: either `files` in this order, or the tree below `directory`.
struct ScopeTarget {
    std::vector<std::string> files;
    std::optional<std::string> directory;
    std::vector<std::string> excludes; // With `directory`.
    std::string ignoreRoot;
    std::shared_ptr<const project::DirectoryRoles> roles; // With `directory`; may be null.
};

/*
 * Resolves `scope`. `solution` may be null (no solution open); `openFiles` are the paths of the open
 * documents, in tab order. Errors:
 *   - NotFound: Solution or Project without a solution; Project when the current file is in no
 *     project of the solution;
 *   - InvalidArgument: Project without a current file; Directory with a path that is not absolute.
 * O(nodes log nodes) for Solution.
 */
[[nodiscard]] Result<ScopeTarget> resolveScope(const SearchScope& scope, const project::SolutionModel* solution,
                                               const std::vector<std::string>& openFiles);

// The key paths are compared by: the path with '\\' kept, ASCII case folded when it is a Windows
// path (a drive letter or a UNC root). Pure. O(n).
[[nodiscard]] std::string pathKey(std::string_view path);

} // namespace frost::search
