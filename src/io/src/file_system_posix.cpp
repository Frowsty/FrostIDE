// SPDX-License-Identifier: Apache-2.0
//
// The real IFileSystem on POSIX (Linux): open/read/write/rename/stat/readdir,
// every descriptor opened with O_CLOEXEC so children never inherit it.

#include "frost/io/file_system.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"

#include "internal.hpp"
#include <sys/stat.h>
#include <sys/xattr.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <format>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

namespace frost::io {
namespace {

using detail::UniqueFd;

// Validates an absolute path and returns a NUL-terminated copy for the C APIs.
Result<std::string> checkedPath(std::string_view path) {
    if (!isAbsolutePath(path, PathStyle::Posix)) {
        if (path.find('\0') != std::string_view::npos) {
            return fail(ErrorCode::InvalidArgument, "path contains a NUL character");
        }
        return fail(ErrorCode::InvalidArgument, "not an absolute path: '" + std::string(path) + "'");
    }
    return std::string(path);
}

// A final ".." names the parent, not an entry: removeFile and removeDirectory refuse it
// (rmdir would report ENOTEMPTY, unlink EISDIR), as writeFileAtomic and MemFs do.
Result<void> checkEntryName(std::string_view checked, std::string_view operation, std::string_view path) {
    auto normalized = normalizePath(checked, PathStyle::Posix);
    if (normalized && fileName(*normalized, PathStyle::Posix) == "..") {
        return fail(ErrorCode::InvalidArgument,
                    std::string(operation) + " '" + std::string(path) + "': '..' is not an entry name");
    }
    return {};
}

IClock::SystemTime toSystemTime(const timespec& time) noexcept {
    return IClock::SystemTime(std::chrono::seconds(time.tv_sec) + std::chrono::nanoseconds(time.tv_nsec));
}

FileType typeFromMode(mode_t mode) noexcept {
    if (S_ISREG(mode)) {
        return FileType::Regular;
    }
    if (S_ISDIR(mode)) {
        return FileType::Directory;
    }
    if (S_ISLNK(mode)) {
        return FileType::Symlink;
    }
    return FileType::Other;
}

bool isDirectory(const std::string& path) noexcept {
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// ENOTDIR means either "the target is not a directory" or "a prefix component is
// not one"; only the first is reported as InvalidArgument, the second is NotFound.
// lstat: a symlink is itself "not a directory", wherever it points.
bool existsAsNonDirectory(const std::string& path) noexcept {
    struct stat info{};
    return ::lstat(path.c_str(), &info) == 0 && !S_ISDIR(info.st_mode);
}

Error notARegularFile(std::string_view path) {
    return Error(ErrorCode::InvalidArgument, "readFile '" + std::string(path) + "': not a regular file");
}

/*
 * Opens `checked` for readFile. O_NONBLOCK: opening a FIFO without a writer would
 * otherwise block forever (readFile clears the flag once fstat shows a regular
 * file). O_NOCTTY: opening a terminal must not make it our controlling terminal.
 * Two errors of the non-blocking open need a second look:
 *   - EWOULDBLOCK on a regular file: another process holds a write lease (Samba
 *     oplocks, NFS delegations). The failed open has still started the lease
 *     break (fcntl(2), "Leases"), so retrying succeeds once the holder lets go, or
 *     once the kernel revokes the lease after /proc/sys/fs/lease-break-time (45 s by
 *     default). Retrying without blocking, rather than reopening in blocking mode,
 *     means a FIFO swapped in meanwhile still cannot hang the call.
 *   - ENXIO: sockets refuse open(); they are reported as "not a regular file".
 */
Result<UniqueFd> openForReading(const std::string& checked, std::string_view path) {
    using namespace std::chrono_literals;
    const auto giveUp = std::chrono::steady_clock::now() + 90s;
    auto pause = 1ms;
    for (;;) {
        UniqueFd fd(::open(checked.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY));
        if (fd.valid()) {
            return fd;
        }
        const int error = errno;
        struct stat info{};
        const bool known = ::stat(checked.c_str(), &info) == 0;
        if (error == ENXIO && known && !S_ISREG(info.st_mode)) {
            return std::unexpected(notARegularFile(path));
        }
        // EWOULDBLOCK is EAGAIN on Linux.
        if (error == EWOULDBLOCK && known && S_ISREG(info.st_mode) && std::chrono::steady_clock::now() < giveUp) {
            std::this_thread::sleep_for(pause);
            pause = std::min(pause * 2, std::chrono::milliseconds(50));
            continue;
        }
        return std::unexpected(errnoError(error, "readFile", path));
    }
}

// The extended attribute that holds a file's POSIX access ACL (acl(5)).
constexpr const char* kAccessAcl = "system.posix_acl_access";

// Whether an xattr call's errno value means "no ACL here": none is set (ENODATA), or the
// file system has no ACLs (EOPNOTSUPP, which is also ENOTSUP on Linux; ENOSYS for some FUSE).
bool meansNoAcl(int error) noexcept { return error == ENODATA || error == EOPNOTSUPP || error == ENOSYS; }

// Reads the access ACL of `path` (following symlinks, like stat) into `acl`: nullopt when
// it has none. Returns 0 or an errno value.
int readAccessAcl(const std::string& path, std::optional<std::string>& acl) {
    acl.reset();
    for (int attempt = 0; attempt < 4; ++attempt) {
        const ssize_t size = ::getxattr(path.c_str(), kAccessAcl, nullptr, 0);
        if (size <= 0) {
            if (size == 0) {
                acl.emplace(); // Malformed, so treated as granting non-owners nothing.
                return 0;
            }
            return meansNoAcl(errno) ? 0 : errno;
        }
        std::string value(static_cast<std::size_t>(size), '\0');
        const ssize_t got = ::getxattr(path.c_str(), kAccessAcl, value.data(), value.size());
        if (got >= 0) {
            value.resize(static_cast<std::size_t>(got));
            acl = std::move(value);
            return 0;
        }
        if (errno != ERANGE) { // ERANGE: the ACL grew between the two calls; ask again.
            return meansNoAcl(errno) ? 0 : errno;
        }
    }
    return ERANGE;
}

/*
 * Gives the temp file `fd` the replaced target's owner, group, permission bits and
 * access ACL (`acl`, nullopt if it has none), as far as this process may: the owner
 * only with CAP_CHOWN, the group only if this process belongs to it (some file
 * systems, such as vfat or drvfs, ignore both), and the ACL only when both are kept.
 * Whatever cannot be kept fails safe (detail::replacementMode). Order matters:
 *   1. An access ACL the temp file inherited from the directory's default ACL is
 *      removed while this process still owns the file: it is not the target's, and
 *      its named entries would read the target's group bits as their mask.
 *   2. fchown, which clears the set-id bits, so it precedes fchmod.
 *   3. fchmod with the fail-safe mode, which is what remains if the ACL copy fails.
 *   4. The ACL, after fchmod, which would otherwise overwrite its mask. Best effort:
 *      the kernel derives the group bits from the ACL mask and keeps the set-id bits.
 * Returns 0 or an errno value.
 */
int copyOwnershipAndMode(int fd, const struct stat& existing, const std::optional<std::string>& acl) noexcept {
    if (::fremovexattr(fd, kAccessAcl) != 0 && !meansNoAcl(errno)) {
        return errno;
    }
    if (::fchown(fd, existing.st_uid, existing.st_gid) != 0) {
        static_cast<void>(::fchown(fd, static_cast<uid_t>(-1), existing.st_gid)); // EPERM is fine.
    }
    struct stat now{};
    if (::fstat(fd, &now) != 0) {
        return errno;
    }
    const bool ownerKept = now.st_uid == existing.st_uid;
    const bool groupKept = now.st_gid == existing.st_gid;
    std::optional<detail::AclFloor> floor;
    if (acl) {
        // A value this code cannot parse grants the non-owner classes nothing until it is copied.
        floor = detail::accessAclFloor(*acl).value_or(detail::AclFloor{});
    }
    const auto mode =
        static_cast<mode_t>(detail::replacementMode(existing.st_mode & 07777, ownerKept, groupKept, floor));
    if (::fchmod(fd, mode) != 0) {
        return errno;
    }
    if (acl && ownerKept && groupKept) {
        static_cast<void>(::fsetxattr(fd, kAccessAcl, acl->data(), acl->size(), 0));
    }
    return 0;
}

// Writes all of `data`, retrying on EINTR and short writes. Returns 0 or an errno value.
int writeAll(int fd, std::string_view data) noexcept {
    while (!data.empty()) {
        const ssize_t done = ::write(fd, data.data(), data.size());
        if (done < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        data.remove_prefix(static_cast<std::size_t>(done));
    }
    return 0;
}

enum class MkdirOutcome : unsigned char { Done, ParentMissing, Failed };

class PosixFileSystem final : public IFileSystem {
public:
    constexpr PosixFileSystem() noexcept = default;

    Result<std::string> readFile(std::string_view path) override {
        auto checked = checkedPath(path);
        if (!checked) {
            return std::unexpected(std::move(checked.error()));
        }
        auto opened = openForReading(*checked, path);
        if (!opened) {
            return std::unexpected(std::move(opened.error()));
        }
        const UniqueFd fd = std::move(*opened);
        struct stat info{};
        if (::fstat(fd.get(), &info) != 0) {
            return std::unexpected(errnoError(errno, "readFile", path));
        }
        if (S_ISDIR(info.st_mode)) {
            return std::unexpected(detail::isADirectoryError("readFile", path));
        }
        // FIFOs, sockets and devices (/dev/zero never ends) are not files to read whole.
        if (!S_ISREG(info.st_mode)) {
            return std::unexpected(notARegularFile(path));
        }
        const int flags = ::fcntl(fd.get(), F_GETFL);
        if (flags < 0 || ::fcntl(fd.get(), F_SETFL, flags & ~O_NONBLOCK) != 0) {
            return std::unexpected(errnoError(errno, "readFile", path));
        }
        // One spare byte lets the final zero-byte read land without reallocating.
        std::string out;
        out.resize(static_cast<std::size_t>(std::max<off_t>(info.st_size, 0)) + 1);
        std::size_t used = 0;
        for (;;) {
            if (used == out.size()) {
                out.resize(out.size() + 64 * 1024);
            }
            const ssize_t got = ::read(fd.get(), out.data() + used, out.size() - used);
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return std::unexpected(errnoError(errno, "readFile", path));
            }
            if (got == 0) {
                break;
            }
            used += static_cast<std::size_t>(got);
        }
        out.resize(used);
        return out;
    }

    Result<void> writeFileAtomic(std::string_view path, std::string_view data) override {
        auto target = normalizePath(path, PathStyle::Posix);
        if (!target) {
            return std::unexpected(std::move(target.error()));
        }
        const std::string directory(parentPath(*target, PathStyle::Posix));
        const std::string_view name = fileName(*target, PathStyle::Posix);
        if (directory.empty() || name.empty() || name == "..") {
            return fail(ErrorCode::InvalidArgument, "writeFileAtomic '" + std::string(path) + "': not a file path");
        }
        /*
         * Replacing a file: the temp file is created 0600 and gets the target's owner,
         * group, mode and access ACL (copyOwnershipAndMode) only once its content is
         * complete, so no one else can open it meanwhile. Creating one: the temp file
         * is created 0666 like any open(O_CREAT, 0666), so the umask or the directory's
         * default ACL decides its mode, exactly as for files other programs create there.
         */
        struct stat existing{};
        const bool hadTarget = ::stat(target->c_str(), &existing) == 0 && S_ISREG(existing.st_mode);
        const mode_t createMode = hadTarget ? 0600 : 0666;
        std::optional<std::string> acl;
        if (hadTarget) {
            if (const int error = readAccessAcl(*target, acl); error != 0) {
                return std::unexpected(errnoError(error, "writeFileAtomic: read the access ACL of", path));
            }
        }

        std::string tempPath;
        UniqueFd fd;
        for (int attempt = 0; !fd.valid(); ++attempt) {
            auto candidate =
                joinPath(directory, detail::atomicTempName(name, detail::nextUniqueId()), PathStyle::Posix);
            if (!candidate) {
                return std::unexpected(std::move(candidate.error()));
            }
            tempPath = std::move(*candidate);
            fd.reset(::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, createMode));
            if (!fd.valid() && (errno != EEXIST || attempt >= 16)) {
                return std::unexpected(errnoError(errno, "writeFileAtomic: create temp file for", path));
            }
        }

        // From here on every failure removes the temp file.
        const auto abandon = [&](Error error) -> Result<void> {
            fd.reset();
            ::unlink(tempPath.c_str());
            return std::unexpected(std::move(error));
        };
        if (const int error = writeAll(fd.get(), data); error != 0) {
            return abandon(errnoError(error, "writeFileAtomic: write temp file for", path));
        }
        if (hadTarget) {
            if (const int error = copyOwnershipAndMode(fd.get(), existing, acl); error != 0) {
                return abandon(errnoError(error, "writeFileAtomic: copy owner and permissions for", path));
            }
        }
        if (::fsync(fd.get()) != 0) {
            return abandon(errnoError(errno, "writeFileAtomic: flush temp file for", path));
        }
        if (::close(fd.release()) != 0) {
            return abandon(errnoError(errno, "writeFileAtomic: close temp file for", path));
        }
        if (::rename(tempPath.c_str(), target->c_str()) != 0) {
            const int error = errno;
            if (error == EISDIR) {
                return abandon(detail::isADirectoryError("writeFileAtomic", path));
            }
            return abandon(errnoError(error, "writeFileAtomic: replace", path));
        }
        // Make the rename itself durable. Best effort: the new content is already in place,
        // and some file systems refuse to fsync directories (EINVAL).
        if (UniqueFd dirFd(::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)); dirFd.valid()) {
            static_cast<void>(::fsync(dirFd.get()));
        }
        return {};
    }

    Result<FileStatus> status(std::string_view path) override {
        auto checked = checkedPath(path);
        if (!checked) {
            return std::unexpected(std::move(checked.error()));
        }
        struct stat info{};
        if (::stat(checked->c_str(), &info) != 0) {
            return std::unexpected(errnoError(errno, "status", path));
        }
        FileStatus result;
        result.type = typeFromMode(info.st_mode);
        result.size =
            result.type == FileType::Directory ? 0 : static_cast<std::uint64_t>(std::max<off_t>(info.st_size, 0));
        result.mtime = toSystemTime(info.st_mtim);
        return result;
    }

    Result<std::vector<DirEntry>> listDirectory(std::string_view path) override {
        auto checked = checkedPath(path);
        if (!checked) {
            return std::unexpected(std::move(checked.error()));
        }
        UniqueFd dirFd(::open(checked->c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (!dirFd.valid()) {
            const int error = errno;
            if (error == ENOTDIR && existsAsNonDirectory(*checked)) {
                return std::unexpected(detail::notADirectoryError("listDirectory", path));
            }
            return std::unexpected(errnoError(error, "listDirectory", path));
        }
        DIR* dir = ::fdopendir(dirFd.get());
        if (dir == nullptr) {
            return std::unexpected(errnoError(errno, "listDirectory", path));
        }
        static_cast<void>(dirFd.release()); // closedir() now owns the descriptor.
        struct DirGuard {
            DIR* dir;
            ~DirGuard() { ::closedir(dir); }
        } guard{dir};

        std::vector<DirEntry> entries;
        for (;;) {
            errno = 0;
            const dirent* entry = ::readdir(dir);
            if (entry == nullptr) {
                if (errno != 0) {
                    return std::unexpected(errnoError(errno, "listDirectory", path));
                }
                break;
            }
            const std::string_view name(entry->d_name);
            if (name == "." || name == "..") {
                continue;
            }
            FileType type = FileType::Other;
            switch (entry->d_type) {
            case DT_REG: type = FileType::Regular; break;
            case DT_DIR: type = FileType::Directory; break;
            case DT_LNK: type = FileType::Symlink; break;
            case DT_UNKNOWN: {
                // Some file systems (older XFS, some FUSE) do not fill d_type.
                struct stat info{};
                if (::fstatat(::dirfd(dir), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) == 0) {
                    type = typeFromMode(info.st_mode);
                }
                break;
            }
            default: break;
            }
            entries.push_back(DirEntry{std::string(name), type});
        }
        std::ranges::sort(entries, {}, &DirEntry::name);
        return entries;
    }

    Result<DirListing> listDirectoryWithDiagnostics(std::string_view path) override {
        auto entries = listDirectory(path);
        if (!entries) {
            return std::unexpected(std::move(entries.error()));
        }
        DirListing listing;
        for (const DirEntry& entry : *entries) {
            if (!isValidUtf8(entry.name)) {
                listing.issues.push_back(DirListingIssue{
                    entry.name, true,
                    Error(ErrorCode::InvalidArgument,
                          std::format("listDirectory '{}': a name of {} bytes is not valid UTF-8 after byte {}", path,
                                      entry.name.size(), validUtf8Prefix(entry.name)))});
            }
        }
        listing.entries = std::move(*entries);
        return listing;
    }

    Result<void> createDirectories(std::string_view path) override {
        auto normalized = normalizePath(path, PathStyle::Posix);
        if (!normalized) {
            return std::unexpected(std::move(normalized.error()));
        }
        // Walk up until a directory can be created or exists, then create the rest downwards.
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
            current = parentPath(current, PathStyle::Posix);
            if (current.empty()) {
                return fail(ErrorCode::NotFound,
                            "createDirectories '" + std::string(path) + "': the root does not exist");
            }
        }
        while (!missing.empty()) {
            // ParentMissing here means the parent just made (or found) is gone again; the error names it.
            const std::string_view child = missing.back();
            Error error(ErrorCode::NotFound, "createDirectories '" + std::string(path) + "': '" +
                                                 std::string(parentPath(child, PathStyle::Posix)) +
                                                 "' vanished before '" + std::string(child) + "' was created");
            if (makeDirectory(missing.back(), path, error) != MkdirOutcome::Done) {
                return std::unexpected(std::move(error));
            }
            missing.pop_back();
        }
        return {};
    }

    Result<void> removeFile(std::string_view path) override {
        auto checked = checkedPath(path);
        if (!checked) {
            return std::unexpected(std::move(checked.error()));
        }
        if (auto entry = checkEntryName(*checked, "removeFile", path); !entry) {
            return entry;
        }
        if (::unlink(checked->c_str()) != 0) {
            const int error = errno;
            // Linux reports EISDIR, POSIX allows EPERM for directories.
            if ((error == EISDIR || error == EPERM) && isDirectory(*checked)) {
                return std::unexpected(detail::isADirectoryError("removeFile", path));
            }
            return std::unexpected(errnoError(error, "removeFile", path));
        }
        return {};
    }

    Result<void> removeDirectory(std::string_view path) override {
        auto checked = checkedPath(path);
        if (!checked) {
            return std::unexpected(std::move(checked.error()));
        }
        if (auto entry = checkEntryName(*checked, "removeDirectory", path); !entry) {
            return entry;
        }
        if (::rmdir(checked->c_str()) != 0) {
            const int error = errno;
            if (error == ENOTDIR && existsAsNonDirectory(*checked)) {
                return std::unexpected(detail::notADirectoryError("removeDirectory", path));
            }
            return std::unexpected(errnoError(error, "removeDirectory", path));
        }
        return {};
    }

    Result<bool> exists(std::string_view path) override {
        auto result = status(path);
        if (result) {
            return true;
        }
        if (result.error().code() == ErrorCode::NotFound) {
            return false;
        }
        return std::unexpected(std::move(result.error()));
    }

private:
    // Creates one directory. Done if it was created or already is a directory;
    // ParentMissing if its parent does not exist; otherwise Failed with `error` set.
    static MkdirOutcome makeDirectory(std::string_view normalized, std::string_view original, Error& error) {
        const std::string path(normalized);
        if (::mkdir(path.c_str(), 0777) == 0) {
            return MkdirOutcome::Done;
        }
        const int code = errno;
        // ENOTDIR: a prefix component is a file; walking up reports it as AlreadyExists, as on Windows.
        if (code == ENOENT || code == ENOTDIR) {
            return MkdirOutcome::ParentMissing;
        }
        if (code == EEXIST) {
            if (isDirectory(path)) {
                return MkdirOutcome::Done;
            }
            error = Error(ErrorCode::AlreadyExists, "createDirectories '" + std::string(original) + "': '" + path +
                                                        "' exists and is not a directory");
            return MkdirOutcome::Failed;
        }
        error = errnoError(code, "createDirectories", original);
        return MkdirOutcome::Failed;
    }
};

// constinit and trivially destructible: no code runs before main or after exit.
constinit PosixFileSystem g_realFileSystem;

} // namespace

IFileSystem& realFileSystem() noexcept { return g_realFileSystem; }

} // namespace frost::io
