// SPDX-License-Identifier: Apache-2.0
//
// Private path helpers of frost::project (paths.hpp): MSBuild-style resolution, keys and
// wildcard expansion.
#include "paths.hpp"

#include "frost/io/file_system.hpp"

#include "text_util.hpp"

#include <format>
#include <map>

namespace frost::project::detail {
namespace {

constexpr bool kWindows = io::kNativePathStyle == io::PathStyle::Windows;
// The separators a pattern may use once toNativeSeparators has run.
constexpr std::string_view kPatternSeparators = kWindows ? "\\/" : "/";

[[nodiscard]] bool sameChar(char a, char b) noexcept {
    if constexpr (kWindows) {
        return toLowerAscii(a) == toLowerAscii(b);
    } else {
        return a == b;
    }
}

[[nodiscard]] std::vector<std::string_view> splitPath(std::string_view path, std::string_view separators) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find_first_of(separators, start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        if (end > start) {
            out.push_back(path.substr(start, end - start));
        }
        start = end + 1;
    }
    return out;
}

// Matches path components [j, end) against pattern components [i, end); "**" spans any number of
// components (zero included). Exponential only for many "**" in one pattern, which the callers cap.
[[nodiscard]] bool globMatch(const std::vector<std::string>& pattern, std::size_t i,
                             const std::vector<std::string_view>& path, std::size_t j) {
    if (i == pattern.size()) {
        return j == path.size();
    }
    if (pattern[i] == "**") {
        for (std::size_t k = j; k <= path.size(); ++k) {
            if (globMatch(pattern, i + 1, path, k)) {
                return true;
            }
        }
        return false;
    }
    return j < path.size() && matchName(pattern[i], path[j]) && globMatch(pattern, i + 1, path, j + 1);
}

// State of one expandWildcard walk.
struct Walker {
    Walker(io::IFileSystem& fileSystem, const std::vector<std::string>& patternComponents,
           const WildcardLimits& walkLimits)
        : fs(fileSystem), components(patternComponents), limits(walkLimits) {}

    io::IFileSystem& fs;
    const std::vector<std::string>& components;
    const WildcardLimits& limits;
    std::map<std::string, std::string> files; // Key -> path, so the output is sorted and unique.
    std::size_t directories = 0;
    bool truncated = false;
    std::string firstError;

    void addFile(std::string path) {
        if (files.size() >= limits.maxFiles) {
            truncated = true;
            return;
        }
        std::string key = pathKey(path);
        files.emplace(std::move(key), std::move(path));
    }

    // Whether `entry` (at `path`) is a file a pattern may match; symlinks to files count.
    [[nodiscard]] bool isFile(const io::DirEntry& entry, const std::string& path) {
        if (entry.type == io::FileType::Regular) {
            return true;
        }
        if (entry.type != io::FileType::Symlink) {
            return false;
        }
        auto status = fs.status(path);
        return status && status->type == io::FileType::Regular;
    }

    void walk(const std::string& dir, std::size_t i, bool top) {
        if (truncated || limits.stop.stop_requested()) {
            return;
        }
        if (++directories > limits.maxDirectories) {
            truncated = true;
            return;
        }
        auto entries = fs.listDirectory(dir);
        if (!entries) {
            if (!(top && entries.error().code() == ErrorCode::NotFound) && firstError.empty()) {
                firstError = entries.error().message();
            }
            return;
        }
        const std::string& component = components[i];
        const bool last = i + 1 == components.size();
        if (component == "**" && !last) {
            walk(dir, i + 1, false); // "**" spanning no directory.
        }
        for (const io::DirEntry& entry : *entries) {
            if (limits.stop.stop_requested())
                return;
            auto child = io::joinPath(dir, entry.name);
            if (!child) {
                continue;
            }
            if (component == "**") {
                if (entry.type == io::FileType::Directory) {
                    walk(*child, i, false);
                } else if (last && isFile(entry, *child)) {
                    addFile(std::move(*child));
                }
            } else if (last) {
                if (matchName(component, entry.name) && isFile(entry, *child)) {
                    addFile(std::move(*child));
                }
            } else if (entry.type == io::FileType::Directory && matchName(component, entry.name)) {
                walk(*child, i + 1, false);
            }
            if (truncated) {
                return;
            }
        }
    }
};

} // namespace

std::string toNativeSeparators(std::string_view path) {
    std::string out(path);
    if constexpr (!kWindows) {
        for (char& c : out) {
            if (c == '\\') {
                c = '/';
            }
        }
    }
    return out;
}

std::string resolveDotDot(std::string_view normalized) {
    if constexpr (kWindows) {
        return std::string(normalized);
    } else {
        std::vector<std::string_view> stack;
        for (std::string_view component : splitPath(normalized, "/")) {
            if (component == ".") {
                continue;
            }
            if (component == "..") {
                if (!stack.empty()) {
                    stack.pop_back();
                }
                continue;
            }
            stack.push_back(component);
        }
        std::string out;
        for (std::string_view component : stack) {
            out += '/';
            out += component;
        }
        return out.empty() ? std::string("/") : out;
    }
}

Result<std::string> normalizeAbsolute(std::string_view path) {
    auto normalized = io::normalizePath(path);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    return resolveDotDot(*normalized);
}

Result<std::string> resolvePath(std::string_view baseDir, std::string_view path) {
    const std::string native = toNativeSeparators(trim(path));
    if (native.empty()) {
        return fail(ErrorCode::InvalidArgument, "empty path");
    }
    if (io::isAbsolutePath(native)) {
        return normalizeAbsolute(native);
    }
    auto joined = io::joinPath(baseDir, native);
    if (!joined) {
        return std::unexpected(std::move(joined.error()));
    }
    return normalizeAbsolute(*joined);
}

std::string pathKey(std::string_view normalized) {
    if constexpr (kWindows) {
        return toLowerAscii(normalized);
    } else {
        return std::string(normalized);
    }
}

bool isSameOrUnder(std::string_view path, std::string_view root) {
    const std::string p = pathKey(path);
    const std::string r = pathKey(root);
    if (r.empty() || !p.starts_with(r)) {
        return false;
    }
    if (p.size() == r.size() || r.back() == kNativeSeparator) {
        return true;
    }
    return p[r.size()] == kNativeSeparator;
}

bool hasWildcard(std::string_view text) noexcept { return text.find_first_of("*?") != std::string_view::npos; }

bool matchName(std::string_view pattern, std::string_view name) noexcept {
    // Iterative matching with one backtrack point (the last '*'), linear in practice.
    std::size_t p = 0;
    std::size_t n = 0;
    std::size_t star = std::string_view::npos;
    std::size_t starName = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] != '*' && (pattern[p] == '?' || sameChar(pattern[p], name[n]))) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            starName = n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++starName;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

Result<PathPattern> PathPattern::make(std::string_view baseDir, std::string_view pattern) {
    const std::string native = toNativeSeparators(trim(pattern));
    PathPattern out;
    if (!hasWildcard(native)) {
        auto resolved = resolvePath(baseDir, native);
        if (!resolved) {
            return std::unexpected(std::move(resolved.error()));
        }
        out.m_directory = std::move(*resolved);
        return out;
    }
    const std::size_t wild = native.find_first_of("*?");
    const std::size_t separator = native.find_last_of(kPatternSeparators, wild);
    const std::string_view prefix =
        separator == std::string::npos ? std::string_view{} : std::string_view(native).substr(0, separator + 1);
    const std::string_view rest =
        separator == std::string::npos ? std::string_view(native) : std::string_view(native).substr(separator + 1);
    if (prefix.empty()) {
        auto dir = normalizeAbsolute(baseDir);
        if (!dir) {
            return std::unexpected(std::move(dir.error()));
        }
        out.m_directory = std::move(*dir);
    } else {
        auto dir = resolvePath(baseDir, prefix);
        if (!dir) {
            return std::unexpected(std::move(dir.error()));
        }
        out.m_directory = std::move(*dir);
    }
    for (std::string_view component : splitPath(rest, kPatternSeparators)) {
        if (component == "." || component == "..") {
            return fail(ErrorCode::InvalidArgument,
                        std::format("wildcard pattern '{}' has a '{}' after its first wildcard", pattern, component));
        }
        if (component == "**" && !out.m_components.empty() && out.m_components.back() == "**") {
            continue; // "**/**" is the same as "**".
        }
        out.m_components.emplace_back(component);
    }
    if (out.m_components.empty()) {
        return fail(ErrorCode::InvalidArgument, std::format("wildcard pattern '{}' names no file", pattern));
    }
    return out;
}

bool PathPattern::matches(std::string_view path) const {
    if (m_components.empty()) {
        return pathKey(path) == pathKey(m_directory);
    }
    if (!isSameOrUnder(path, m_directory) || path.size() == m_directory.size()) {
        return false;
    }
    std::string_view remainder = path.substr(m_directory.size());
    const std::vector<std::string_view> components = splitPath(remainder, std::string_view(&kNativeSeparator, 1));
    return globMatch(m_components, 0, components, 0);
}

Result<WildcardExpansion> expandWildcard(io::IFileSystem& fs, const PathPattern& pattern,
                                         const WildcardLimits& limits) {
    WildcardExpansion out;
    if (pattern.components().empty()) {
        auto status = fs.status(pattern.directory());
        if (status && status->type == io::FileType::Regular) {
            out.paths.push_back(pattern.directory());
        }
        return out;
    }
    bool recursive = false;
    for (const std::string& component : pattern.components()) {
        recursive = recursive || component == "**";
    }
    if (recursive && io::parentPath(pattern.directory()).empty()) {
        out.warning =
            std::format("refused to expand a recursive wildcard directly under the root {}", pattern.directory());
        return out;
    }
    Walker walker(fs, pattern.components(), limits);
    walker.walk(pattern.directory(), 0, true);
    if (limits.stop.stop_requested())
        return fail(ErrorCode::Cancelled, "project wildcard expansion cancelled");
    out.paths.reserve(walker.files.size());
    for (auto& [key, path] : walker.files) {
        out.paths.push_back(std::move(path));
    }
    if (walker.truncated) {
        out.warning = std::format("wildcard under {} stopped after {} directories and {} files", pattern.directory(),
                                  walker.directories, walker.files.size());
    } else if (!walker.firstError.empty()) {
        out.warning = std::format("wildcard under {}: {}", pattern.directory(), walker.firstError);
    }
    return out;
}

} // namespace frost::project::detail
