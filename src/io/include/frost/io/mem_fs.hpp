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
 *     ".<name>.frost-tmp-<pid>-<n>" entry (<name> cut to at most 64 bytes at a
 *     UTF-8 character boundary), fills it, then renames it over the target,
 *     all under one lock (no caller can observe the temp entry), so faults can
 *     be injected at either step (FsOp::Write fails before the temp file
 *     exists, FsOp::AtomicRename after it has been written). Either way the
 *     target keeps its old content and the temp entry is removed.
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
 * Observers (MemFsWatcher, ADR 0020). Every successful mutation reports what
 * inotify would report for it, in order, to every registered IMemFsObserver:
 *   writeFileAtomic   Created(temp), Modified(temp), MovedFrom(temp), MovedTo(target)
 *                     (with an AtomicRename fault: Created, Modified, Removed of the temp)
 *   writeInPlace      Created then Modified for a new file, Modified for an existing one
 *   createDirectories Created for each directory made, outermost first
 *   removeFile        Removed (Regular);  removeDirectory  Removed (Directory)
 *   rename            MovedFrom(from), MovedTo(to), sharing a cookie; a replaced
 *                     destination is not reported separately (as with inotify)
 * Parent directories' mtime changes are not reported (inotify reports none).
 * Cookies are unique per MemFs. rename and writeInPlace exist only on the
 * double: they let tests produce the event shapes of other programs.
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
#include <span>
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
    Rename, // rename (MemFs only); writeInPlace uses Write.
};

enum class MemFsEventKind : unsigned char { Created, Modified, Removed, MovedFrom, MovedTo };

// One MemFs mutation step, as inotify would report it (see above).
struct MemFsEvent {
    MemFsEventKind kind = MemFsEventKind::Modified;
    std::string path{}; // Normalised tree key.
    FileType type = FileType::Regular;
    std::uint64_t cookie = 0; // MovedFrom/MovedTo only: the same non-zero value for both halves.

    friend bool operator==(const MemFsEvent&, const MemFsEvent&) = default;
};

class IMemFsObserver {
public:
    /*
     * Called once per successful mutation with its events in order, on the mutating thread and under
     * the MemFs lock, so observers see mutations in their exact order. Must not call back into the MemFs
     * (that would deadlock) and must not block.
     */
    virtual void onMemFsEvents(std::span<const MemFsEvent> events) noexcept = 0;

protected:
    IMemFsObserver() = default;
    IMemFsObserver(const IMemFsObserver&) = default;
    IMemFsObserver& operator=(const IMemFsObserver&) = default;
    ~IMemFsObserver() = default;
};

class MemFs final : public IFileSystem {
public:
    explicit MemFs(const IClock& clock, PathStyle style = kNativePathStyle);

    Result<std::string> readFile(std::string_view path) override;
    Result<void> writeFileAtomic(std::string_view path, std::string_view data) override;
    Result<FileStatus> status(std::string_view path) override;
    Result<std::vector<DirEntry>> listDirectory(std::string_view path) override;
    // As listDirectory (FsOp::List faults included); names that are not valid UTF-8 are listed and
    // reported, as on POSIX.
    Result<DirListing> listDirectoryWithDiagnostics(std::string_view path) override;
    Result<void> createDirectories(std::string_view path) override;
    Result<void> removeFile(std::string_view path) override;
    Result<void> removeDirectory(std::string_view path) override;
    Result<bool> exists(std::string_view path) override;
    // normalizePath in this MemFs's style; NotFound for a missing path.
    Result<std::string> canonicalPath(std::string_view path) override;

    // Makes the next `count` `op` operations on `path` fail with `code` (see above). `path` must be
    // absolute (or empty for "any path") and `count` at least 1; anything else is a contract violation.
    void injectFault(FsOp op, std::string_view path, ErrorCode code, std::uint32_t count = 1);

    // Makes `root` ("D:\", "\\server\share\", ...) exist, empty. Precondition: `root`
    // is a root path under this MemFs's style. Adding an existing root does nothing.
    void addRoot(std::string_view root);

    // Number of injected failures still to fire (the sum of the remaining counts).
    [[nodiscard]] std::size_t pendingFaults() const;

    [[nodiscard]] PathStyle style() const noexcept { return m_style; }

    /*
     * Moves the file or directory `from` to `to` as rename(2) does: an existing destination file is
     * replaced, an existing destination directory must be empty and `from` a directory too. Renaming an
     * entry onto itself does nothing. Errors: NotFound (`from`, or the parent of `to`, missing),
     * InvalidArgument (`to` below `from`; a directory onto a file or the reverse; a root), IoError (the
     * destination directory is not empty), injected faults (FsOp::Rename, matched on `from`).
     * O(k log n) for k entries below `from`.
     */
    [[nodiscard]] Result<void> rename(std::string_view from, std::string_view to);

    // Truncates and rewrites `path` in place (open with O_TRUNC, write), creating it if missing, so the
    // file's identity is kept. Errors as writeFileAtomic's (faults: FsOp::Write).
    [[nodiscard]] Result<void> writeInPlace(std::string_view path, std::string_view data);

    // Registers `observer` (see above); from the call on, every mutation is reported to it. The observer
    // must stay valid until removeObserver returns. Registering twice is a contract violation.
    void addObserver(IMemFsObserver& observer);
    // Unregisters `observer`; no callback runs after this returns. Not from inside a callback.
    void removeObserver(IMemFsObserver& observer);

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
    // Reports one mutation's events to every observer.
    void notify(std::span<const MemFsEvent> events) FROST_REQUIRES(m_mutex);
    // A fresh rename cookie (never 0).
    [[nodiscard]] std::uint64_t nextCookie() FROST_REQUIRES(m_mutex) { return ++m_cookies; }

    const IClock& m_clock;
    const PathStyle m_style;
    mutable Mutex m_mutex;
    // Keyed by normalised path; std::map keeps listings sorted and deterministic.
    std::map<std::string, Node, std::less<>> m_nodes FROST_GUARDED_BY(m_mutex);
    std::set<std::string, std::less<>> m_roots FROST_GUARDED_BY(m_mutex);
    std::deque<Fault> m_faults FROST_GUARDED_BY(m_mutex);
    std::vector<IMemFsObserver*> m_observers FROST_GUARDED_BY(m_mutex);
    std::uint64_t m_cookies FROST_GUARDED_BY(m_mutex) = 0;
};

} // namespace frost::io
