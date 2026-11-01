// SPDX-License-Identifier: Apache-2.0
//
/*
 * WatchMirror: the reference consumer of file-watch batches for tests (ADR 0020). It keeps a model of a
 * watched tree (path -> type and content) and applies FileChangeBatch values exactly as
 * frost/io/file_watcher.hpp tells consumers to, re-reading through an IFileSystem. Property and
 * integration tests drive changes, apply every batch (possibly while the disk has already moved on) and
 * finally compare tree() with snapshot(): the watcher is correct when they match after it went quiet.
 */
#pragma once

#include "frost/io/file_system.hpp"
#include "frost/io/file_watcher.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::test {

struct MirrorNode {
    io::FileType type = io::FileType::Regular;
    std::string content{}; // Regular files only, when the mirror reads content.

    friend bool operator==(const MirrorNode&, const MirrorNode&) = default;
    // For gtest's failure messages (found by argument-dependent lookup).
    friend void PrintTo(const MirrorNode& node, std::ostream* out) {
        *out << (node.type == io::FileType::Directory ? "dir" : "file");
        if (node.type != io::FileType::Directory) {
            *out << " '" << node.content << "'";
        }
    }
};

// One change, for traces in test failure messages.
inline std::string describe(const io::FileChange& change) {
    static constexpr std::array<const char*, 5> kinds{"Created", "Modified", "Removed", "Renamed", "Rescan"};
    std::string text = std::string(kinds[static_cast<std::size_t>(change.kind)]) + " " + change.path;
    if (change.kind == io::FileChangeKind::Renamed) {
        text += " <- " + change.oldPath;
    }
    return text;
}

// Absolute paths strictly below the root.
using MirrorTree = std::map<std::string, MirrorNode>;

struct MirrorOptions {
    char separator = '/';
    bool recursive = true;
    bool readContent = true;
    bool skipFrostTemp = true;           // Names containing ".frost-tmp-" (WatchOptions::reportFrostTempFiles = false).
    std::vector<std::string> excluded{}; // Directories whose entries are never listed (WatchOptions::excluded).
};

class WatchMirror {
public:
    WatchMirror(io::IFileSystem& fs, std::string root, MirrorOptions options = {})
        : m_fs(fs), m_root(std::move(root)), m_options(std::move(options)) {}

    // Lists the whole tree into the model (the state before the first batch).
    void load() {
        m_tree.clear();
        listChildren(m_root, m_tree, 1);
    }

    // Applies one batch in the documented order: renames (lift deepest source first, place shallowest
    // destination first), removals, then re-reads of every Created, Modified, Rescan and destination.
    void apply(const io::FileChangeBatch& batch) {
        std::vector<const io::FileChange*> renames;
        for (const io::FileChange& change : batch.changes) {
            if (change.kind == io::FileChangeKind::Renamed) {
                renames.push_back(&change);
            }
        }
        std::ranges::sort(renames, [&](const io::FileChange* a, const io::FileChange* b) {
            return depth(a->oldPath) > depth(b->oldPath);
        });
        std::vector<std::pair<const io::FileChange*, std::vector<std::pair<std::string, MirrorNode>>>> lifted;
        for (const io::FileChange* rename : renames) {
            if (m_tree.contains(rename->oldPath)) {
                lifted.emplace_back(rename, extract(rename->oldPath));
            }
        }
        std::ranges::sort(lifted,
                          [&](const auto& a, const auto& b) { return depth(a.first->path) < depth(b.first->path); });
        for (auto& [rename, nodes] : lifted) {
            removeSubtree(rename->path);
            for (auto& [suffix, node] : nodes) {
                m_tree.insert_or_assign(rename->path + suffix, std::move(node));
            }
        }
        for (const io::FileChange& change : batch.changes) {
            if (change.kind == io::FileChangeKind::Removed) {
                removeSubtree(change.path);
            }
        }
        // Re-reads, shallowest first, so a re-listed directory is not undone by a deeper re-read.
        std::vector<const io::FileChange*> rereads;
        for (const io::FileChange& change : batch.changes) {
            if (change.kind != io::FileChangeKind::Removed) {
                rereads.push_back(&change);
            }
        }
        std::ranges::stable_sort(
            rereads, [&](const io::FileChange* a, const io::FileChange* b) { return depth(a->path) < depth(b->path); });
        for (const io::FileChange* change : rereads) {
            reread(*change);
        }
    }

    [[nodiscard]] const MirrorTree& tree() const noexcept { return m_tree; }

    // The tree as the file system has it now, with the same filters.
    [[nodiscard]] MirrorTree snapshot() const {
        MirrorTree tree;
        listChildren(m_root, tree, 1);
        return tree;
    }

private:
    [[nodiscard]] std::size_t depth(std::string_view path) const {
        return static_cast<std::size_t>(std::ranges::count(path, m_options.separator));
    }

    [[nodiscard]] bool below(std::string_view path, std::string_view ancestor) const {
        if (path.size() <= ancestor.size() || !path.starts_with(ancestor)) {
            return false;
        }
        return ancestor.back() == m_options.separator || path[ancestor.size()] == m_options.separator;
    }

    [[nodiscard]] std::string join(const std::string& directory, std::string_view name) const {
        std::string path = directory;
        if (path.back() != m_options.separator) {
            path += m_options.separator;
        }
        path += name;
        return path;
    }

    [[nodiscard]] bool isExcluded(const std::string& directory) const {
        return std::ranges::find(m_options.excluded, directory) != m_options.excluded.end();
    }

    // The depth of `path` below the root (1 for the root's children).
    [[nodiscard]] std::size_t level(std::string_view path) const {
        return depth(path) - depth(m_root) + (m_root.back() == m_options.separator ? 1 : 0);
    }

    void removeSubtree(const std::string& path) {
        m_tree.erase(path);
        for (auto it = m_tree.upper_bound(path); it != m_tree.end() && it->first.starts_with(path);) {
            it = below(it->first, path) ? m_tree.erase(it) : std::next(it);
        }
    }

    // Removes `path` and its subtree, returning them keyed by their suffix after `path`.
    [[nodiscard]] std::vector<std::pair<std::string, MirrorNode>> extract(const std::string& path) {
        std::vector<std::pair<std::string, MirrorNode>> nodes;
        for (auto it = m_tree.lower_bound(path); it != m_tree.end() && it->first.starts_with(path);) {
            if (it->first == path || below(it->first, path)) {
                nodes.emplace_back(it->first.substr(path.size()), std::move(it->second));
                it = m_tree.erase(it);
            } else {
                ++it;
            }
        }
        return nodes;
    }

    // Reads `path` (one entry; with `subtree`, a directory's whole subtree) from disk into `tree`.
    void readEntry(const std::string& path, MirrorTree& tree, bool subtree) const {
        const std::string_view name = std::string_view(path).substr(path.rfind(m_options.separator) + 1);
        if (m_options.skipFrostTemp && name.find(".frost-tmp-") != std::string_view::npos) {
            return;
        }
        auto status = m_fs.status(path);
        if (!status) {
            return; // Gone (the disk moved on).
        }
        MirrorNode node{status->type, {}};
        if (status->type == io::FileType::Regular && m_options.readContent) {
            auto content = m_fs.readFile(path);
            if (!content) {
                return;
            }
            node.content = std::move(*content);
        }
        tree.insert_or_assign(path, std::move(node));
        if (status->type == io::FileType::Directory && subtree) {
            listChildren(path, tree, level(path) + 1);
        }
    }

    void listChildren(const std::string& directory, MirrorTree& tree, std::size_t childLevel) const {
        if (!m_options.recursive && childLevel > 1) {
            return;
        }
        if (isExcluded(directory)) {
            return;
        }
        auto entries = m_fs.listDirectory(directory);
        if (!entries) {
            return;
        }
        for (const io::DirEntry& entry : *entries) {
            readEntry(join(directory, entry.name), tree, /*subtree=*/true);
        }
    }

    // Modified on a directory: drop the children no longer on disk, read the new ones with their subtree.
    void relist(const std::string& directory) {
        if (isExcluded(directory) || (!m_options.recursive && directory != m_root)) {
            return;
        }
        auto entries = m_fs.listDirectory(directory);
        if (!entries) {
            return; // Gone or unreadable: its own entries say so.
        }
        std::vector<std::string> present;
        for (const io::DirEntry& entry : *entries) {
            present.push_back(join(directory, entry.name));
        }
        std::ranges::sort(present);
        std::vector<std::string> vanished;
        const std::string prefix = join(directory, "");
        for (auto it = m_tree.lower_bound(prefix); it != m_tree.end() && it->first.starts_with(prefix); ++it) {
            const bool child = it->first.find(m_options.separator, prefix.size()) == std::string::npos;
            if (child && !std::ranges::binary_search(present, it->first)) {
                vanished.push_back(it->first);
            }
        }
        for (const std::string& path : vanished) {
            removeSubtree(path);
        }
        for (const std::string& path : present) {
            if (!m_tree.contains(path)) {
                readEntry(path, m_tree, /*subtree=*/true);
            }
        }
    }

    void reread(const io::FileChange& change) {
        if (change.path == m_root) {
            if (change.kind == io::FileChangeKind::Rescan) {
                m_tree.clear();
                listChildren(m_root, m_tree, 1);
            } else if (change.kind == io::FileChangeKind::Modified) {
                relist(m_root);
            }
            return;
        }
        if (!m_options.recursive && level(change.path) > 1) {
            return;
        }
        const auto known = m_tree.find(change.path);
        const bool wasDirectory = known != m_tree.end() && known->second.type == io::FileType::Directory;
        if (change.kind == io::FileChangeKind::Modified && wasDirectory) {
            auto status = m_fs.status(change.path);
            if (status && status->type == io::FileType::Directory) {
                relist(change.path); // Its children may differ; it was not replaced.
                return;
            }
        }
        removeSubtree(change.path);
        readEntry(change.path, m_tree, /*subtree=*/true);
    }

    io::IFileSystem& m_fs;
    std::string m_root;
    MirrorOptions m_options;
    MirrorTree m_tree;
};

} // namespace frost::test
