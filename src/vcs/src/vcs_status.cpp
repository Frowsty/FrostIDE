// SPDX-License-Identifier: Apache-2.0
//
// VcsStatusMap and the path helpers (frost/vcs/vcs_status.hpp).

#include "frost/vcs/vcs_status.hpp"

#include "frost/io/path.hpp"

#include "environment.hpp"

#include <algorithm>

namespace frost::vcs {
namespace {

#ifdef _WIN32
constexpr char kSeparator = '\\';
#else
constexpr char kSeparator = '/';
#endif

} // namespace

std::string vcsPathKey(std::string_view absolutePath) {
    auto normalized = io::normalizePath(absolutePath);
    if (!normalized) {
        return {};
    }
#ifdef _WIN32
    return detail::asciiLower(*normalized);
#else
    return std::move(*normalized);
#endif
}

std::string absoluteGitPath(std::string_view workTree, std::string_view relativePath) {
    std::string joined(workTree);
    if (!joined.empty() && joined.back() != '/' && joined.back() != '\\') {
        joined.push_back(kSeparator);
    }
    for (const char c : relativePath) {
        joined.push_back(c == '/' ? kSeparator : c);
    }
    auto normalized = io::normalizePath(joined);
    return normalized ? std::move(*normalized) : std::string();
}

std::optional<std::string> relativeGitPath(std::string_view workTree, std::string_view absolutePath) {
    auto root = io::normalizePath(workTree);
    auto path = io::normalizePath(absolutePath);
    if (!root || !path) {
        return std::nullopt;
    }
#ifdef _WIN32
    const std::string rootKey = detail::asciiLower(*root);
    const std::string pathKey = detail::asciiLower(*path);
#else
    const std::string& rootKey = *root;
    const std::string& pathKey = *path;
#endif
    std::size_t prefix = rootKey.size();
    if (!rootKey.empty() && rootKey.back() == kSeparator) {
        --prefix; // A drive root ("C:\") keeps its separator.
    }
    if (pathKey.size() <= prefix + 1 || pathKey.compare(0, prefix, rootKey, 0, prefix) != 0 ||
        pathKey[prefix] != kSeparator) {
        return std::nullopt;
    }
    std::string relative = path->substr(prefix + 1);
    std::ranges::replace(relative, '\\', '/');
    return relative;
}

VcsStatusMap::VcsStatusMap(std::string workTree, const GitStatus& status, std::uint64_t generation)
    : m_workTree(std::move(workTree)), m_workTreeKey(vcsPathKey(m_workTree)), m_generation(generation) {
    for (const GitStatusEntry& entry : status.entries) {
        const VcsFileStatus fileStatus = entry.fileStatus();
        if (fileStatus == VcsFileStatus::Unmodified) {
            continue;
        }
        std::string path = absoluteGitPath(m_workTree, entry.path);
        if (path.empty()) {
            continue;
        }
        std::string key = vcsPathKey(path);
        m_byKey.insert_or_assign(std::move(key), std::pair{std::move(path), fileStatus});
    }
}

std::optional<VcsFileStatus> VcsStatusMap::statusOf(std::string_view absolutePath) const {
    if (m_workTreeKey.empty()) {
        return std::nullopt;
    }
    const std::string key = vcsPathKey(absolutePath);
    if (key.empty()) {
        return std::nullopt;
    }
    if (const auto it = m_byKey.find(key); it != m_byKey.end()) {
        return it->second.second;
    }
    // Inside the work tree (the root itself included): tracked and clean, or a directory.
    std::size_t prefix = m_workTreeKey.size();
    if (m_workTreeKey.back() == kSeparator) {
        --prefix;
    }
    const bool inside =
        key.compare(0, prefix, m_workTreeKey, 0, prefix) == 0 && (key.size() == prefix || key[prefix] == kSeparator);
    if (!inside) {
        return std::nullopt;
    }
    return VcsFileStatus::Unmodified;
}

std::vector<std::pair<std::string, VcsFileStatus>> VcsStatusMap::entries() const {
    std::vector<std::pair<std::string, VcsFileStatus>> out;
    out.reserve(m_byKey.size());
    for (const auto& [key, value] : m_byKey) {
        out.push_back(value);
    }
    std::ranges::sort(out, {}, &std::pair<std::string, VcsFileStatus>::first);
    return out;
}

} // namespace frost::vcs
