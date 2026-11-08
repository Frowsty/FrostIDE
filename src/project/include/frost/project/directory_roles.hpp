// SPDX-License-Identifier: Apache-2.0
//
// Directory roles (UX-23, PLAN.md Appendix C R1; PLAN.md rows 23 and 86): what each folder of an open
// project is for, and so whether the project tree and Go to File (the project index), Find in Files
// and the file watcher traverse it. FrostEngine's build/windows-debug alone is 49 GB and its .cache
// 40 GB; walking them made every search and every watcher setup crawl.
/*
 * Roles. Source (the default) and Library are traversed: listed, searched and watched. Generated
 * (build output, CMake binary directories, download caches) and Excluded are not: a walk reports
 * the folder itself but never lists it, and the watcher reports nothing below it. Library marks
 * third-party code the user still wants searched; it is never a default.
 *
 * The decision for one folder (decide), first match wins:
 *   1. Version-control and IDE state folders, by name at any depth: .git, .svn, .hg and .vs are
 *      Excluded and cannot be overridden (they are never project content).
 *   2. A persisted user override for exactly this folder (DirectoryRoleStore).
 *   3. A CMake binary directory named by a configure preset of the root's CMakePresets.json or
 *      CMakeUserPresets.json (Options::generatedDirectories; load() reads them): Generated.
 *   4. By name at any depth, ASCII case exact: build, out, .cache and cmake-build-* are Generated.
 *      The exact case matters: FrostEngine tracks source folders named Build (its game export
 *      module), which a case-folding rule would hide.
 *   5. A folder below the root holding a CMakeCache.txt (a CMake binary directory not named by a
 *      preset): Generated.
 *   6. MSBuild output folders: bin, obj, ipch, x64, x86, ARM64, Debug and Release (ASCII case
 *      ignored, as MSBuild writes them) are Generated only when their parent folder holds a
 *      solution or project file (.sln, .slnx, .vcxproj, .vcxitems, .csproj, .vbproj, .fsproj).
 *      FrostEngine tracks a source folder Frost/src/Frost/Renderer/Debug; a name-only rule would
 *      hide it from search.
 *   7. Otherwise Source.
 * The root itself is Source unless overridden. A folder's effective role (roleOf) is the first
 * non-traversed decision on the way down from the root, else the deepest non-default decision
 * (a Library folder's subfolders are Library).
 *
 * Paths are absolute in the file system's native syntax; separators '/' and '\\' are both accepted
 * and, on Windows paths (a drive letter or UNC root), ASCII case is ignored when comparing. Rules 5
 * and 6 read the file system (one status() or one listing per folder) and are cached per folder for
 * the object's lifetime; build a new object (load()) when the project's CMake state changes.
 *
 * Thread safety: a DirectoryRoles is shared immutable state plus an internally locked cache; every
 * member may be called from any thread (scanTree's descend runs on scheduler threads, the watcher's
 * filter on reactor threads). The file system outlives the object. DirectoryRoleStore locks
 * internally too.
 */
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::project {

enum class DirectoryRole : std::uint8_t { Source, Library, Generated, Excluded };

// "source", "library", "generated", "excluded" (the persisted spelling and the log's).
[[nodiscard]] std::string_view directoryRoleName(DirectoryRole role) noexcept;
// The inverse of directoryRoleName; nullopt for anything else.
[[nodiscard]] std::optional<DirectoryRole> parseDirectoryRole(std::string_view name) noexcept;
// Source and Library folders are walked, searched and watched.
[[nodiscard]] constexpr bool isTraversed(DirectoryRole role) noexcept {
    return role == DirectoryRole::Source || role == DirectoryRole::Library;
}

// Which rule of the header comment decided a folder's role.
enum class DirectoryRoleReason : std::uint8_t {
    Default,        // Rule 7.
    VersionControl, // Rule 1.
    Override,       // Rule 2.
    CMakePreset,    // Rule 3.
    BuildName,      // Rule 4.
    CMakeCache,     // Rule 5.
    MSBuildOutput,  // Rule 6.
};

struct DirectoryRoleDecision {
    DirectoryRole role = DirectoryRole::Source;
    DirectoryRoleReason reason = DirectoryRoleReason::Default;

    friend bool operator==(const DirectoryRoleDecision&, const DirectoryRoleDecision&) = default;
};

// A user's role for one folder (absolute, normalised).
struct DirectoryRoleOverride {
    std::string path;
    DirectoryRole role = DirectoryRole::Source;

    friend bool operator==(const DirectoryRoleOverride&, const DirectoryRoleOverride&) = default;
};

class DirectoryRoles {
public:
    struct Options {
        // CMake binary directories (absolute) that are Generated (rule 3).
        std::vector<std::string> generatedDirectories{};
        // User overrides (rule 2); entries outside the root are ignored.
        std::vector<DirectoryRoleOverride> overrides{};
    };

    /*
     * The roles of the project at `root` (absolute). Errors: InvalidArgument (a relative root). Never
     * reads the file system itself; decide() does (header comment).
     */
    [[nodiscard]] static Result<std::shared_ptr<const DirectoryRoles>> create(io::IFileSystem& fs, std::string root,
                                                                              Options options);
    /*
     * create() with rule 3 filled from the root's CMake presets (every configure preset's binaryDir,
     * hidden and disabled ones included: the folders exist whichever preset is active). Reading
     * presets runs nothing (cmake_presets.hpp), so this is allowed before the folder is trusted. A
     * presets file that does not parse leaves rule 3 empty and is reported by presetsProblem().
     * Errors: create()'s.
     */
    [[nodiscard]] static Result<std::shared_ptr<const DirectoryRoles>>
    load(io::IFileSystem& fs, std::string root, std::vector<DirectoryRoleOverride> overrides = {});

    DirectoryRoles(const DirectoryRoles&) = delete ("shared by pointer");
    DirectoryRoles& operator=(const DirectoryRoles&) = delete ("shared by pointer");
    ~DirectoryRoles();

    [[nodiscard]] const std::string& root() const noexcept;
    [[nodiscard]] const std::vector<std::string>& generatedDirectories() const noexcept;
    [[nodiscard]] const std::vector<DirectoryRoleOverride>& overrides() const noexcept;
    // Why load() found no CMake binary directories although a presets file exists; "" otherwise.
    [[nodiscard]] const std::string& presetsProblem() const noexcept;

    // The decision for the folder `directory` itself (header comment); a path outside the root is
    // Source/Default. O(path length + overrides + generated) plus, uncached, one stat or listing.
    [[nodiscard]] DirectoryRoleDecision decide(std::string_view directory) const;
    // The effective role of `directory` (header comment). O(depth x decide).
    [[nodiscard]] DirectoryRole roleOf(std::string_view directory) const;
    // True when a walk that reached `directory` lists it: isTraversed(decide(directory).role).
    [[nodiscard]] bool traversed(std::string_view directory) const;
    // True when `path` (a file or folder) lies strictly below a folder that is not traversed, the
    // root excluded: what the watcher filters. O(depth x decide).
    [[nodiscard]] bool belowUntraversed(std::string_view path) const;

    struct State; // directory_roles.cpp.

private:
    explicit DirectoryRoles(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> m_state;
};

/*
 * The persisted overrides: one small UTF-8 text file in the user data directory, beside the trust
 * store ("directory-roles.txt"): a header line "# FrostIDE directory roles v1", then one
 * "<role>\t<absolute folder>" line per override. Never written into a project. Written with
 * IFileSystem::writeFileAtomic; set() re-reads the file first, so overrides another FrostIDE
 * instance saved meanwhile are kept. Lines that do not parse are skipped (warnings()); a file whose
 * header names another version is Unsupported and never rewritten.
 */
class DirectoryRoleStore {
public:
    // Opens the store kept in `path` (absolute; need not exist yet). Errors: InvalidArgument (a
    // relative path), Unsupported (another version), readFile's errors other than NotFound.
    [[nodiscard]] static Result<std::unique_ptr<DirectoryRoleStore>> open(io::IFileSystem& fs, std::string path);
    // An in-memory store (tests, no user profile): set() keeps overrides for this process only.
    [[nodiscard]] static std::unique_ptr<DirectoryRoleStore> inMemory(io::IFileSystem& fs);

    DirectoryRoleStore(const DirectoryRoleStore&) = delete ("owns the file's state");
    DirectoryRoleStore& operator=(const DirectoryRoleStore&) = delete ("owns the file's state");
    ~DirectoryRoleStore();

    // The overrides at or below `root` (absolute), sorted by path.
    [[nodiscard]] std::vector<DirectoryRoleOverride> overridesFor(std::string_view root) const;
    /*
     * Sets (or with nullopt removes) the override of `directory` (absolute) and saves. Errors:
     * InvalidArgument (relative, NUL or line breaks), Unsupported, the file errors of
     * createDirectories and writeFileAtomic (the overrides are unchanged then).
     */
    [[nodiscard]] Result<void> set(std::string_view directory, std::optional<DirectoryRole> role);
    [[nodiscard]] std::vector<std::string> warnings() const;

    struct State; // directory_roles.cpp.

private:
    explicit DirectoryRoleStore(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> m_state;
};

} // namespace frost::project
