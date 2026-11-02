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
#include "frost/io/path.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
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

// A name listDirectoryWithDiagnostics() reports: one frost cannot use as a UTF-8 path component.
struct DirListingIssue {
    std::string name;    // Windows: UTF-8 with each unpaired surrogate as U+FFFD; POSIX: the raw bytes.
    bool listed = false; // Whether DirListing::entries has it (POSIX: yes, as raw bytes; Windows: no).
    Error error{ErrorCode::InvalidArgument}; // InvalidArgument, naming the directory and the problem.
};

// listDirectoryWithDiagnostics(): the entries listDirectory() returns, and what it passed over silently.
struct DirListing {
    std::vector<DirEntry> entries;
    std::vector<DirListingIssue> issues;
};

// How IFileSystem::safeWrite put the bytes in place.
enum class SafeWriteMethod : unsigned char {
    Created,  // No file existed; a temp file was renamed into place.
    Replaced, // A temp file replaced the file (ReplaceFileW on Windows, rename on POSIX).
    InPlace,  // The file has several hard links and was rewritten in place.
};

// Options of IFileSystem::safeWrite.
struct SafeWriteOptions {
    // Replace a read-only target after making it writable (the explicit "overwrite read-only file"
    // action); otherwise such a target fails with PermissionDenied.
    bool overwriteReadOnly = false;
};

// What IFileSystem::safeWrite did.
struct SafeWriteResult {
    std::string target{}; // The file written: `path` normalised, or the file a symlink at `path` resolves to.
    SafeWriteMethod method = SafeWriteMethod::Replaced;
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
     * the new file keeps the read-only mode. That stays so for this primitive (the
     * settings and storage writers rely on it); safeWrite, the editor's save
     * primitive, refuses a non-writable target with PermissionDenied on both
     * platforms unless SafeWriteOptions::overwriteReadOnly asks for the explicit
     * "overwrite read-only file" action (ADR 0011, E3; ADR 0028).
     *
     * Not preserved from an existing target: extended attributes other than the
     * POSIX access ACL; Windows attributes, ACLs and alternate streams; hard-link
     * identity and symlinks: on POSIX a symlink at `path` is replaced by a regular
     * file, which takes the owner, mode and access ACL of the file the link pointed
     * to; on Windows a file symlink at `path` is replaced too, even one that points
     * at a directory, while a directory symlink or junction, even a dangling one,
     * makes the call fail with InvalidArgument ("is a directory") and is left in place.
     * safeWrite adds attribute, ACL, hard-link and symlink preservation on top of
     * this protocol. Temp files left by killed processes are removed by
     * removeStaleTempFiles (below), which the SaveManager runs for every directory it
     * opens or saves files in (ADR 0028; scanTree and the watchers hide them meanwhile).
     *
     * Errors: NotFound (parent directory missing), PermissionDenied, Busy,
     * InvalidArgument (the target is a directory, or on Windows a directory
     * symlink or junction, even a dangling one), IoError (disk full, ...).
     */
    [[nodiscard]] virtual Result<void> writeFileAtomic(std::string_view path, std::string_view data) = 0;

    /*
     * The editor's save primitive (SaveManager, ADR 0028): writes `data` as the new content of the
     * file at `path` so that a crash, a full disk or a refused replace never leaves a partial or
     * mixed file, and keeps what identifies the file:
     *   - a symlink at `path` stays a link: the file it resolves to is written (result.target);
     *   - a file with one link gets a temp file beside it (writeFileAtomic's name and flush), swapped
     *     in with ReplaceFileW on Windows, which keeps its attributes, ACL, alternate streams and
     *     creation time, or with rename on POSIX after the owner, mode and access ACL were copied as
     *     writeFileAtomic does;
     *   - a file with several hard links is rewritten in place, so every link keeps seeing it: the
     *     space is reserved first, so a full disk fails before any byte changes (a crash during the
     *     rewrite itself can still leave a mixture; the SaveManager's journal keeps the buffer);
     *   - a missing file is created through writeFileAtomic (a dangling symlink is NotFound).
     * A read-only target (Windows: FILE_ATTRIBUTE_READONLY; POSIX: no write bit, or this process
     * may not write it) fails with PermissionDenied unless options.overwriteReadOnly, which makes it
     * writable first (the saved file stays writable; on failure the attribute or mode is put back).
     * A file another program holds open without sharing fails with Busy after three short retries,
     * and is left as it was. Thread safety as the other members; blocking. O(size).
     * Errors: as writeFileAtomic, plus PermissionDenied (read-only), Busy (in use), NotFound (a
     * dangling symlink). The default implementation, for test doubles, is exists() then
     * writeFileAtomic() with result.target = `path`.
     */
    [[nodiscard]] virtual Result<SafeWriteResult> safeWrite(std::string_view path, std::string_view data,
                                                            const SafeWriteOptions& options);

    // Type, size and modification time, following symlinks. Errors: NotFound
    // (including dangling links), PermissionDenied, IoError.
    [[nodiscard]] virtual Result<FileStatus> status(std::string_view path) = 0;

    /*
     * The entries of a directory, sorted by name (byte-wise, i.e. by code point).
     * Errors: NotFound, InvalidArgument (not a directory), PermissionDenied, IoError.
     * Windows names that are not valid UTF-16 (lone surrogates) cannot be represented
     * and are skipped; POSIX names are returned as the kernel's bytes, valid UTF-8 or
     * not (see DirEntry). listDirectoryWithDiagnostics() reports both.
     */
    [[nodiscard]] virtual Result<std::vector<DirEntry>> listDirectory(std::string_view path) = 0;

    /*
     * As listDirectory(), plus an issue for every name it skipped (Windows: not valid UTF-16) or returned
     * as bytes that are not valid UTF-8 (POSIX), in name order. The scanner (tree_scan.hpp) turns them
     * into ScanIssues. Errors as listDirectory's. O(entries).
     */
    [[nodiscard]] virtual Result<DirListing> listDirectoryWithDiagnostics(std::string_view path) = 0;

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

    /*
     * The spelling that identifies the existing file or directory at `path`, so that two spellings of
     * one file compare equal as strings (the one document identity of the shell and the workbench,
     * ADR 0042, ADR 0044). Windows (the real file system): the real case of every component
     * ("c:\src\FOO.cpp" becomes "C:\src\Foo.cpp", 8.3 names become long names) with links, junctions
     * and subst drives kept as spelled: GetFinalPathNameByHandleW's answer when it differs from the
     * normalised input in ASCII case only, else one directory lookup per component. POSIX (the real
     * file system): normalizePath, or realpath's answer when the path has a ".." component. This
     * default (test doubles): normalizePath in the native style, links kept. Errors: InvalidArgument
     * (a relative path), NotFound, PermissionDenied, IoError. O(path length) plus one open, plus one
     * lookup per component on the Windows slow path.
     */
    [[nodiscard]] virtual Result<std::string> canonicalPath(std::string_view path);

protected:
    constexpr IFileSystem() noexcept = default;
    constexpr IFileSystem(const IFileSystem&) noexcept = default;
    constexpr IFileSystem& operator=(const IFileSystem&) noexcept = default;
    ~IFileSystem() = default;
};

// The process-wide real file system (wide Win32 APIs on Windows, POSIX on Linux).
// Stateless and thread-safe.
[[nodiscard]] IFileSystem& realFileSystem() noexcept;

// The parts of a writeFileAtomic / safeWrite temp-file name ".<name>.frost-tmp-<pid>-<n>".
struct AtomicTempName {
    std::uint32_t processId = 0;
    std::uint64_t id = 0;

    friend bool operator==(const AtomicTempName&, const AtomicTempName&) = default;
};

// Parses a directory entry name as a temp-file name (a leading '.', a non-empty stem, the marker,
// then decimal <pid>-<n>); nullopt for every other name. Pure. O(name).
[[nodiscard]] std::optional<AtomicTempName> parseAtomicTempName(std::string_view name) noexcept;

/*
 * Removes the temp files that killed processes left in `directory` (one level, no recursion):
 * regular files whose name parseAtomicTempName accepts, whose process id is not this process's
 * (its own saves may be in flight) and whose modification time is before `olderThan` (a live
 * writer in another process finishes long before that). Entries that vanish or cannot be removed
 * are skipped. Returns the number removed. Errors: only those of listing `directory`.
 * O(entries). Blocking; `style` is the path style of `fs` (MemFs may use the other one).
 */
[[nodiscard]] Result<std::size_t> removeStaleTempFiles(IFileSystem& fs, std::string_view directory,
                                                       IClock::SystemTime olderThan,
                                                       PathStyle style = kNativePathStyle);

} // namespace frost::io
