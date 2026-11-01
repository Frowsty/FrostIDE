// SPDX-License-Identifier: Apache-2.0
//
// MemFs: the deterministic IFileSystem double (PLAN.md section 6.2). An
// in-memory tree whose modification times come from an IClock (normally a
// ManualClock) and that can be told to fail specific operations.
//
/*
 * Semantics follow the real implementations closely enough that code tested
 * against MemFs behaves the same on disk:
 *   - Paths must be absolute under the MemFs's PathStyle (native by default)
 *     and are normalised with normalizePath, so "C:/a" and "C:\a" are the same
 *     file and every name normalizePath refuses is refused here too. The
 *     verbatim forms "\\?\C:\x" and "\\?\UNC\srv\share\x" name the same files as
 *     "C:\x" and "\\srv\share\x" (with the escape hatch's looser name rules);
 *     other verbatim roots are separate trees. POSIX ".." is resolved the way the
 *     kernel does, through existing directories only ("/a/b/../c" needs /a/b;
 *     otherwise NotFound), except that createDirectories follows the real
 *     implementation's mkdir -p walk ("/a/new/../c" creates /a/new and /a/c). A
 *     final ".." is not an entry name for writeFileAtomic, removeFile and
 *     removeDirectory (InvalidArgument), as with the real implementation.
 *     Another exception: in POSIX style a trailing separator or final "." after
 *     a non-directory is dropped here ("/d/file/" reads /d/file), while the real
 *     readFile, status, listDirectory, removeFile, removeDirectory and exists
 *     hand that spelling to the kernel and fail with NotFound (ENOTDIR; exists
 *     answers false). writeFileAtomic and createDirectories normalise in both.
 *   - Only "/" (POSIX) or "C:\" (Windows) exists initially; other roots (drives,
 *     "\\server\share\") exist once addRoot() is called, so code that assumes a
 *     drive is present fails here as it would on a machine without it. Drive
 *     letters are case-insensitive; other names are case-sensitive in both
 *     styles. TODO(M2): optional case-insensitive lookups for Windows-style
 *     trees, matching NTFS.
 *   - Errors use the same codes and refinements as file_system.hpp.
 *   - writeFileAtomic runs the same protocol as the real one: it creates a
 *     ".<name>.frost-tmp-<pid>-<n>" entry (<name> cut to its first 64 bytes),
 *     fills it, then renames it over the target, all under one lock (no caller
 *     can observe the temp entry), so faults can be injected at either step
 *     (FsOp::Write fails before the temp file exists, FsOp::AtomicRename after
 *     it has been written). Either way the target keeps its old content and
 *     the temp entry is removed.
 *   - Every successful mutation stamps the touched entry with clock.systemNow(),
 *     and the parent directory too for every writeFileAtomic (the temp-file
 *     rename changes the directory, as on disk), creation and removal.
 *   - Symlinks are not modelled.
 *
 * Fault injection. injectFault(op, path, code, count) makes the next `count`
 * operations of kind `op` on `path` fail with `code` before they change
 * anything. An operation matches when its own normalised spelling equals the
 * normalised `path`, or when the entry it reaches does (so a fault on "/a/c"
 * also fires for "/a/b/../c"). Faults are consumed in the order they were
 * injected; an empty path matches every path.
 *
 * Thread safety: every member function may be called concurrently; one mutex
 * guards the whole tree (contention is irrelevant for a test double). The
 * clock must outlive the MemFs.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {

// The operations faults can target. AtomicRename is the second step of
// writeFileAtomic (temp file written, about to replace the target).
enum class FsOp : unsigned char {
    Read,
    Write,
    AtomicRename,
    Status,
    List,
    CreateDirectories,
    RemoveFile,
    RemoveEmptyDirectory, // removeDirectory (named so it cannot collide with the windows.h RemoveDirectory macro)
    Exists,
};

class MemFs final : public IFileSystem {
public:
    explicit MemFs(const IClock& clock, PathStyle style = kNativePathStyle);

    Result<std::string> readFile(std::string_view path) override;
    Result<void> writeFileAtomic(std::string_view path, std::string_view data) override;
    Result<FileStatus> status(std::string_view path) override;
    Result<std::vector<DirEntry>> listDirectory(std::string_view path) override;
    Result<void> createDirectories(std::string_view path) override;
    Result<void> removeFile(std::string_view path) override;
    Result<void> removeDirectory(std::string_view path) override;
    Result<bool> exists(std::string_view path) override;

    // Makes the next `count` `op` operations on `path` fail with `code` (see above). `path` must be
    // absolute (or empty for "any path") and `count` at least 1; anything else is a contract violation.
    void injectFault(FsOp op, std::string_view path, ErrorCode code, std::uint32_t count = 1);

    // Makes `root` ("D:\", "\\server\share\", ...) exist, empty. Precondition: `root`
    // is a root path under this MemFs's style. Adding an existing root does nothing.
    void addRoot(std::string_view root);

    // Number of injected failures still to fire (the sum of the remaining counts).
    [[nodiscard]] std::size_t pendingFaults() const;

    // Every normalised path in the tree (files and directories, roots excluded),
    // sorted. For assertions such as "no temp files are left".
    [[nodiscard]] std::vector<std::string> allPaths() const;

private:
    struct Node {
        FileType type = FileType::Regular;
        std::string content;
        IClock::SystemTime mtime{};
    };

    struct Fault {
        FsOp op;
        std::string path; // Normalised; empty matches any path.
        ErrorCode code;
        std::uint32_t remaining;
    };

    // Normalises `path` under m_style (\\?\ drive and UNC forms mapped to their plain
    // spelling, drive letters upper-cased); InvalidArgument for relative paths. Pure.
    [[nodiscard]] Result<std::string> normalize(std::string_view path) const;
    // How a lookup ends: every parent found, or the kernel's ENOENT / ENOTDIR.
    enum class Lookup : unsigned char { Resolved, Missing, NotADirectory };
    struct Walk {
        Lookup status = Lookup::Missing;
        std::string key{}; // With Resolved: the tree key of the final entry (which may not exist).
    };
    // What one mkdir(2) of the real createDirectories would do.
    enum class MkdirOutcome : unsigned char { Done, ParentMissing, Failed };

    // Looks a normalised path up through the tree like the kernel (see mem_fs.cpp).
    [[nodiscard]] Walk walk(std::string_view normalized) FROST_REQUIRES(m_mutex);
    // The tree key for `path`: normalize() plus POSIX ".." resolution against the tree.
    // `entryTarget`: the operation names an entry to create or remove, so a final ".." is refused.
    [[nodiscard]] Result<std::string> resolve(std::string_view path, std::string_view operation, bool entryTarget)
        FROST_REQUIRES(m_mutex);
    // One mkdir(2) of `spelled` (a normalised path); sets `error` when it returns Failed.
    [[nodiscard]] MkdirOutcome makeDirectory(std::string_view spelled, std::string_view original, Error& error)
        FROST_REQUIRES(m_mutex);
    // Consumes a matching fault and returns its error, if any.
    [[nodiscard]] Result<void> takeFault(FsOp op, const std::string& path, std::string_view original)
        FROST_REQUIRES(m_mutex);
    // The node at `path`, or nullptr. Roots are tracked in m_roots, not as nodes.
    [[nodiscard]] Node* find(const std::string& path) FROST_REQUIRES(m_mutex);
    // Whether `path` has the syntax of a root ("/", "C:\", "\\server\share\"), existing or not.
    [[nodiscard]] bool isRootPath(std::string_view path) const noexcept;
    // Whether `path` names an existing directory (roots included).
    [[nodiscard]] bool isDirectory(const std::string& path) FROST_REQUIRES(m_mutex);
    // Stamps the parent directory of `path` (if it is not a root) with the current time.
    void touchParent(std::string_view path) FROST_REQUIRES(m_mutex);

    const IClock& m_clock;
    const PathStyle m_style;
    mutable Mutex m_mutex;
    // Keyed by normalised path; std::map keeps listings sorted and deterministic.
    std::map<std::string, Node, std::less<>> m_nodes FROST_GUARDED_BY(m_mutex);
    std::set<std::string, std::less<>> m_roots FROST_GUARDED_BY(m_mutex);
    std::deque<Fault> m_faults FROST_GUARDED_BY(m_mutex);
};

} // namespace frost::io
