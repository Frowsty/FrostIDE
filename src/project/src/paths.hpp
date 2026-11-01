// SPDX-License-Identifier: Apache-2.0
//
// Private path helpers of frost::project: MSBuild-style resolution of project-relative paths,
// lookup keys, and the '*', '?' and "**" wildcards of item Includes and Excludes.
//
/*
 * MSBuild resolves an item path with Path.GetFullPath against the project directory: '\' and '/'
 * both separate, and ".." is resolved lexically on every platform. resolvePath does the same on
 * top of io::normalizePath (which already resolves ".." lexically on Windows but keeps it on
 * POSIX). Keys (pathKey) lower-case ASCII on Windows, where NTFS compares names
 * case-insensitively, and are the path itself on POSIX. Every function is thread-safe; the
 * wildcard functions only list directories through the given IFileSystem.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/path.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::project::detail {

inline constexpr char kNativeSeparator = io::kNativePathStyle == io::PathStyle::Windows ? '\\' : '/';

// The path with MSBuild's '\' turned into '/' on POSIX; unchanged on Windows.
[[nodiscard]] std::string toNativeSeparators(std::string_view path);

// Resolves "." and ".." of an absolute path lexically (POSIX; Windows paths from normalizePath
// are already resolved), stopping at the root.
[[nodiscard]] std::string resolveDotDot(std::string_view normalized);

// `path` (as written in a project or solution file, relative or absolute) resolved against the
// absolute directory `baseDir`. Errors: InvalidArgument (empty, NUL, names Windows refuses).
[[nodiscard]] Result<std::string> resolvePath(std::string_view baseDir, std::string_view path);

// normalizePath plus resolveDotDot for an absolute path. Errors: those of normalizePath.
[[nodiscard]] Result<std::string> normalizeAbsolute(std::string_view path);

// The lookup key of a normalised path (see above).
[[nodiscard]] std::string pathKey(std::string_view normalized);

// True when `path` equals `root` or lies below it; both normalised. Case as pathKey.
[[nodiscard]] bool isSameOrUnder(std::string_view path, std::string_view root);

// True when `text` contains '*' or '?'.
[[nodiscard]] bool hasWildcard(std::string_view text) noexcept;

// Matches one name against a pattern of literal characters, '*' and '?'. Case as pathKey.
[[nodiscard]] bool matchName(std::string_view pattern, std::string_view name) noexcept;

// A wildcard pattern split into its literal directory and the components after it.
class PathPattern {
public:
    // Compiles `pattern` (relative to `baseDir` or absolute). Errors: InvalidArgument when the
    // literal part does not resolve or the wildcard part holds "." or ".." components.
    [[nodiscard]] static Result<PathPattern> make(std::string_view baseDir, std::string_view pattern);

    // True when the normalised absolute `path` matches.
    [[nodiscard]] bool matches(std::string_view path) const;

    [[nodiscard]] const std::string& directory() const noexcept { return m_directory; }
    [[nodiscard]] const std::vector<std::string>& components() const noexcept { return m_components; }

private:
    std::string m_directory;               // The literal part, normalised.
    std::vector<std::string> m_components; // The rest; empty for a pattern without wildcards.
};

struct WildcardLimits {
    std::size_t maxDirectories = 20'000;
    std::size_t maxFiles = 100'000;
};

struct WildcardExpansion {
    std::vector<std::string> paths; // Normalised, sorted, without duplicates.
    std::string warning;            // Set when the walk was refused, cut short or hit errors.
};

// The files matching `pattern`. A missing literal directory gives no files and no warning.
// Errors: those of PathPattern::make.
[[nodiscard]] Result<WildcardExpansion> expandWildcard(io::IFileSystem& fs, const PathPattern& pattern,
                                                       const WildcardLimits& limits = {});

} // namespace frost::project::detail
