// SPDX-License-Identifier: Apache-2.0
//
// MemFs, the in-memory IFileSystem double (see frost/io/mem_fs.hpp).

#include "frost/io/mem_fs.hpp"

#include "frost/core/assert.hpp"
#include "frost/io/utf.hpp"

#include "internal.hpp"

#include <algorithm>
#include <format>
#include <iterator>
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
    case FsOp::Rename: return "rename";
    }
    return "?";
}

Error notFound(std::string_view operation, std::string_view path) {
    return Error(ErrorCode::NotFound, std::format("{} '{}': no such file or directory", operation, path));
}

// The default root of a fresh MemFs: "/" or "C:\".
std::string defaultRoot(PathStyle style) { return style == PathStyle::Windows ? "C:\\" : "/"; }

// A drive letter, as normalizePath recognises one.
constexpr bool isAsciiAlpha(char c) noexcept { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

} // namespace

MemFs::MemFs(const IClock& clock, PathStyle style) : m_clock(clock), m_style(style) {
    LockGuard lock(m_mutex);
    m_roots.insert(defaultRoot(style));
}

Result<std::string> MemFs::normalize(std::string_view path) const {
    auto normalized = normalizePath(path, m_style);
    if (!normalized || m_style != PathStyle::Windows) {
        return normalized;
    }
    std::string& key = *normalized;
    // "\\?\C:\x" and "\\?\UNC\srv\share\x" name the same files as "C:\x" and "\\srv\share\x";
    // only the name checks differ, and normalizePath has applied those already. "\\?\1:\x" is
    // no drive form (normalizePath reads it as a generic verbatim root), so it stays as it is.
    if (key.starts_with("\\\\?\\")) {
        const std::string_view rest = std::string_view(key).substr(4);
        if (rest.size() >= 3 && isAsciiAlpha(rest[0]) && rest[1] == ':' && rest[2] == '\\') {
            key = std::string(rest);
        } else if (rest.size() >= 4 && (rest[0] == 'U' || rest[0] == 'u') && (rest[1] == 'N' || rest[1] == 'n') &&
                   (rest[2] == 'C' || rest[2] == 'c') && rest[3] == '\\') {
            key = "\\\\" + std::string(rest.substr(4));
            if (parentPath(key, m_style).empty() && !key.ends_with('\\')) {
                key += '\\'; // A share root is spelled "\\srv\share\", as normalizePath writes it.
            }
        }
    }
    // Drive letters are case-insensitive on Windows ("c:\" is "C:\").
    if (key.size() >= 2 && key[1] == ':' && key[0] >= 'a' && key[0] <= 'z') {
        key[0] = static_cast<char>(key[0] - 'a' + 'A');
    }
    return normalized;
}

/*
 * Looks `normalized` up the way the kernel does: every component before the last
 * must be an existing directory, and ".." steps to the parent of the directory
 * reached so far (never above the root). The final entry itself need not exist.
 * Missing and NotADirectory correspond to ENOENT and ENOTDIR. Windows-style paths
 * have no ".." left (normalizePath resolved it lexically, as Win32 does), so for
 * them this only checks that the parents exist.
 */
MemFs::Walk MemFs::walk(std::string_view normalized) {
    std::vector<std::string_view> chain; // The prefixes from the root down: "/", "/a", "/a/..", ...
    for (std::string_view p = normalized; !p.empty(); p = parentPath(p, m_style)) {
        chain.push_back(p);
    }
    std::ranges::reverse(chain);
    std::string current(chain.front());
    if (!m_roots.contains(current)) {
        return {Lookup::Missing, {}};
    }
    const char separator = m_style == PathStyle::Windows ? '\\' : '/';
    for (std::size_t i = 1; i < chain.size(); ++i) {
        if (!isDirectory(current)) {
            return {find(current) != nullptr ? Lookup::NotADirectory : Lookup::Missing, {}};
        }
        const std::string_view part = fileName(chain[i], m_style);
        if (part == "..") {
            if (const std::string_view parent = parentPath(current, m_style); !parent.empty()) {
                current = std::string(parent);
            }
            continue;
        }
        if (current.back() != separator) {
            current += separator;
        }
        current += part;
    }
    return {Lookup::Resolved, std::move(current)};
}

namespace {

// Whether a normalised POSIX path has a ".." component.
bool hasDotDot(std::string_view normalized) noexcept {
    return normalized.ends_with("/..") || normalized.find("/../") != std::string_view::npos;
}

} // namespace

Result<std::string> MemFs::resolve(std::string_view path, std::string_view operation, bool entryTarget) {
    auto normalized = normalize(path);
    if (!normalized) {
        return normalized;
    }
    // A final ".." names a directory, never an entry to create or remove (POSIX only:
    // Windows-style paths have no ".." left).
    if (entryTarget && fileName(*normalized, m_style) == "..") {
        return fail(ErrorCode::InvalidArgument, std::format("{} '{}': '..' is not an entry name", operation, path));
    }
    if (m_style == PathStyle::Windows || !hasDotDot(*normalized)) {
        return normalized;
    }
    // POSIX keeps "..", which the kernel resolves against the real tree: only through existing
    // directories ("/a/b/../c" needs /a/b). Without symlinks that is exactly walk().
    Walk walked = walk(*normalized);
    if (walked.status != Lookup::Resolved) {
        return std::unexpected(notFound(operation, path)); // ENOENT and ENOTDIR both map to NotFound.
    }
    return std::move(walked.key);
}

Result<void> MemFs::takeFault(FsOp op, const std::string& path, std::string_view original) {
    // A fault matches the entry an operation reaches or the operation's own spelling, so a
    // fault injected on "/a/b/../c" fires for that spelling and one on "/a/c" for both.
    const auto spelled = normalize(original);
    for (auto it = m_faults.begin(); it != m_faults.end(); ++it) {
        const bool matches = it->path.empty() || it->path == path || (spelled && it->path == *spelled);
        if (it->op != op || !matches) {
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

    // Step 1: the temp file (never observable: the lock is held throughout).
    auto temp = joinPath(parent, detail::atomicTempName(name, detail::nextUniqueId()), m_style);
    if (!temp) {
        return std::unexpected(std::move(temp.error()));
    }
    m_nodes[*temp] = Node{FileType::Regular, std::string(data), m_clock.systemNow()};

    // Step 2: the rename. A fault here leaves the target untouched and removes the temp file.
    if (auto fault = takeFault(FsOp::AtomicRename, *key, path); !fault) {
        m_nodes.erase(*temp);
        const MemFsEvent events[] = {{MemFsEventKind::Created, *temp, FileType::Regular, 0},
                                     {MemFsEventKind::Modified, *temp, FileType::Regular, 0},
                                     {MemFsEventKind::Removed, *temp, FileType::Regular, 0}};
        notify(events);
        return std::unexpected(std::move(fault.error()));
    }
    auto node = m_nodes.extract(*temp);
    node.mapped().mtime = m_clock.systemNow();
    m_nodes.insert_or_assign(*key, std::move(node.mapped()));
    touchParent(*key);
    if (!m_observers.empty()) {
        const std::uint64_t cookie = nextCookie();
        const MemFsEvent events[] = {{MemFsEventKind::Created, *temp, FileType::Regular, 0},
                                     {MemFsEventKind::Modified, *temp, FileType::Regular, 0},
                                     {MemFsEventKind::MovedFrom, *temp, FileType::Regular, cookie},
                                     {MemFsEventKind::MovedTo, *key, FileType::Regular, cookie}};
        notify(events);
    }
    return {};
}

Result<void> MemFs::writeInPlace(std::string_view path, std::string_view data) {
    LockGuard lock(m_mutex);
    auto key = resolve(path, "writeInPlace", /*entryTarget=*/true);
    if (!key) {
        return std::unexpected(std::move(key.error()));
    }
    if (auto fault = takeFault(FsOp::Write, *key, path); !fault) {
        return std::unexpected(std::move(fault.error()));
    }
    if (fileName(*key, m_style).empty()) {
        return fail(ErrorCode::InvalidArgument, std::format("writeInPlace '{}': not a file path", path));
    }
    if (!isDirectory(std::string(parentPath(*key, m_style)))) {
        return std::unexpected(notFound("writeInPlace", path));
    }
    if (isDirectory(*key)) {
        return std::unexpected(detail::isADirectoryError("writeInPlace", path));
    }
    Node* node = find(*key);
    const bool created = node == nullptr;
    if (created) {
        node = &m_nodes[*key];
        node->type = FileType::Regular;
        touchParent(*key);
    }
    node->content.assign(data);
    node->mtime = m_clock.systemNow();
    if (created) {
        const MemFsEvent events[] = {{MemFsEventKind::Created, *key, FileType::Regular, 0},
                                     {MemFsEventKind::Modified, *key, FileType::Regular, 0}};
        notify(events);
    } else {
        const MemFsEvent events[] = {{MemFsEventKind::Modified, *key, FileType::Regular, 0}};
        notify(events);
    }
    return {};
}

Result<void> MemFs::rename(std::string_view from, std::string_view to) {
    LockGuard lock(m_mutex);
    auto source = resolve(from, "rename", /*entryTarget=*/true);
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    auto target = resolve(to, "rename", /*entryTarget=*/true);
    if (!target) {
        return std::unexpected(std::move(target.error()));
    }
    if (auto fault = takeFault(FsOp::Rename, *source, from); !fault) {
        return std::unexpected(std::move(fault.error()));
    }
    if (isRootPath(*source) || isRootPath(*target)) {
        return fail(ErrorCode::InvalidArgument, std::format("rename '{}' -> '{}': cannot rename a root", from, to));
    }
    const Node* sourceNode = find(*source);
    if (sourceNode == nullptr) {
        return std::unexpected(notFound("rename", from));
    }
    if (*source == *target) {
        return {}; // rename(2) of an entry onto itself does nothing.
    }
    const char separator = m_style == PathStyle::Windows ? '\\' : '/';
    const std::string sourcePrefix = *source + separator;
    if (target->starts_with(sourcePrefix)) {
        return fail(ErrorCode::InvalidArgument,
                    std::format("rename '{}' -> '{}': the destination is inside the source", from, to));
    }
    if (!isDirectory(std::string(parentPath(*target, m_style)))) {
        return std::unexpected(notFound("rename", to));
    }
    const FileType type = sourceNode->type;
    if (const Node* targetNode = find(*target); targetNode != nullptr) {
        if (type == FileType::Directory && targetNode->type != FileType::Directory) {
            return std::unexpected(detail::notADirectoryError("rename", to));
        }
        if (type != FileType::Directory && targetNode->type == FileType::Directory) {
            return std::unexpected(detail::isADirectoryError("rename", to));
        }
        const std::string targetPrefix = *target + separator;
        if (const auto it = m_nodes.lower_bound(targetPrefix);
            it != m_nodes.end() && it->first.starts_with(targetPrefix)) {
            return fail(ErrorCode::IoError, std::format("rename '{}' -> '{}': directory not empty", from, to));
        }
        m_nodes.erase(*target);
    }
    // Re-key the entry and everything below it.
    std::vector<decltype(m_nodes)::node_type> moved;
    moved.push_back(m_nodes.extract(*source));
    for (auto it = m_nodes.lower_bound(sourcePrefix); it != m_nodes.end() && it->first.starts_with(sourcePrefix);) {
        auto next = std::next(it);
        moved.push_back(m_nodes.extract(it));
        it = next;
    }
    for (auto& node : moved) {
        node.key() = *target + node.key().substr(source->size());
        m_nodes.insert(std::move(node));
    }
    touchParent(*source);
    touchParent(*target);
    const std::uint64_t cookie = nextCookie();
    const MemFsEvent events[] = {{MemFsEventKind::MovedFrom, *source, type, cookie},
                                 {MemFsEventKind::MovedTo, *target, type, cookie}};
    notify(events);
    return {};
}

void MemFs::addObserver(IMemFsObserver& observer) {
    LockGuard lock(m_mutex);
    FROST_PRE(std::ranges::find(m_observers, &observer) == m_observers.end());
    m_observers.push_back(&observer);
}

void MemFs::removeObserver(IMemFsObserver& observer) {
    LockGuard lock(m_mutex);
    std::erase(m_observers, &observer);
}

void MemFs::notify(std::span<const MemFsEvent> events) {
    for (IMemFsObserver* observer : m_observers) {
        observer->onMemFsEvents(events);
    }
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

Result<DirListing> MemFs::listDirectoryWithDiagnostics(std::string_view path) {
    auto entries = listDirectory(path);
    if (!entries) {
        return std::unexpected(std::move(entries.error()));
    }
    DirListing listing;
    for (const DirEntry& entry : *entries) {
        if (!isValidUtf8(entry.name)) {
            listing.issues.push_back(DirListingIssue{
                entry.name, true,
                Error(ErrorCode::InvalidArgument, std::format("listDirectory '{}': a name is not valid UTF-8", path))});
        }
    }
    listing.entries = std::move(*entries);
    return listing;
}

MemFs::MkdirOutcome MemFs::makeDirectory(std::string_view spelled, std::string_view original, Error& error) {
    const Walk walked = walk(spelled);
    if (walked.status != Lookup::Resolved) {
        return MkdirOutcome::ParentMissing; // mkdir(2) fails with ENOENT or ENOTDIR.
    }
    // mkdir("a/..") fails with EEXIST: the name is the (existing) parent directory.
    if (fileName(spelled, m_style) == ".." || isDirectory(walked.key)) {
        return MkdirOutcome::Done;
    }
    if (find(walked.key) != nullptr) {
        error = Error(ErrorCode::AlreadyExists,
                      std::format("createDirectories '{}': '{}' exists and is not a directory", original, spelled));
        return MkdirOutcome::Failed;
    }
    m_nodes[walked.key] = Node{FileType::Directory, {}, m_clock.systemNow()};
    touchParent(walked.key);
    const MemFsEvent events[] = {{MemFsEventKind::Created, walked.key, FileType::Directory, 0}};
    notify(events);
    return MkdirOutcome::Done;
}

Result<void> MemFs::createDirectories(std::string_view path) {
    LockGuard lock(m_mutex);
    auto normalized = normalize(path);
    if (!normalized) {
        return std::unexpected(std::move(normalized.error()));
    }
    const Walk target = walk(*normalized);
    const std::string faultKey = target.status == Lookup::Resolved ? target.key : *normalized;
    if (auto fault = takeFault(FsOp::CreateDirectories, faultKey, path); !fault) {
        return std::unexpected(std::move(fault.error()));
    }
    /*
     * The real implementation's algorithm, step by step, so ".." behaves the same
     * (mkdir -p): walk up the spelled path until a directory can be made or
     * exists, then make the missing ones downwards. "/w/new/../out" thus creates
     * /w/new and then /w/out, and "/w/file/../out" fails with AlreadyExists.
     */
    std::vector<std::string_view> missing;
    std::string_view current = *normalized;
    for (;;) {
        Error error(ErrorCode::Unknown);
        const MkdirOutcome outcome = makeDirectory(current, path, error);
        if (outcome == MkdirOutcome::Done) {
            break;
        }
        if (outcome == MkdirOutcome::Failed) {
            return std::unexpected(std::move(error));
        }
        missing.push_back(current);
        current = parentPath(current, m_style);
        if (current.empty()) {
            return fail(ErrorCode::NotFound, std::format("createDirectories '{}': the root does not exist", path));
        }
    }
    while (!missing.empty()) {
        // Unreachable here: under the lock, the parent just made (or found) cannot disappear. The text
        // matches the real implementations, where a concurrent removal of that parent ends here.
        const std::string_view child = missing.back();
        Error error(ErrorCode::NotFound, std::format("createDirectories '{}': '{}' vanished before '{}' was created",
                                                     path, parentPath(child, m_style), child));
        if (makeDirectory(missing.back(), path, error) != MkdirOutcome::Done) {
            return std::unexpected(std::move(error));
        }
        missing.pop_back();
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
    const MemFsEvent events[] = {{MemFsEventKind::Removed, *key, FileType::Regular, 0}};
    notify(events);
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
    const MemFsEvent events[] = {{MemFsEventKind::Removed, *key, FileType::Directory, 0}};
    notify(events);
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

Result<std::string> MemFs::physicalPath(std::string_view path) {
    LockGuard lock(m_mutex);
    auto key = resolve(path, "physicalPath", /*entryTarget=*/false);
    if (!key)
        return std::unexpected(key.error());
    if (auto fault = takeFault(FsOp::Exists, *key, path); !fault)
        return std::unexpected(fault.error());
    if (!isDirectory(*key) && !find(*key))
        return std::unexpected(notFound("physicalPath", path));
    return key;
}

Result<std::string> MemFs::canonicalPath(std::string_view path) {
    // MemFs is case-sensitive, so the normalised key is the identity (in this MemFs's style).
    auto normalized = normalize(path);
    if (!normalized) {
        return normalized;
    }
    auto found = exists(*normalized);
    if (!found) {
        return std::unexpected(std::move(found.error()));
    }
    if (!*found) {
        return fail(ErrorCode::NotFound, std::string("no such file or directory: ").append(*normalized));
    }
    return normalized;
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
