// SPDX-License-Identifier: Apache-2.0
//
// The real IFileSystem on POSIX (Linux): open/read/write/rename/stat/readdir,
// every descriptor opened with O_CLOEXEC so children never inherit it.

#include "frost/io/file_system.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"

#include "internal.hpp"
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
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
bool existsAsNonDirectory(const std::string& path) noexcept {
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && !S_ISDIR(info.st_mode);
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
        UniqueFd fd(::open(checked->c_str(), O_RDONLY | O_CLOEXEC));
        if (!fd.valid()) {
            return std::unexpected(errnoError(errno, "readFile", path));
        }
        struct stat info{};
        if (::fstat(fd.get(), &info) != 0) {
            return std::unexpected(errnoError(errno, "readFile", path));
        }
        if (S_ISDIR(info.st_mode)) {
            return std::unexpected(detail::isADirectoryError("readFile", path));
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
        // Keep an existing target's permission bits; a new file gets 0666 minus the umask.
        struct stat existing{};
        const bool hadTarget = ::stat(target->c_str(), &existing) == 0 && S_ISREG(existing.st_mode);

        std::string tempPath;
        UniqueFd fd;
        for (int attempt = 0; !fd.valid(); ++attempt) {
            auto candidate =
                joinPath(directory, detail::atomicTempName(name, detail::nextUniqueId()), PathStyle::Posix);
            if (!candidate) {
                return std::unexpected(std::move(candidate.error()));
            }
            tempPath = std::move(*candidate);
            fd.reset(::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666));
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
        if (hadTarget && ::fchmod(fd.get(), existing.st_mode & 07777) != 0) {
            return abandon(errnoError(errno, "writeFileAtomic: copy permissions for", path));
        }
        if (const int error = writeAll(fd.get(), data); error != 0) {
            return abandon(errnoError(error, "writeFileAtomic: write temp file for", path));
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
            Error error(ErrorCode::Unknown);
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
