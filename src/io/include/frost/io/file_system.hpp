// SPDX-License-Identifier: Apache-2.0
//
// IFileSystem: the file-system seam (PLAN.md sections 3.3 and 6.2). Production
// code reaches files only through an IFileSystem, so tests can substitute
// MemFs (frost/io/mem_fs.hpp) with a virtual clock and injected faults.
//
/*
 * Conventions shared by every implementation:
 *
 * Paths are absolute UTF-8 strings (frost/io/path.hpp); relative paths fail
 * with InvalidArgument and are never resolved against the current directory.
 * The real implementation hands Win32 the extended-length (\\?\) form, so
 * paths longer than MAX_PATH work even when LongPathsEnabled is 0.
 *
 * File contents are raw bytes held in std::string; nothing is decoded,
 * re-encoded or line-ending converted.
 *
 * Errors are frost::Error values with codes mapped as in frost/io/os_error.hpp
 * and messages naming the operation, the path and the OS error. Operation-
 * specific refinements: reading, removing or atomically writing a directory
 * where a file is expected fails with InvalidArgument ("is a directory");
 * listing a non-directory fails with InvalidArgument ("not a directory").
 *
 * Thread safety: implementations are safe to call from any thread
 * concurrently; operations on the same path race as they would on the OS.
 *
 * Interfaces are passed by reference and never deleted through a base
 * pointer, hence the protected non-virtual destructor (see IClock).
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {

enum class FileType : unsigned char {
    Regular,
    Directory,
    Symlink, // Only reported by listDirectory (status follows links). Windows junctions count as links.
    Other,   // Devices, pipes, sockets.
};

// What status() reports. `size` is 0 for directories. `mtime` is the last
// modification time at the file system's resolution (100 ns on NTFS).
struct FileStatus {
    FileType type = FileType::Regular;
    std::uint64_t size = 0;
    IClock::SystemTime mtime{};

    friend bool operator==(const FileStatus&, const FileStatus&) = default;
};

// One listDirectory() entry: a name (one component, never "." or "..") and the entry's own type, without
// following symlinks. The name is UTF-8 on Windows (names that are not valid UTF-16 are skipped); on POSIX it
// is the raw bytes the kernel returns, which need not be valid UTF-8.
struct DirEntry {
    std::string name;
    FileType type = FileType::Regular;

    friend bool operator==(const DirEntry&, const DirEntry&) = default;
};

class IFileSystem {
public:
    /*
     * Returns the whole file. Errors: NotFound, PermissionDenied, Busy (Windows sharing
     * violation; on POSIX a write lease that is still held after 90 s), InvalidArgument (a
     * directory, or not a regular file: FIFOs, sockets and devices are refused without
     * blocking), IoError. A POSIX file under another process's write lease is read once the
     * lease is released or revoked. O(size).
     */
    [[nodiscard]] virtual Result<std::string> readFile(std::string_view path) = 0;

    /*
     * Replaces (or creates) the file at `path` with `data`, atomically: other
     * readers and a crash observe either the complete old content or the
     * complete new content, never a mixture or a truncated file.
     *
     * Protocol: write a temporary file in the same directory
     * (".<name>.frost-tmp-<pid>-<n>", with <name> cut to at most 64 bytes at a
     * UTF-8 character boundary, created exclusively), flush it to stable storage,
     * then rename it over the target in one step (MoveFileExW with
     * MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH on Windows; rename() then
     * an fsync of the directory on POSIX). On any failure the temporary file is
     * removed and the target is left as it was. A process killed mid-write can
     * leave a temporary file behind but never a damaged target.
     *
     * POSIX metadata. A new file is created like any open(O_CREAT, 0666), so the
     * umask or the directory's default ACL decides its mode. When a file is
     * replaced, the temporary file is created 0600 and, only once its content is
     * complete, receives the target's owner, group, permission bits (set-id bits
     * included) and POSIX access ACL, as far as this process may set them: the
     * owner needs CAP_CHOWN, the group needs membership, and the ACL is copied
     * only when both are kept (one the temporary file inherited from the
     * directory's default ACL is removed). Whatever cannot be kept fails safe, so
     * no one gets more access than the target gave them: a different group drops
     * S_ISGID and leaves the group and other classes only what both had (0664
     * becomes 0644, 0640 becomes 0600); a different owner drops S_ISUID and limits
     * both classes to the owner's bits; an ACL that is not copied leaves both
     * classes only what every non-owner entry granted (named entries may deny).
     *
     * Read-only targets: Windows refuses to replace them (PermissionDenied). POSIX
     * replaces them, because rename needs write access to the directory only, and
     * the new file keeps the read-only mode. TODO(M1): refuse a non-writable target
     * with PermissionDenied on POSIX too, matching Windows, with a test; M1's
     * SaveManager then offers an explicit "overwrite read-only file" action (ADR 0011, E3).
     *
     * Not preserved from an existing target: extended attributes other than the
     * POSIX access ACL; Windows attributes, ACLs and alternate streams; hard-link
     * identity and symlinks: on POSIX a symlink at `path` is replaced by a regular
     * file, which takes the owner, mode and access ACL of the file the link pointed
     * to; on Windows a file symlink at `path` is replaced too, even one that points
     * at a directory, while a directory symlink or junction, even a dangling one,
     * makes the call fail with InvalidArgument ("is a directory") and is left in place.
     * SaveManager (M1) adds metadata and symlink preservation on top of this
     * primitive. TODO(M1): sweep stale .frost-tmp-* files left by killed processes.
     *
     * Errors: NotFound (parent directory missing), PermissionDenied, Busy,
     * InvalidArgument (the target is a directory, or on Windows a directory
     * symlink or junction, even a dangling one), IoError (disk full, ...).
     */
    [[nodiscard]] virtual Result<void> writeFileAtomic(std::string_view path, std::string_view data) = 0;

    // Type, size and modification time, following symlinks. Errors: NotFound
    // (including dangling links), PermissionDenied, IoError.
    [[nodiscard]] virtual Result<FileStatus> status(std::string_view path) = 0;

    /*
     * The entries of a directory, sorted by name (byte-wise, i.e. by code point).
     * Errors: NotFound, InvalidArgument (not a directory), PermissionDenied, IoError.
     * Windows names that are not valid UTF-16 (lone surrogates) cannot be represented
     * and are skipped; POSIX names are returned as the kernel's bytes, valid UTF-8 or
     * not (see DirEntry). TODO(M1): surface both to the scanner as diagnostics.
     */
    [[nodiscard]] virtual Result<std::vector<DirEntry>> listDirectory(std::string_view path) = 0;

    // Creates `path` and any missing parents; succeeds if it already is a directory (links are followed).
    // Errors: NotFound (the root, drive or share does not exist), AlreadyExists (it or a parent exists as a
    // non-directory, including a dangling link), PermissionDenied, IoError.
    [[nodiscard]] virtual Result<void> createDirectories(std::string_view path) = 0;

    /*
     * Removes a file, or a link itself: every entry listDirectory reports as Symlink
     * (including Windows directory symlinks and junctions) is removed with removeFile on
     * every platform, and the link is never followed. Errors: NotFound, InvalidArgument
     * (a directory), PermissionDenied (includes read-only files on Windows), Busy.
     */
    [[nodiscard]] virtual Result<void> removeFile(std::string_view path) = 0;

    // Removes an empty directory. A link to a directory is not a directory here
    // (InvalidArgument; use removeFile). Errors: NotFound, InvalidArgument (not a
    // directory), IoError (not empty), PermissionDenied, Busy.
    [[nodiscard]] virtual Result<void> removeDirectory(std::string_view path) = 0;

    // Whether status(path) would succeed. NotFound yields false; other failures
    // (e.g. PermissionDenied on a parent) are returned as errors.
    [[nodiscard]] virtual Result<bool> exists(std::string_view path) = 0;

protected:
    constexpr IFileSystem() noexcept = default;
    constexpr IFileSystem(const IFileSystem&) noexcept = default;
    constexpr IFileSystem& operator=(const IFileSystem&) noexcept = default;
    ~IFileSystem() = default;
};

// The process-wide real file system (wide Win32 APIs on Windows, POSIX on Linux).
// Stateless and thread-safe.
[[nodiscard]] IFileSystem& realFileSystem() noexcept;

} // namespace frost::io
