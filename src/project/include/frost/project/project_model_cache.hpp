// SPDX-License-Identifier: Apache-2.0
//
// ProjectModelCache (SLN-4): MSBuild's evaluation of a project configuration (its items, and its
// GetClCommandLines when SLN-5 asked for them) kept on disk between sessions, so a warm open of a
// solution shows MSBuild's model without evaluating every project again. SolutionEvaluation
// (project_host.hpp) reads and writes it.
/*
 * An entry is valid only while everything the evaluation is known to have read is unchanged:
 *
 *   - the key: solution, project, project configuration and platform (exact strings);
 *   - the global properties sent to MSBuild (evaluationGlobalProperties), in order;
 *   - the MSBuild identity: the path, size and modification time of MSBuild.exe and of the
 *     Microsoft.Build.dll beside it (a Visual Studio update changes them);
 *   - the environment fingerprint: a hash of the environment the evaluator inherits (this
 *     process's, plus the variables the caller adds), since MSBuild exposes every variable as a
 *     property (environmentFingerprint);
 *   - the size and modification time of the project file and of every import the evaluation
 *     reported, so editing, replacing or deleting a .props/.targets file invalidates the entry
 *     ("invalidated when imports change");
 *   - for every include glob ("src\*.cpp", "**\*.h"), the names below its fixed directory (the
 *     part before the first wildcard; the whole subtree when a directory part has a wildcard or
 *     is "**"), so adding, removing or renaming a file a glob could match invalidates the entry.
 *
 * What the checks cannot see: an Exists() condition on a file that was not imported, or a property
 * function that reads a file. SolutionEvaluation therefore revalidates every entry it served
 * (SolutionEvaluationOptions::revalidateCached) once the solution is shown, and replaces the ones
 * that differ. Only ProjectHost results are stored: the MSBuild.exe fallback reports no imports,
 * so its entries could not be checked.
 *
 * Storage: one JSON file per key under the cache directory, named by a stable 64-bit FNV-1a hash
 * of the key (a collision is detected by comparing the stored key and reads as a miss), written
 * with IFileSystem::writeFileAtomic. An unreadable, foreign-version or damaged file is a miss.
 *
 * Thread safety: none needed by SolutionEvaluation (one worker); concurrent use of one object needs
 * external locking. Different objects over one directory are safe (atomic writes, last one wins).
 * Costs: lookup() is one read, one parse, one stat per recorded file and one directory walk per
 * glob; store() one write.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/project/cl_command_lines.hpp"
#include "frost/project/solution_model.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::project {

// Which evaluation an entry is for.
struct ProjectModelCacheKey {
    std::string solutionPath; // Absolute; may be "".
    std::string projectPath;  // Absolute .vcxproj.
    std::string configuration;
    std::string platform;

    friend bool operator==(const ProjectModelCacheKey&, const ProjectModelCacheKey&) = default;
};

// The inputs of an evaluation besides its files (see the header comment).
struct EvaluationInputs {
    std::vector<std::pair<std::string, std::string>> globalProperties;
    std::string msbuildIdentity; // msbuildIdentity()'s value.
    std::string environment;     // environmentFingerprint()'s value.

    friend bool operator==(const EvaluationInputs&, const EvaluationInputs&) = default;
};

// What an entry holds.
struct CachedProjectModel {
    EvaluatedProjectItems items;
    std::optional<ProjectClCommandLines> clCommandLines = std::nullopt;
};

// "<path>|<size>|<mtime>" of MSBuild.exe and of its Microsoft.Build.dll. Errors: those of
// IFileSystem::status (the caller then does not use the cache).
[[nodiscard]] Result<std::string> msbuildIdentity(io::IFileSystem& fs, std::string_view msbuildPath);

// A hash of this process's environment with `extra` applied (a nullopt value removes the
// variable), names compared without case on Windows. Stable for a given environment. O(size).
[[nodiscard]] std::string
environmentFingerprint(std::span<const std::pair<std::string, std::optional<std::string>>> extra = {});

class ProjectModelCache {
public:
    // `fs` must outlive the cache; `directory` is absolute and created on the first store().
    ProjectModelCache(io::IFileSystem& fs, std::string directory);

    // The entry for `key` if it is valid for `inputs` (see the header comment), else nullopt;
    // `whyNot`, when given, receives the reason of a miss ("no entry", "Common.props changed").
    // Never fails: a problem reading the entry is a miss.
    [[nodiscard]] std::optional<CachedProjectModel>
    lookup(const ProjectModelCacheKey& key, const EvaluationInputs& inputs, std::string* whyNot = nullptr);

    /*
     * Stores `model` for `key`, recording the stamps of the project file and of
     * model.items.imports, and the listings of `globs` (relative to the project directory or
     * absolute). Errors: InvalidArgument (a path that is not absolute), the file system's (stat of
     * an import, the write); nothing is stored then.
     */
    [[nodiscard]] Result<void> store(const ProjectModelCacheKey& key, const EvaluationInputs& inputs,
                                     const CachedProjectModel& model, std::span<const std::string> globs);

    // Removes the entry for `key` if there is one. Errors: the file system's, other than NotFound.
    [[nodiscard]] Result<void> remove(const ProjectModelCacheKey& key);

    [[nodiscard]] const std::string& directory() const noexcept { return m_directory; }
    // The entry file of `key` (for tests and the log).
    [[nodiscard]] std::string entryPath(const ProjectModelCacheKey& key) const;

private:
    io::IFileSystem& m_fs;
    std::string m_directory;
};

} // namespace frost::project
