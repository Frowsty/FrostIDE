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
#include <format>
#include <limits>
#include <string>
#include <string_view>
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

// `name` as UTF-8 with every unpaired surrogate replaced by U+FFFD (for messages only).
std::string lossyUtf8(std::wstring_view name) {
    std::wstring repaired(name);
    for (std::size_t i = 0; i < repaired.size(); ++i) {
        const wchar_t unit = repaired[i];
        const bool high = unit >= 0xD800 && unit <= 0xDBFF;
        const bool low = unit >= 0xDC00 && unit <= 0xDFFF;
        if (high && i + 1 < repaired.size() && repaired[i + 1] >= 0xDC00 && repaired[i + 1] <= 0xDFFF) {
            ++i; // A valid pair.
        } else if (high || low) {
            repaired[i] = 0xFFFD;
        }
    }
    return fromWide(repaired).value_or(std::string("?"));
}

// Whether the entry at `widePath` itself (a directory link included) has the directory attribute.
bool isDirectory(const std::wstring& widePath) noexcept {
    const DWORD attributes = attributesOf(widePath);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/*
 * Whether `widePath` reaches a directory once symlinks and junctions are followed,
 * as status() decides it (and POSIX stat). A dangling link is not one: opening it
 * fails with a NotFound code. When the open fails for another reason (a sharing
 * violation, access denied), the entry's own directory attribute decides.
 */
bool reachesDirectory(const std::wstring& widePath) noexcept {
    const UniqueHandle handle(CreateFileW(widePath.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle.valid()) {
        return errorCodeFromWin32(GetLastError()) != ErrorCode::NotFound && isDirectory(widePath);
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(handle.get(), &info) == FALSE) {
        return isDirectory(widePath);
    }
    return (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// Whether the entry at `widePath` is itself (not following it) a symbolic link or a junction,
// the reparse points listDirectory reports as FileType::Symlink. Other reparse points
// (OneDrive placeholders, deduplicated files) are ordinary files and directories.
bool isLink(const std::wstring& widePath) noexcept {
    const UniqueHandle handle(CreateFileW(widePath.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid()) {
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &info, sizeof(info)) == FALSE) {
        return false;
    }
    return (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
           (info.ReparseTag == IO_REPARSE_TAG_SYMLINK || info.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT);
}

// A temp file created beside its target: its UTF-8 and Win32 spellings and the open handle.
struct TempFile {
    std::string path;
    std::wstring widePath;
    UniqueHandle handle;
};

/*
 * Creates writeFileAtomic's temp file for `name` in `directory` exclusively, for writing; a name
 * collision (a stale file from a killed process that reused our pid) moves on to the next id.
 * `operation` names the caller in the error ("writeFileAtomic: create temp file for").
 */
Result<TempFile> createTempFile(std::string_view directory, std::string_view name, std::string_view operation,
                                std::string_view path) {
    TempFile temp;
    for (int attempt = 0; !temp.handle.valid(); ++attempt) {
        auto tempPath = joinPath(directory, detail::atomicTempName(name, detail::nextUniqueId()), PathStyle::Windows);
        if (!tempPath) {
            return std::unexpected(std::move(tempPath.error()));
        }
        auto wide = toWin32Path(*tempPath);
        if (!wide) {
            return std::unexpected(std::move(wide.error()));
        }
        temp.path = std::move(*tempPath);
        temp.widePath = std::move(*wide);
        temp.handle.reset(
            CreateFileW(temp.widePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!temp.handle.valid()) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_EXISTS || attempt >= 16) {
                return std::unexpected(win32Error(error, operation, path));
            }
        }
    }
    return temp;
}

// Writes all of `data` at the file's current position. Returns ERROR_SUCCESS or the Win32 error.
DWORD writeAll(HANDLE file, std::string_view data) noexcept {
    for (std::size_t written = 0; written < data.size();) {
        const DWORD want = static_cast<DWORD>(std::min(data.size() - written, kMaxIoChunk));
        DWORD done = 0;
        if (WriteFile(file, data.data() + written, want, &done, nullptr) == FALSE) {
            return GetLastError();
        }
        written += done;
    }
    return ERROR_SUCCESS;
}

// ASCII-case-insensitive equality; the file system folds more than ASCII, but a final path that
// differs beyond ASCII case is treated as another spelling.
bool equalIgnoringAsciiCase(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](char x, char y) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        return lower(x) == lower(y);
    });
}

/*
 * The existing, normalised `normalized` with each component after the root in the name the
 * directory lists it under (real case, long name), links and subst drives kept as spelled; the
 * drive letter is upper-cased. One FindFirstFileExW per component. `path` names the call in errors.
 */
Result<std::string> realCaseOfComponents(const std::string& normalized, std::string_view path) {
    std::vector<std::string_view> names;
    std::string_view current = normalized;
    for (std::string_view parent = parentPath(current, PathStyle::Windows); !parent.empty();
         parent = parentPath(current, PathStyle::Windows)) {
        names.push_back(fileName(current, PathStyle::Windows));
        current = parent;
    }
    std::string out(current);
    if (out.size() >= 2 && out[1] == ':' && out[0] >= 'a' && out[0] <= 'z') {
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        auto candidate = joinPath(out, *it, PathStyle::Windows);
        if (!candidate) {
            return candidate;
        }
        auto wide = toWin32Path(*candidate);
        if (!wide) {
            return std::unexpected(std::move(wide.error()));
        }
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW(wide->c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE) {
            return std::unexpected(lastWin32Error("canonicalPath", path));
        }
        FindClose(find);
        auto name = fromWide(data.cFileName);
        if (!name) {
            return std::unexpected(std::move(name.error()));
        }
        auto joined = joinPath(out, *name, PathStyle::Windows);
        if (!joined) {
            return joined;
        }
        out = std::move(*joined);
    }
    return out;
}

// safeWrite's retries of a replace that another program's open handle blocks (antivirus scanners
// and indexers hold files for milliseconds).
constexpr int kBusyRetries = 3;
constexpr DWORD kBusyRetryMs = 30;

// The attributes SetFileAttributesW accepts, for putting a read-only file's attributes back.
constexpr DWORD kSettableAttributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                      FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED |
                                      FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_TEMPORARY;

/*
 * The normalised UTF-8 path GetFinalPathNameByHandleW reports for `handle`, with the \\?\ and
 * \\?\UNC\ prefixes turned back into "C:\..." and "\\server\share\...". `what` and `path` name the
 * operation in errors.
 */
Result<std::string> finalPathOfHandle(HANDLE handle, std::string_view what, std::string_view path) {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                       FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0) {
            return std::unexpected(lastWin32Error(what, path));
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(length); // Too small: `length` includes the terminator.
    }
    std::wstring plain;
    if (std::wstring_view(buffer).starts_with(L"\\\\?\\UNC\\")) {
        plain = L"\\\\" + buffer.substr(8);
    } else if (std::wstring_view(buffer).starts_with(L"\\\\?\\")) {
        plain = buffer.substr(4);
    } else {
        plain = std::move(buffer);
    }
    auto utf8 = fromWide(plain);
    if (!utf8) {
        return std::unexpected(std::move(utf8.error()));
    }
    return normalizePath(*utf8, PathStyle::Windows);
}

/*
 * The normalised UTF-8 path of the file `widePath` finally names, all links followed
 * (GetFinalPathNameByHandleW). NotFound when the link dangles.
 */
Result<std::string> finalPathOf(const std::wstring& widePath, std::string_view path) {
    const UniqueHandle handle(CreateFileW(widePath.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!handle.valid()) {
        const DWORD error = GetLastError();
        if (errorCodeFromWin32(error) == ErrorCode::NotFound) {
            return fail(ErrorCode::NotFound,
                        std::format("safeWrite '{}': the symbolic link points at a file that does not exist", path));
        }
        return std::unexpected(win32Error(error, "safeWrite: resolve the link", path));
    }
    return finalPathOfHandle(handle.get(), "safeWrite: resolve the link", path);
}

/*
 * safeWrite for a file with one link: a flushed temp file beside it, swapped in with ReplaceFileW,
 * which carries the target's attributes, ACL, alternate streams, object id and creation time over
 * to it. Every failure leaves the target as it was and removes the temp file, except when
 * ReplaceFileW reports that it removed the target but could not rename the temp file
 * (ERROR_UNABLE_TO_MOVE_REPLACEMENT): then the temp file is moved into place, and kept if that
 * fails too, because it then holds the only copy.
 */
Result<void> replaceWithTemp(const std::string& target, const std::wstring& wideTarget, std::string_view data,
                             std::string_view path) {
    auto temp = createTempFile(parentPath(target, PathStyle::Windows), fileName(target, PathStyle::Windows),
                               "safeWrite: create temp file for", path);
    if (!temp) {
        return std::unexpected(std::move(temp.error()));
    }
    const auto abandon = [&](Error error) -> Result<void> {
        temp->handle.reset();
        DeleteFileW(temp->widePath.c_str());
        return std::unexpected(std::move(error));
    };
    if (const DWORD error = writeAll(temp->handle.get(), data); error != ERROR_SUCCESS) {
        return abandon(win32Error(error, "safeWrite: write temp file for", path));
    }
    if (FlushFileBuffers(temp->handle.get()) == FALSE) {
        return abandon(lastWin32Error("safeWrite: flush temp file for", path));
    }
    if (CloseHandle(temp->handle.release()) == FALSE) {
        return abandon(lastWin32Error("safeWrite: close temp file for", path));
    }
    constexpr DWORD kMove = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
    for (int attempt = 0;; ++attempt) {
        if (ReplaceFileW(wideTarget.c_str(), temp->widePath.c_str(), nullptr, 0, nullptr, nullptr) != FALSE) {
            return {};
        }
        const DWORD error = GetLastError();
        switch (error) {
        case ERROR_UNABLE_TO_MOVE_REPLACEMENT:
            if (MoveFileExW(temp->widePath.c_str(), wideTarget.c_str(), kMove) != FALSE) {
                return {};
            }
            return fail(ErrorCode::IoError,
                        std::format("safeWrite '{}': the old file was removed but the new content could not be "
                                    "moved into place; it is kept in '{}'",
                                    path, temp->path));
        case ERROR_INVALID_FUNCTION:
        case ERROR_NOT_SUPPORTED:
        case ERROR_CALL_NOT_IMPLEMENTED:
            // A file system without ReplaceFile support (some network redirectors): a plain rename.
            if (MoveFileExW(temp->widePath.c_str(), wideTarget.c_str(), kMove) != FALSE) {
                return {};
            }
            return abandon(lastWin32Error("safeWrite: replace", path));
        case ERROR_UNABLE_TO_REMOVE_REPLACED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_USER_MAPPED_FILE:
            if (attempt < kBusyRetries) {
                Sleep(kBusyRetryMs);
                continue;
            }
            return abandon(
                Error(ErrorCode::Busy, std::format("safeWrite '{}': the file is in use by another program and was left "
                                                   "unchanged (Win32 error {})",
                                                   path, error)));
        default: return abandon(win32Error(error, "safeWrite: replace", path));
        }
    }
}

/*
 * safeWrite for a file with several hard links: rewritten in place through one handle that denies
 * other writers. A larger content first extends the file (the space is allocated then, so a full
 * disk fails before any byte changes); then the bytes are written from the start, the end is set
 * and the file flushed.
 */
Result<void> rewriteInPlace(const std::wstring& wideTarget, std::string_view data, std::string_view path) {
    const UniqueHandle file(CreateFileW(wideTarget.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return std::unexpected(lastWin32Error("safeWrite: open the hard-linked file", path));
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file.get(), &size) == FALSE) {
        return std::unexpected(lastWin32Error("safeWrite: size of", path));
    }
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = static_cast<LONGLONG>(data.size());
    if (static_cast<std::uint64_t>(size.QuadPart) < data.size() &&
        SetFileInformationByHandle(file.get(), FileEndOfFileInfo, &end, sizeof(end)) == FALSE) {
        return std::unexpected(lastWin32Error("safeWrite: reserve space for", path));
    }
    if (const DWORD error = writeAll(file.get(), data); error != ERROR_SUCCESS) {
        return std::unexpected(win32Error(error, "safeWrite: rewrite (the file may be incomplete)", path));
    }
    if (SetFileInformationByHandle(file.get(), FileEndOfFileInfo, &end, sizeof(end)) == FALSE) {
        return std::unexpected(lastWin32Error("safeWrite: set the end of", path));
    }
    if (FlushFileBuffers(file.get()) == FALSE) {
        return std::unexpected(lastWin32Error("safeWrite: flush", path));
    }
    return {};
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
        // Pipes and character devices (reachable through explicit \\?\ paths) never end.
        if (GetFileType(file.get()) != FILE_TYPE_DISK) {
            return fail(ErrorCode::InvalidArgument, "readFile '" + std::string(path) + "': not a regular file");
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

        auto temp = createTempFile(directory, name, "writeFileAtomic: create temp file for", path);
        if (!temp) {
            return std::unexpected(std::move(temp.error()));
        }
        const std::wstring& wideTemp = temp->widePath;
        UniqueHandle& file = temp->handle;

        // From here on every failure removes the temp file.
        const auto abandon = [&](Error error) -> Result<void> {
            file.reset();
            DeleteFileW(wideTemp.c_str());
            return std::unexpected(std::move(error));
        };
        if (const DWORD error = writeAll(file.get(), data); error != ERROR_SUCCESS) {
            return abandon(win32Error(error, "writeFileAtomic: write temp file for", path));
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

    Result<SafeWriteResult> safeWrite(std::string_view path, std::string_view data,
                                      const SafeWriteOptions& options) override {
        auto normalized = normalizePath(path, PathStyle::Windows);
        if (!normalized) {
            return std::unexpected(std::move(normalized.error()));
        }
        auto wide = toWin32Path(*normalized);
        if (!wide) {
            return std::unexpected(std::move(wide.error()));
        }
        SafeWriteResult result{std::move(*normalized), SafeWriteMethod::Replaced};
        std::wstring wideTarget = std::move(*wide);
        // A symlink stays a link: the file it names is written, in its own directory.
        if (isLink(wideTarget)) {
            auto resolved = finalPathOf(wideTarget, path);
            if (!resolved) {
                return std::unexpected(std::move(resolved.error()));
            }
            auto wideResolved = toWin32Path(*resolved);
            if (!wideResolved) {
                return std::unexpected(std::move(wideResolved.error()));
            }
            result.target = std::move(*resolved);
            wideTarget = std::move(*wideResolved);
        }
        UniqueHandle probe(CreateFileW(wideTarget.c_str(), FILE_READ_ATTRIBUTES, kShareAll, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!probe.valid()) {
            const DWORD error = GetLastError();
            if (errorCodeFromWin32(error) != ErrorCode::NotFound) {
                return std::unexpected(win32Error(error, "safeWrite", path));
            }
            // A new file (a missing parent directory fails in writeFileAtomic with NotFound).
            if (auto written = writeFileAtomic(result.target, data); !written) {
                return std::unexpected(std::move(written.error()));
            }
            result.method = SafeWriteMethod::Created;
            return result;
        }
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileInformationByHandle(probe.get(), &info) == FALSE) {
            return std::unexpected(lastWin32Error("safeWrite", path));
        }
        probe.reset();
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return std::unexpected(detail::isADirectoryError("safeWrite", path));
        }
        const DWORD attributes = info.dwFileAttributes & kSettableAttributes;
        const bool readOnly = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
        if (readOnly) {
            if (!options.overwriteReadOnly) {
                return fail(ErrorCode::PermissionDenied,
                            std::format("safeWrite '{}': the file is read-only and was left unchanged", path));
            }
            const DWORD writable = attributes & ~DWORD{FILE_ATTRIBUTE_READONLY};
            if (SetFileAttributesW(wideTarget.c_str(), writable != 0 ? writable : FILE_ATTRIBUTE_NORMAL) == FALSE) {
                return std::unexpected(lastWin32Error("safeWrite: clear the read-only attribute of", path));
            }
        }
        const bool linked = info.nNumberOfLinks > 1;
        auto written =
            linked ? rewriteInPlace(wideTarget, data, path) : replaceWithTemp(result.target, wideTarget, data, path);
        if (!written) {
            if (readOnly) {
                SetFileAttributesW(wideTarget.c_str(), attributes); // Best effort: as it was.
            }
            return std::unexpected(std::move(written.error()));
        }
        result.method = linked ? SafeWriteMethod::InPlace : SafeWriteMethod::Replaced;
        return result;
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

    Result<std::vector<DirEntry>> listDirectory(std::string_view path) override { return list(path, nullptr); }

    Result<DirListing> listDirectoryWithDiagnostics(std::string_view path) override {
        DirListing listing;
        auto entries = list(path, &listing.issues);
        if (!entries) {
            return std::unexpected(std::move(entries.error()));
        }
        listing.entries = std::move(*entries);
        std::ranges::sort(listing.issues, {}, &DirListingIssue::name);
        return listing;
    }

    // The body of both listings; names that are not valid UTF-16 go to `issues` when given.
    Result<std::vector<DirEntry>> list(std::string_view path, std::vector<DirListingIssue>* issues) {
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
                // Not representable as UTF-8 (lone surrogate); see file_system.hpp.
                if (issues != nullptr) {
                    std::string lossy = lossyUtf8(name);
                    std::string message = std::format(
                        "listDirectory '{}': the name '{}' is not valid UTF-16 and was skipped", path, lossy);
                    issues->push_back(DirListingIssue{std::move(lossy), false,
                                                      Error(ErrorCode::InvalidArgument, std::move(message))});
                }
                continue;
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
            // ParentMissing here means the parent just made (or found) is gone again; the error names it.
            const std::string_view child = missing.back();
            Error error(ErrorCode::NotFound, "createDirectories '" + std::string(path) + "': '" +
                                                 std::string(parentPath(child, PathStyle::Windows)) +
                                                 "' vanished before '" + std::string(child) + "' was created");
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
            if (error == ERROR_ACCESS_DENIED && isLink(*wide)) {
                // A directory symlink or junction: RemoveDirectoryW removes the link itself,
                // never the target or its contents, which is what POSIX unlink does.
                if (RemoveDirectoryW(wide->c_str()) == FALSE) {
                    return std::unexpected(lastWin32Error("removeFile", path));
                }
                return {};
            }
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
        // A link is removed with removeFile, as on POSIX (where rmdir on a symlink fails).
        if (isLink(*wide)) {
            return std::unexpected(detail::notADirectoryError("removeDirectory", path));
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

    /*
     * One spelling per file (ADR 0042, ADR 0044): the real case of every component. The fast path
     * is GetFinalPathNameByHandleW, used when its answer differs from the normalised input in ASCII
     * case alone. Otherwise the path goes through a link, a junction, a subst drive or an 8.3 name,
     * which the final path would resolve: each component then takes its own real (long) name from
     * FindFirstFileExW, so the document stays where the user opened it.
     */
    Result<std::string> canonicalPath(std::string_view path) override {
        auto normalized = normalizePath(path, PathStyle::Windows);
        if (!normalized) {
            return normalized;
        }
        auto wide = toWin32Path(*normalized);
        if (!wide) {
            return std::unexpected(std::move(wide.error()));
        }
        // The name needs no access rights; FILE_FLAG_OPEN_REPARSE_POINT keeps a link as the link
        // (its own real-case name), FILE_FLAG_BACKUP_SEMANTICS opens directories.
        const UniqueHandle handle(CreateFileW(wide->c_str(), 0, kShareAll, nullptr, OPEN_EXISTING,
                                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!handle.valid()) {
            return std::unexpected(lastWin32Error("canonicalPath", path));
        }
        auto final = finalPathOfHandle(handle.get(), "canonicalPath", path);
        if (!final) {
            return final;
        }
        if (final->size() == normalized->size() && equalIgnoringAsciiCase(*final, *normalized)) {
            return final;
        }
        return realCaseOfComponents(*normalized, path);
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
        // Volume roots answer ERROR_ACCESS_DENIED rather than ERROR_ALREADY_EXISTS. An existing
        // entry counts as a directory only if links lead to one, so a dangling junction fails
        // here, where it sits, as a dangling symlink does on POSIX.
        if (attributesOf(*wide) != INVALID_FILE_ATTRIBUTES) {
            if (reachesDirectory(*wide)) {
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
