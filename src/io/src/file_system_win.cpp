// SPDX-License-Identifier: Apache-2.0
//
// The real IFileSystem on Windows: wide Win32 APIs only, every path in the
// extended-length (\\?\) form so MAX_PATH never applies (see frost/io/path.hpp).

#include "frost/io/file_system.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"

#include "internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>

#include <windows.h>

namespace frost::io {
namespace {

using detail::UniqueHandle;

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
// Largest single ReadFile/WriteFile request: 1 GiB keeps the DWORD byte count far from overflow.
constexpr std::size_t kMaxIoChunk = std::size_t{1} << 30;

// FILETIME counts 100 ns ticks since 1601-01-01; the system clock counts from 1970-01-01.
IClock::SystemTime toSystemTime(FILETIME time) noexcept {
    constexpr std::int64_t kTicksFrom1601To1970 = 116'444'736'000'000'000;
    // Nanoseconds in int64 cover 1678..2262; clamp ticks so earlier or later stamps saturate.
    constexpr std::int64_t kMaxTicks = std::numeric_limits<std::int64_t>::max() / 100;
    const std::uint64_t raw = (std::uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime;
    std::int64_t ticks =
        static_cast<std::int64_t>(std::min<std::uint64_t>(raw, std::uint64_t{1} << 62)) - kTicksFrom1601To1970;
    ticks = std::clamp(ticks, -kMaxTicks, kMaxTicks);
    return IClock::SystemTime(std::chrono::nanoseconds(ticks * 100));
}

// GetFileAttributesW, or INVALID_FILE_ATTRIBUTES.
DWORD attributesOf(const std::wstring& widePath) noexcept { return GetFileAttributesW(widePath.c_str()); }

bool isDirectory(const std::wstring& widePath) noexcept {
    const DWORD attributes = attributesOf(widePath);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Outcome of creating one directory, for createDirectories' walk.
enum class MkdirOutcome : unsigned char { Done, ParentMissing, Failed };

class WindowsFileSystem final : public IFileSystem {
public:
    constexpr WindowsFileSystem() noexcept = default;

    Result<std::string> readFile(std::string_view path) override {
        auto wide = toWin32Path(path);
        if (!wide) {
            return std::unexpected(std::move(wide.error()));
        }
        UniqueHandle file(CreateFileW(wide->c_str(), GENERIC_READ, kShareAll, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file.valid()) {
            const DWORD error = GetLastError();
            if (error == ERROR_ACCESS_DENIED && isDirectory(*wide)) {
                return std::unexpected(detail::isADirectoryError("readFile", path));
            }
            return std::unexpected(win32Error(error, "readFile", path));
        }
        LARGE_INTEGER size{};
        if (GetFileSizeEx(file.get(), &size) == FALSE) {
            return std::unexpected(lastWin32Error("readFile", path));
        }
        // One spare byte lets the final zero-byte read land without reallocating; a file
        // that grows meanwhile is read to its new end.
        std::string out;
        out.resize(static_cast<std::size_t>(size.QuadPart) + 1);
        std::size_t used = 0;
        for (;;) {
            if (used == out.size()) {
                out.resize(out.size() + 64 * 1024);
            }
            const DWORD want = static_cast<DWORD>(std::min(out.size() - used, kMaxIoChunk));
            DWORD got = 0;
            if (ReadFile(file.get(), out.data() + used, want, &got, nullptr) == FALSE) {
                return std::unexpected(lastWin32Error("readFile", path));
            }
            if (got == 0) {
                break;
            }
            used += got;
        }
        out.resize(used);
        return out;
    }

    Result<void> writeFileAtomic(std::string_view path, std::string_view data) override {
        auto target = normalizePath(path, PathStyle::Windows);
        if (!target) {
            return std::unexpected(std::move(target.error()));
        }
        const std::string_view directory = parentPath(*target, PathStyle::Windows);
        const std::string_view name = fileName(*target, PathStyle::Windows);
        if (directory.empty() || name.empty()) {
            return fail(ErrorCode::InvalidArgument, "writeFileAtomic '" + std::string(path) + "': not a file path");
        }
        auto wideTarget = toWin32Path(*target);
        if (!wideTarget) {
            return std::unexpected(std::move(wideTarget.error()));
        }

        // Create the temp file exclusively; a name collision (a stale file from a killed
        // process that reused our pid) just moves on to the next id.
        std::wstring wideTemp;
        UniqueHandle file;
        for (int attempt = 0; !file.valid(); ++attempt) {
            auto tempPath =
                joinPath(directory, detail::atomicTempName(name, detail::nextUniqueId()), PathStyle::Windows);
            if (!tempPath) {
                return std::unexpected(std::move(tempPath.error()));
            }
            auto wide = toWin32Path(*tempPath);
            if (!wide) {
                return std::unexpected(std::move(wide.error()));
            }
            wideTemp = std::move(*wide);
            file.reset(
                CreateFileW(wideTemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!file.valid()) {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_EXISTS || attempt >= 16) {
                    return std::unexpected(win32Error(error, "writeFileAtomic: create temp file for", path));
                }
            }
        }

        // From here on every failure removes the temp file.
        const auto abandon = [&](Error error) -> Result<void> {
            file.reset();
            DeleteFileW(wideTemp.c_str());
            return std::unexpected(std::move(error));
        };
        for (std::size_t written = 0; written < data.size();) {
            const DWORD want = static_cast<DWORD>(std::min(data.size() - written, kMaxIoChunk));
            DWORD done = 0;
            if (WriteFile(file.get(), data.data() + written, want, &done, nullptr) == FALSE) {
                return abandon(lastWin32Error("writeFileAtomic: write temp file for", path));
            }
            written += done;
        }
        if (FlushFileBuffers(file.get()) == FALSE) {
            return abandon(lastWin32Error("writeFileAtomic: flush temp file for", path));
        }
        if (CloseHandle(file.release()) == FALSE) {
            return abandon(lastWin32Error("writeFileAtomic: close temp file for", path));
        }
        // One rename on the same volume: NTFS and ReFS make it atomic for every reader.
        if (MoveFileExW(wideTemp.c_str(), wideTarget->c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
            FALSE) {
            const DWORD error = GetLastError();
            if (error == ERROR_ACCESS_DENIED && isDirectory(*wideTarget)) {
                return abandon(detail::isADirectoryError("writeFileAtomic", path));
            }
            return abandon(win32Error(error, "writeFileAtomic: replace", path));
        }
        return {};
    }

    Result<FileStatus> status(std::string_view path) override {
        auto wide = toWin32Path(path);
        if (!wide) {
            return std::unexpected(std::move(wide.error()));
        }
        // Opening the path (rather than GetFileAttributesExW) follows symlinks and junctions;
        // FILE_FLAG_BACKUP_SEMANTICS is required to open directories.
        UniqueHandle handle(CreateFileW(wide->c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!handle.valid()) {
            const DWORD error = GetLastError();
            if (error != ERROR_SHARING_VIOLATION) {
                return std::unexpected(win32Error(error, "status", path));
            }
            // A few system files (pagefile.sys) refuse even attribute-only opens; their
            // directory entry still answers, though without following links.
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(wide->c_str(), GetFileExInfoStandard, &data) == FALSE) {
                return std::unexpected(lastWin32Error("status", path));
            }
            const bool directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            return FileStatus{directory ? FileType::Directory : FileType::Regular,
                              directory ? 0 : (std::uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow,
                              toSystemTime(data.ftLastWriteTime)};
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileInformationByHandle(handle.get(), &info) == FALSE) {
            return std::unexpected(lastWin32Error("status", path));
        }
        FileStatus result;
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            result.type = FileType::Directory;
        } else {
            result.type = GetFileType(handle.get()) == FILE_TYPE_DISK ? FileType::Regular : FileType::Other;
            result.size = (std::uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
        }
        result.mtime = toSystemTime(info.ftLastWriteTime);
        return result;
    }

    Result<std::vector<DirEntry>> listDirectory(std::string_view path) override {
        auto wide = toWin32Path(path);
        if (!wide) {
            return std::unexpected(std::move(wide.error()));
        }
        std::wstring pattern = *wide;
        if (pattern.back() != L'\\') {
            pattern += L'\\';
        }
        pattern += L'*';
        WIN32_FIND_DATAW data{};
        HANDLE raw = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                      FIND_FIRST_EX_LARGE_FETCH);
        std::vector<DirEntry> entries;
        if (raw == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            const DWORD attributes = attributesOf(*wide);
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                return std::unexpected(detail::notADirectoryError("listDirectory", path));
            }
            if (error == ERROR_FILE_NOT_FOUND && attributes != INVALID_FILE_ATTRIBUTES) {
                return entries; // An empty volume root has no "." or "..".
            }
            return std::unexpected(win32Error(error, "listDirectory", path));
        }
        // FindClose, not CloseHandle, releases a find handle.
        struct FindGuard {
            HANDLE handle;
            ~FindGuard() { FindClose(handle); }
        } guard{raw};
        do {
            const std::wstring_view name(data.cFileName);
            if (name == L"." || name == L"..") {
                continue;
            }
            auto utf8 = fromWide(name);
            if (!utf8) {
                continue; // Not representable as UTF-8 (lone surrogate); see file_system.hpp.
            }
            FileType type = FileType::Regular;
            const DWORD attributes = data.dwFileAttributes;
            const bool link =
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
                (data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT);
            if (link) {
                type = FileType::Symlink;
            } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                type = FileType::Directory;
            } else if ((attributes & FILE_ATTRIBUTE_DEVICE) != 0) {
                type = FileType::Other;
            }
            entries.push_back(DirEntry{std::move(*utf8), type});
        } while (FindNextFileW(raw, &data) != FALSE);
        if (const DWORD error = GetLastError(); error != ERROR_NO_MORE_FILES) {
            return std::unexpected(win32Error(error, "listDirectory", path));
        }
        std::ranges::sort(entries, {}, &DirEntry::name);
        return entries;
    }

    Result<void> createDirectories(std::string_view path) override {
        auto normalized = normalizePath(path, PathStyle::Windows);
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
            current = parentPath(current, PathStyle::Windows);
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
        auto wide = toWin32Path(path);
        if (!wide) {
            return std::unexpected(std::move(wide.error()));
        }
        if (DeleteFileW(wide->c_str()) == FALSE) {
            const DWORD error = GetLastError();
            if (error == ERROR_ACCESS_DENIED && isDirectory(*wide)) {
                return std::unexpected(detail::isADirectoryError("removeFile", path));
            }
            return std::unexpected(win32Error(error, "removeFile", path));
        }
        return {};
    }

    Result<void> removeDirectory(std::string_view path) override {
        auto wide = toWin32Path(path);
        if (!wide) {
            return std::unexpected(std::move(wide.error()));
        }
        if (RemoveDirectoryW(wide->c_str()) == FALSE) {
            const DWORD error = GetLastError();
            if (error == ERROR_DIRECTORY) {
                return std::unexpected(detail::notADirectoryError("removeDirectory", path));
            }
            return std::unexpected(win32Error(error, "removeDirectory", path));
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
        auto wide = toWin32Path(normalized);
        if (!wide) {
            error = std::move(wide.error());
            return MkdirOutcome::Failed;
        }
        if (CreateDirectoryW(wide->c_str(), nullptr) != FALSE) {
            return MkdirOutcome::Done;
        }
        const DWORD code = GetLastError();
        if (code == ERROR_PATH_NOT_FOUND) {
            return MkdirOutcome::ParentMissing;
        }
        // Volume roots answer ERROR_ACCESS_DENIED rather than ERROR_ALREADY_EXISTS.
        const DWORD attributes = attributesOf(*wide);
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                return MkdirOutcome::Done;
            }
            error = Error(ErrorCode::AlreadyExists, "createDirectories '" + std::string(original) + "': '" +
                                                        std::string(normalized) + "' exists and is not a directory");
            return MkdirOutcome::Failed;
        }
        error = win32Error(code, "createDirectories", original);
        return MkdirOutcome::Failed;
    }
};

// constinit and trivially destructible: no code runs before main or after exit.
constinit WindowsFileSystem g_realFileSystem;

} // namespace

IFileSystem& realFileSystem() noexcept { return g_realFileSystem; }

} // namespace frost::io
