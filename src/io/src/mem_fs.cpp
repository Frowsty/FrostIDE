// SPDX-License-Identifier: Apache-2.0
//
// MemFs, the in-memory IFileSystem double (see frost/io/mem_fs.hpp).

#include "frost/io/mem_fs.hpp"

#include "frost/core/assert.hpp"

#include "internal.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace frost::io {
namespace {

std::string_view opName(FsOp op) noexcept {
    switch (op) {
    case FsOp::Read: return "readFile";
    case FsOp::Write: return "writeFileAtomic";
    case FsOp::AtomicRename: return "writeFileAtomic: replace";
    case FsOp::Status: return "status";
    case FsOp::List: return "listDirectory";
    case FsOp::CreateDirectories: return "createDirectories";
    case FsOp::RemoveFile: return "removeFile";
    case FsOp::RemoveEmptyDirectory: return "removeDirectory";
    case FsOp::Exists: return "exists";
    }
    return "?";
}

Error notFound(std::string_view operation, std::string_view path) {
    return Error(ErrorCode::NotFound, std::format("{} '{}': no such file or directory", operation, path));
}

// The default root of a fresh MemFs: "/" or "C:\".
std::string defaultRoot(PathStyle style) { return style == PathStyle::Windows ? "C:\\" : "/"; }

} // namespace

MemFs::MemFs(const IClock& clock, PathStyle style) : m_clock(clock), m_style(style) {
    LockGuard lock(m_mutex);
    m_roots.insert(defaultRoot(style));
}

Result<std::string> MemFs::normalize(std::string_view path) const {
    auto normalized = normalizePath(path, m_style);
    // Drive letters are case-insensitive on Windows ("c:\" is "C:\").
    if (normalized && m_style == PathStyle::Windows && normalized->size() >= 2 && (*normalized)[1] == ':' &&
        (*normalized)[0] >= 'a' && (*normalized)[0] <= 'z') {
        (*normalized)[0] = static_cast<char>((*normalized)[0] - 'a' + 'A');
    }
    return normalized;
}

Result<std::string> MemFs::resolve(std::string_view path, std::string_view operation, bool entryTarget) {
    auto normalized = normalize(path);
    if (!normalized) {
        return normalized;
    }
    if (m_style == PathStyle::Windows) {
        return normalized; // normalizePath already resolved ".." lexically, as Win32 does.
    }
    // POSIX keeps "..": the kernel resolves it against the real tree, so it only works through
    // existing directories ("/a/b/../c" needs /a/b). Without symlinks that is exactly this walk.
    if (entryTarget && fileName(*normalized, m_style) == "..") {
        return fail(ErrorCode::InvalidArgument, std::format("{} '{}': '..' is not an entry name", operation, path));
    }
    if (normalized->find("..") == std::string::npos) {
        return normalized;
    }
    std::string current = "/";
    std::string_view rest = std::string_view(*normalized).substr(1);
    while (!rest.empty()) {
        const std::size_t end = rest.find('/');
        const std::string_view part = rest.substr(0, end);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        if (part == "..") {
            if (!isDirectory(current)) {
                return std::unexpected(notFound(operation, path)); // ENOENT, or ENOTDIR (also NotFound).
            }
            const std::string_view parent = parentPath(current, m_style);
            current = parent.empty() ? current : std::string(parent);
            continue;
        }
        if (current.back() != '/') {
            current += '/';
        }
        current += part;
    }
    return current;
}

Result<void> MemFs::takeFault(FsOp op, const std::string& path, std::string_view original) {
    for (auto it = m_faults.begin(); it != m_faults.end(); ++it) {
        if (it->op != op || (!it->path.empty() && it->path != path)) {
            continue;
        }
        const ErrorCode code = it->code;
        if (--it->remaining == 0) {
            m_faults.erase(it);
        }
        return fail(code, std::format("{} '{}': injected fault", opName(op), original));
    }
    return {};
}

MemFs::Node* MemFs::find(const std::string& path) {
    const auto it = m_nodes.find(path);
    return it == m_nodes.end() ? nullptr : &it->second;
}

bool MemFs::isRootPath(std::string_view path) const noexcept {
    return isAbsolutePath(path, m_style) && parentPath(path, m_style).empty();
}

bool MemFs::isDirectory(const std::string& path) {
    if (isRootPath(path)) {
        return m_roots.contains(path);
    }
    const Node* node = find(path);
    return node != nullptr && node->type == FileType::Directory;
}

void MemFs::touchParent(std::string_view path) {
    const std::string parent(parentPath(path, m_style));
    if (Node* node = find(parent); node != nullptr) {
        node->mtime = m_clock.systemNow();
    }
}

void MemFs::addRoot(std::string_view root) {
    auto key = normalize(root);
    FROST_PRE(key.has_value() && isRootPath(*key));
    LockGuard lock(m_mutex);
    m_roots.insert(std::move(*key));
}

Result<std::string> MemFs::readFile(std::string_view path) {
    LockGuard lock(m_mutex);
    auto key = resolve(path, "readFile", /*entryTarget=*/false);
    if (!key) {
        return std::unexpected(std::move(key.error()));
    }
    if (auto fault = takeFault(FsOp::Read, *key, path); !fault) {
        return std::unexpected(std::move(fault.error()));
    }
    if (isDirectory(*key)) {
        return std::unexpected(detail::isADirectoryError("readFile", path));
    }
    const Node* node = find(*key);
    if (node == nullptr) {
        return std::unexpected(notFound("readFile", path));
    }
    return node->content;
}

Result<void> MemFs::writeFileAtomic(std::string_view path, std::string_view data) {
    LockGuard lock(m_mutex);
    auto key = resolve(path, "writeFileAtomic", /*entryTarget=*/true);
    if (!key) {
        return std::unexpected(std::move(key.error()));
    }
    if (auto fault = takeFault(FsOp::Write, *key, path); !fault) {
        return std::unexpected(std::move(fault.error()));
    }
    const std::string_view name = fileName(*key, m_style);
    if (name.empty()) {
        return fail(ErrorCode::InvalidArgument, std::format("writeFileAtomic '{}': not a file path", path));
    }
    const std::string parent(parentPath(*key, m_style));
    if (!isDirectory(parent)) {
        return std::unexpected(notFound("writeFileAtomic", path));
    }
    if (isDirectory(*key)) {
        return std::unexpected(detail::isADirectoryError("writeFileAtomic", path));
    }

    // Step 1: the temp file, visible to listDirectory like the real one.
    auto temp = joinPath(parent, detail::atomicTempName(name, detail::nextUniqueId()), m_style);
    if (!temp) {
        return std::unexpected(std::move(temp.error()));
    }
    m_nodes[*temp] = Node{FileType::Regular, std::string(data), m_clock.systemNow()};

    // Step 2: the rename. A fault here leaves the target untouched and removes the temp file.
    if (auto fault = takeFault(FsOp::AtomicRename, *key, path); !fault) {
        m_nodes.erase(*temp);
        return std::unexpected(std::move(fault.error()));
    }
    auto node = m_nodes.extract(*temp);
    node.mapped().mtime = m_clock.systemNow();
    m_nodes.insert_or_assign(*key, std::move(node.mapped()));
    touchParent(*key);
    return {};
}

Result<FileStatus> MemFs::status(std::string_view path) {
    LockGuard lock(m_mutex);
    auto key = resolve(path, "status", /*entryTarget=*/false);
    if (!key) {
        return std::unexpected(std::move(key.error()));
    }
    if (auto fault = takeFault(FsOp::Status, *key, path); !fault) {
        return std::unexpected(std::move(fault.error()));
    }
    if (isRootPath(*key)) {
        if (!m_roots.contains(*key)) {
            return std::unexpected(notFound("status", path));
        }
        return FileStatus{FileType::Directory, 0, {}};
    }
    const Node* node = find(*key);
    if (node == nullptr) {
        return std::unexpected(notFound("status", path));
    }
    const std::uint64_t size = node->type == FileType::Directory ? 0 : node->content.size();
    return FileStatus{node->type, size, node->mtime};
}

Result<std::vector<DirEntry>> MemFs::listDirectory(std::string_view path) {
    LockGuard lock(m_mutex);
    auto key = resolve(path, "listDirectory", /*entryTarget=*/false);
    if (!key) {
        return std::unexpected(std::move(key.error()));
    }
    if (auto fault = takeFault(FsOp::List, *key, path); !fault) {
        return std::unexpected(std::move(fault.error()));
    }
    if (!isDirectory(*key)) {
        if (find(*key) != nullptr) {
            return std::unexpected(detail::notADirectoryError("listDirectory", path));
        }
        return std::unexpected(notFound("listDirectory", path));
    }
    const char separator = m_style == PathStyle::Windows ? '\\' : '/';
    std::string prefix = *key;
    if (prefix.back() != separator) {
        prefix += separator;
    }
    std::vector<DirEntry> entries;
    for (auto it = m_nodes.lower_bound(prefix); it != m_nodes.end() && it->first.starts_with(prefix); ++it) {
        const std::string_view rest = std::string_view(it->first).substr(prefix.size());
        if (rest.find(separator) == std::string_view::npos) {
            entries.push_back(DirEntry{std::string(rest), it->second.type});
        }
    }
    std::ranges::sort(entries, {}, &DirEntry::name);
    return entries;
}

Result<void> MemFs::createDirectories(std::string_view path) {
    LockGuard lock(m_mutex);
    auto key = resolve(path, "createDirectories", /*entryTarget=*/false);
    if (!key) {
        return std::unexpected(std::move(key.error()));
    }
    if (auto fault = takeFault(FsOp::CreateDirectories, *key, path); !fault) {
        return std::unexpected(std::move(fault.error()));
    }
    // Ancestors from the top down, the target last.
    std::vector<std::string_view> chain;
    for (std::string_view p = *key; !p.empty(); p = parentPath(p, m_style)) {
        chain.push_back(p);
    }
    std::ranges::reverse(chain);
    for (const std::string_view p : chain) {
        const std::string component(p);
        if (isDirectory(component)) {
            continue;
        }
        if (isRootPath(component)) {
            return fail(ErrorCode::NotFound, std::format("createDirectories '{}': the root does not exist", path));
        }
        if (find(component) != nullptr) {
            return fail(ErrorCode::AlreadyExists,
                        std::format("createDirectories '{}': '{}' exists and is not a directory", path, component));
        }
        m_nodes[component] = Node{FileType::Directory, {}, m_clock.systemNow()};
        touchParent(component);
    }
    return {};
}

Result<void> MemFs::removeFile(std::string_view path) {
    LockGuard lock(m_mutex);
    auto key = resolve(path, "removeFile", /*entryTarget=*/true);
    if (!key) {
        return std::unexpected(std::move(key.error()));
    }
    if (auto fault = takeFault(FsOp::RemoveFile, *key, path); !fault) {
        return std::unexpected(std::move(fault.error()));
    }
    if (isDirectory(*key)) {
        return std::unexpected(detail::isADirectoryError("removeFile", path));
    }
    if (isRootPath(*key) || m_nodes.erase(*key) == 0) {
        return std::unexpected(notFound("removeFile", path));
    }
    touchParent(*key);
    return {};
}

Result<void> MemFs::removeDirectory(std::string_view path) {
    LockGuard lock(m_mutex);
    auto key = resolve(path, "removeDirectory", /*entryTarget=*/true);
    if (!key) {
        return std::unexpected(std::move(key.error()));
    }
    if (auto fault = takeFault(FsOp::RemoveEmptyDirectory, *key, path); !fault) {
        return std::unexpected(std::move(fault.error()));
    }
    if (isRootPath(*key)) {
        if (!m_roots.contains(*key)) {
            return std::unexpected(notFound("removeDirectory", path));
        }
        return fail(ErrorCode::PermissionDenied, std::format("removeDirectory '{}': cannot remove a root", path));
    }
    const Node* node = find(*key);
    if (node == nullptr) {
        return std::unexpected(notFound("removeDirectory", path));
    }
    if (node->type != FileType::Directory) {
        return std::unexpected(detail::notADirectoryError("removeDirectory", path));
    }
    const char separator = m_style == PathStyle::Windows ? '\\' : '/';
    const std::string prefix = *key + separator;
    if (const auto it = m_nodes.lower_bound(prefix); it != m_nodes.end() && it->first.starts_with(prefix)) {
        return fail(ErrorCode::IoError, std::format("removeDirectory '{}': directory not empty", path));
    }
    m_nodes.erase(*key);
    touchParent(*key);
    return {};
}

Result<bool> MemFs::exists(std::string_view path) {
    LockGuard lock(m_mutex);
    auto key = resolve(path, "exists", /*entryTarget=*/false);
    if (!key) {
        if (key.error().code() == ErrorCode::NotFound) {
            return false; // A ".." through a missing directory, like status().
        }
        return std::unexpected(std::move(key.error()));
    }
    if (auto fault = takeFault(FsOp::Exists, *key, path); !fault) {
        return std::unexpected(std::move(fault.error()));
    }
    return isDirectory(*key) || find(*key) != nullptr;
}

void MemFs::injectFault(FsOp op, std::string_view path, ErrorCode code, std::uint32_t count) {
    FROST_PRE(path.empty() || isAbsolutePath(path, m_style));
    FROST_PRE(count > 0);
    std::string key;
    if (!path.empty()) {
        auto normalized = normalize(path);
        key = normalized ? std::move(*normalized) : std::string(path);
    }
    LockGuard lock(m_mutex);
    m_faults.push_back(Fault{op, std::move(key), code, count});
}

std::size_t MemFs::pendingFaults() const {
    LockGuard lock(m_mutex);
    std::size_t total = 0;
    for (const Fault& fault : m_faults) {
        total += fault.remaining;
    }
    return total;
}

std::vector<std::string> MemFs::allPaths() const {
    LockGuard lock(m_mutex);
    std::vector<std::string> paths;
    paths.reserve(m_nodes.size());
    for (const auto& [path, _] : m_nodes) {
        paths.push_back(path);
    }
    return paths;
}

} // namespace frost::io
