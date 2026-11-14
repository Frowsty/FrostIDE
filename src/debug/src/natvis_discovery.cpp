// SPDX-License-Identifier: Apache-2.0
// Deterministic bounded Natvis inventory, independent of debugger/backend availability.
#include "frost/debug/natvis_discovery.hpp"

#include "frost/io/path.hpp"

#include <algorithm>
#include <set>

namespace frost::debug {
namespace {
bool natvis(std::string path) {
    std::ranges::transform(path, path.begin(),
                           [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; });
    return path.ends_with(".natvis");
}
} // namespace
Result<NatvisInventory> discoverNatvis(io::IFileSystem& fs, const NatvisDiscovery& request) {
    if (!request.maxDirectories || request.maxDirectories > 4096 || !request.maxEntries ||
        request.maxEntries > 100000 || !request.maxFiles || request.maxFiles > 64 || request.files.size() > 256 ||
        request.roots.size() > 256 || request.directories.size() > 4096)
        return fail(ErrorCode::OutOfRange, "Natvis discovery limits are invalid");
    NatvisInventory result;
    std::set<std::string> seenFiles, seenDirectories;
    auto add = [&](const std::string& path) -> Result<void> {
        if (!io::isAbsolutePath(path) || !natvis(path))
            return fail(ErrorCode::InvalidArgument, "Natvis requires an absolute .natvis path");
        auto status = fs.status(path);
        if (!status && status.error().code() == ErrorCode::NotFound) {
            result.diagnostics.push_back("Visualizer missing: " + path);
            return {};
        }
        if (!status)
            return std::unexpected(status.error());
        if (status->type != io::FileType::Regular || status->size > 4 * 1024 * 1024)
            return fail(ErrorCode::Unsupported, "Visualizer is not a bounded regular file: " + path);
        auto physical = fs.physicalPath(path);
        if (!physical)
            return std::unexpected(physical.error());
        if (seenFiles.insert(*physical).second) {
            if (result.files.size() == request.maxFiles)
                return fail(ErrorCode::OutOfRange, "Too many visualizers; narrow discovery roots");
            result.files.push_back(path);
        }
        return {};
    };
    for (const auto& path : request.files)
        if (auto added = add(path); !added)
            return std::unexpected(added.error());
    std::vector<std::pair<std::string, bool>> pending;
    for (const auto& root : request.roots)
        pending.emplace_back(root, true);
    for (const auto& directory : request.directories)
        pending.emplace_back(directory, false);
    std::size_t entries = 0;
    for (std::size_t next = 0; next < pending.size(); ++next) {
        const auto [path, recursive] = pending[next];
        if (!io::isAbsolutePath(path))
            return fail(ErrorCode::InvalidArgument, "Visualizer root must be absolute");
        auto physical = fs.physicalPath(path);
        if (!physical && physical.error().code() == ErrorCode::NotFound) {
            result.diagnostics.push_back("Visualizer directory missing: " + path);
            continue;
        }
        if (!physical)
            return std::unexpected(physical.error());
        if (!seenDirectories.insert(*physical + (recursive ? "\nrecursive" : "\nshallow")).second)
            continue;
        if (seenDirectories.size() > request.maxDirectories)
            return fail(ErrorCode::OutOfRange, "Visualizer directory budget exceeded");
        auto children = fs.listDirectory(path);
        if (!children)
            return std::unexpected(children.error());
        std::ranges::sort(*children, {}, &io::DirEntry::name);
        for (const auto& child : *children) {
            if (++entries > request.maxEntries)
                return fail(ErrorCode::OutOfRange, "Visualizer entry budget exceeded");
            auto joined = io::joinPath(path, child.name);
            if (!joined)
                return std::unexpected(joined.error());
            if (child.type == io::FileType::Directory) {
                if (child.name == ".git" || child.name == ".svn" || child.name == "node_modules")
                    continue;
                if (recursive)
                    pending.emplace_back(std::move(*joined), true);
            } else if (child.type == io::FileType::Regular && natvis(child.name)) {
                if (auto added = add(*joined); !added)
                    return std::unexpected(added.error());
            }
        }
    }
    return result;
}
} // namespace frost::debug
