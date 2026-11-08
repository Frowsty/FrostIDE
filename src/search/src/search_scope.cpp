// SPDX-License-Identifier: Apache-2.0
//
// resolveScope and pathKey (search_scope.hpp): the Solution and Project scopes from the
// SolutionModel's file nodes, Directory and Open files as given.

#include "frost/search/search_scope.hpp"

#include "frost/io/path.hpp"
#include "frost/project/solution_model.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace frost::search {
namespace {

// Appends the paths of every file node below `from` (included) to `out`.
void collectFiles(const project::SolutionModel& model, project::NodeId from, std::vector<std::string>& out) {
    std::vector<project::NodeId> stack{from};
    while (!stack.empty()) {
        const project::NodeId id = stack.back();
        stack.pop_back();
        const project::Node& node = model.node(id);
        if (node.kind == project::NodeKind::File && !node.path.empty()) {
            out.push_back(node.path);
        }
        for (const project::NodeId child : node.children) {
            stack.push_back(child);
        }
    }
}

// Sorts by path key and drops later duplicates of a key.
std::vector<std::string> sortedUnique(std::vector<std::string> paths) {
    std::vector<std::pair<std::string, std::string>> keyed;
    keyed.reserve(paths.size());
    for (std::string& path : paths) {
        std::string key = pathKey(path);
        keyed.emplace_back(std::move(key), std::move(path));
    }
    std::ranges::stable_sort(keyed, {}, &std::pair<std::string, std::string>::first);
    std::vector<std::string> out;
    out.reserve(keyed.size());
    const std::string* previous = nullptr;
    for (auto& [key, path] : keyed) {
        if (previous != nullptr && *previous == key) {
            continue;
        }
        previous = &key;
        out.push_back(std::move(path));
    }
    return out;
}

} // namespace

std::string_view scopeName(ScopeKind kind) noexcept {
    switch (kind) {
    case ScopeKind::Solution: return "Solution";
    case ScopeKind::Project: return "Project";
    case ScopeKind::Directory: return "Directory";
    case ScopeKind::OpenFiles: return "Open files";
    }
    return "";
}

std::string pathKey(std::string_view path) {
    std::string key(path);
    if (io::isAbsolutePath(path, io::PathStyle::Windows)) {
        for (char& c : key) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            } else if (c == '/') {
                c = '\\';
            }
        }
    }
    return key;
}

Result<ScopeTarget> resolveScope(const SearchScope& scope, const project::SolutionModel* solution,
                                 const std::vector<std::string>& openFiles) {
    ScopeTarget target;
    switch (scope.kind) {
    case ScopeKind::Solution: {
        if (solution == nullptr) {
            return fail(ErrorCode::NotFound, "no solution is open");
        }
        collectFiles(*solution, project::SolutionModel::root(), target.files);
        target.files = sortedUnique(std::move(target.files));
        return target;
    }
    case ScopeKind::Project: {
        if (solution == nullptr) {
            return fail(ErrorCode::NotFound, "no solution is open");
        }
        if (scope.currentFile.empty()) {
            return fail(ErrorCode::InvalidArgument, "the Project scope needs a current file");
        }
        const std::optional<project::ProjectId> owner = solution->owningProject(scope.currentFile);
        if (!owner) {
            return fail(ErrorCode::NotFound,
                        std::format("'{}' does not belong to a project of {}", scope.currentFile, solution->name()));
        }
        collectFiles(*solution, solution->project(*owner).node, target.files);
        target.files = sortedUnique(std::move(target.files));
        return target;
    }
    case ScopeKind::Directory: {
        if (!io::isAbsolutePath(scope.directory, io::PathStyle::Windows) &&
            !io::isAbsolutePath(scope.directory, io::PathStyle::Posix)) {
            return fail(ErrorCode::InvalidArgument, std::format("'{}' is not an absolute directory", scope.directory));
        }
        target.directory = scope.directory;
        target.excludes = scope.excludes;
        target.ignoreRoot = scope.ignoreRoot;
        target.roles = scope.roles;
        return target;
    }
    case ScopeKind::OpenFiles: {
        std::set<std::string> seen;
        for (const std::string& path : openFiles) {
            if (seen.insert(pathKey(path)).second) {
                target.files.push_back(path);
            }
        }
        return target;
    }
    }
    return fail(ErrorCode::InvalidArgument, "unknown scope");
}

} // namespace frost::search
