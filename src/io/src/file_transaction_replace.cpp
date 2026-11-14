// SPDX-License-Identifier: Apache-2.0
// Limited native replacement/inverse participant with retained objects and honest partial effects.
#include "frost/io/file_transaction_replace.hpp"

#include "frost/io/os_error.hpp"
#include "frost/io/path.hpp"
#include "frost/io/spawn_log.hpp"

#include "file_transaction_replace_testing.hpp"
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <limits>
#include <utility>
#include <vector>

#ifdef _WIN32
#include "save_metadata_win.hpp"

#include <aclapi.h>
#include <sddl.h>
#else
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/vfs.h>
#include <sys/xattr.h>

#include <cerrno>

#include <fcntl.h>
#endif

namespace frost::io {
namespace detail {
namespace {
thread_local ReplacementObserver replacementObserver = nullptr;
thread_local void* replacementObserverContext = nullptr;
} // namespace
void setReplacementObserver(ReplacementObserver observer, void* context) noexcept {
    replacementObserver = observer;
    replacementObserverContext = context;
}
void observeReplacement(ReplacementBoundary boundary, const FileReplacementReceipt& receipt) {
    if (replacementObserver)
        replacementObserver(replacementObserverContext, boundary, receipt);
}
} // namespace detail
namespace {
#ifdef _WIN32
using Native = detail::UniqueHandle;
#else
using Native = detail::UniqueFd;
#endif

struct Metadata {
    std::string object;
    std::string volume;
#ifdef _WIN32
    FILE_BASIC_INFO basic{};
    std::vector<unsigned char> security;
#else
    mode_t mode = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    timespec modified{};
    int flags = 0;
#endif
};

Result<Native> openNative(std::string_view path, bool writable = false, bool directory = false,
                          bool followDirectory = false) {
#ifdef _WIN32
    auto wide = toWin32Path(path);
    if (!wide)
        return std::unexpected(wide.error());
    DWORD access = GENERIC_READ | READ_CONTROL;
    if (!directory)
        access |= DELETE;
    if (writable)
        access |= GENERIC_WRITE | WRITE_DAC | FILE_WRITE_ATTRIBUTES;
    Native file(CreateFileW(
        wide->c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | (directory && followDirectory ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), nullptr));
    if (!file.valid())
        return std::unexpected(lastWin32Error("replacement: open native object", path));
#else
    int flags =
        (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NONBLOCK | (directory && followDirectory ? 0 : O_NOFOLLOW);
    if (directory)
        flags |= O_DIRECTORY;
    Native file(::open(std::string(path).c_str(), flags));
    if (!file.valid())
        return std::unexpected(errnoError(errno, "replacement: open native object", path));
#endif
    return file;
}

#ifdef _WIN32
Result<std::vector<unsigned char>> securityOf(HANDLE handle, std::string_view path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD error = GetSecurityInfo(
        handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &descriptor);
    if (error != ERROR_SUCCESS)
        return std::unexpected(win32Error(error, "replacement: inspect owner/group/DACL (not SACL)", path));
    const auto release = [](void* pointer) {
        if (pointer)
            LocalFree(pointer);
    };
    std::unique_ptr<void, decltype(release)> owner(descriptor, release);
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(descriptor, &control, &revision))
        return std::unexpected(lastWin32Error("replacement: inspect security descriptor form", path));
    if ((control & SE_SELF_RELATIVE) != 0) {
        const auto length = GetSecurityDescriptorLength(descriptor);
        if (length == 0 || length > 65536)
            return fail(ErrorCode::OutOfRange, "replacement: supported security descriptor exceeds bound");
        const auto* start = static_cast<const unsigned char*>(descriptor);
        return std::vector<unsigned char>(start, start + length);
    }
    DWORD size = 0;
    MakeSelfRelativeSD(descriptor, nullptr, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0 || size > 65536)
        return std::unexpected(lastWin32Error("replacement: size supported security descriptor", path));
    std::vector<unsigned char> result(size);
    if (!MakeSelfRelativeSD(descriptor, result.data(), &size))
        return std::unexpected(lastWin32Error("replacement: capture supported security descriptor", path));
    result.resize(size);
    return result;
}

Result<void> privateStore(HANDLE handle, std::string_view path, bool requireSafeChildInheritance = false) {
    auto security = securityOf(handle, path);
    if (!security)
        return std::unexpected(security.error());
    HANDLE tokenRaw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenRaw))
        return std::unexpected(lastWin32Error("replacement: inspect current owner token", path));
    Native token(tokenRaw);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size > 65536 || size == 0)
        return std::unexpected(lastWin32Error("replacement: size current owner token", path));
    std::vector<unsigned char> data(size);
    if (!GetTokenInformation(token.get(), TokenUser, data.data(), size, &size))
        return std::unexpected(lastWin32Error("replacement: current owner token", path));
    auto* user = reinterpret_cast<TOKEN_USER*>(data.data());
    PSID owner = nullptr;
    BOOL defaulted = FALSE;
    if (!GetSecurityDescriptorOwner(security->data(), &owner, &defaulted) || !owner || !EqualSid(owner, user->User.Sid))
        return fail(ErrorCode::Unsupported, "replacement: private store must belong to current token user");
    PACL acl = nullptr;
    BOOL present = FALSE;
    if (!GetSecurityDescriptorDacl(security->data(), &present, &acl, &defaulted) || !present || !acl)
        return fail(ErrorCode::Unsupported, "replacement: private store requires an actual restricted DACL");
    bool ownerCanInheritFile = false;
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* entry = nullptr;
        if (!GetAce(acl, index, &entry))
            return std::unexpected(lastWin32Error("replacement: inspect private-store ACE", path));
        const auto* header = static_cast<ACE_HEADER*>(entry);
        if (header->AceType == ACCESS_DENIED_ACE_TYPE)
            continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            return fail(ErrorCode::Unsupported, "replacement: private store has unsupported ACE kind");
        auto* sid = reinterpret_cast<PSID>(&static_cast<ACCESS_ALLOWED_ACE*>(entry)->SidStart);
        const auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(entry);
        if (EqualSid(sid, user->User.Sid) && (header->AceFlags & OBJECT_INHERIT_ACE) != 0 &&
            (allowed->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS)
            ownerCanInheritFile = true;
        if (!EqualSid(sid, user->User.Sid) && !IsWellKnownSid(sid, WinLocalSystemSid) &&
            !IsWellKnownSid(sid, WinBuiltinAdministratorsSid))
            return fail(ErrorCode::Unsupported, "replacement: private store allows non-owner/non-system readers");
    }
    if (requireSafeChildInheritance && !ownerCanInheritFile)
        return fail(ErrorCode::Unsupported,
                    "replacement: private store requires restrictive owner-file inheritance before byte staging");
    return {};
}

bool sameSecurityOwners(const Metadata& a, const Metadata& b) {
    PSID aOwner = nullptr, aGroup = nullptr, bOwner = nullptr, bGroup = nullptr;
    BOOL defaulted = FALSE;
    return GetSecurityDescriptorOwner(const_cast<unsigned char*>(a.security.data()), &aOwner, &defaulted) &&
           GetSecurityDescriptorOwner(const_cast<unsigned char*>(b.security.data()), &bOwner, &defaulted) &&
           GetSecurityDescriptorGroup(const_cast<unsigned char*>(a.security.data()), &aGroup, &defaulted) &&
           GetSecurityDescriptorGroup(const_cast<unsigned char*>(b.security.data()), &bGroup, &defaulted) && aOwner &&
           bOwner && aGroup && bGroup && EqualSid(aOwner, bOwner) && EqualSid(aGroup, bGroup);
}

Result<void> ordinaryStreams(HANDLE file, std::string_view path) {
    // BackupRead without PROCESS_SECURITY does not inspect SACL: that remains explicitly unproven.
    // Every actually reported EA/ADS/other stream is refused, never treated as absent on error.
    void* context = nullptr;
    const auto abort = [&](void*) {
        DWORD ignored = 0;
        BackupRead(file, nullptr, 0, &ignored, TRUE, FALSE, &context);
    };
    std::unique_ptr<void, decltype(abort)> cleanup(reinterpret_cast<void*>(1), abort);
    for (;;) {
        WIN32_STREAM_ID stream{};
        DWORD count = 0;
        constexpr DWORD headerSize = static_cast<DWORD>(offsetof(WIN32_STREAM_ID, cStreamName));
        if (!BackupRead(file, reinterpret_cast<BYTE*>(&stream), headerSize, &count, FALSE, FALSE, &context))
            return std::unexpected(lastWin32Error("replacement: inspect actual backup stream", path));
        if (count == 0)
            return {};
        if (count != headerSize)
            return fail(ErrorCode::IoError, "replacement: truncated native stream header");
        if (stream.dwStreamId != BACKUP_DATA || stream.dwStreamNameSize != 0 || stream.Size.QuadPart < 0)
            return fail(ErrorCode::Unsupported,
                        "replacement: observed EA/ADS/special backup stream is outside first profile");
        DWORD low = 0, high = 0;
        if (!BackupSeek(file, stream.Size.LowPart, static_cast<DWORD>(stream.Size.HighPart), &low, &high, &context) ||
            low != stream.Size.LowPart || high != static_cast<DWORD>(stream.Size.HighPart))
            return std::unexpected(lastWin32Error("replacement: seek inspected default stream", path));
    }
}
#endif

Result<Metadata> metadataOf(const Native& file, std::string_view path, bool privateObject = false) {
    Metadata result;
#ifdef _WIN32
    FILE_ID_INFO id{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(file.get(), FileIdInfo, &id, sizeof(id)) ||
        !GetFileInformationByHandleEx(file.get(), FileStandardInfo, &standard, sizeof(standard)) ||
        !GetFileInformationByHandleEx(file.get(), FileBasicInfo, &result.basic, sizeof(result.basic)))
        return std::unexpected(lastWin32Error("replacement: inspect actual object", path));
    if (standard.Directory || standard.NumberOfLinks != 1 || standard.DeletePending)
        return fail(ErrorCode::Unsupported, "replacement: only live regular single-link files admitted");
    constexpr DWORD allowed = FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    if ((result.basic.FileAttributes & ~allowed) != 0)
        return fail(ErrorCode::Unsupported,
                    "replacement: observed readonly/reparse/EFS/sparse/compressed/special attributes");
    auto wide = toWin32Path(path);
    if (!wide)
        return std::unexpected(wide.error());
    save_metadata::Metadata special;
    const auto specialError = save_metadata::queryMetadata(file.get(), *wide, special);
    if (specialError != ERROR_SUCCESS)
        return std::unexpected(win32Error(specialError, "replacement: inspect object-ID/shortname", path));
    if (special.hasObjectId || (!privateObject && !special.shortName.empty()))
        return fail(ErrorCode::Unsupported, "replacement: observed object-ID/shortname outside first profile");
    std::array<wchar_t, 32> fs{};
    if (!GetVolumeInformationByHandleW(file.get(), nullptr, 0, nullptr, nullptr, nullptr, fs.data(),
                                       static_cast<DWORD>(fs.size())))
        return std::unexpected(lastWin32Error("replacement: inspect volume", path));
    std::array<wchar_t, MAX_PATH> root{};
    if (!GetVolumePathNameW(wide->c_str(), root.data(), static_cast<DWORD>(root.size())))
        return std::unexpected(lastWin32Error("replacement: inspect local volume root", path));
    if (GetDriveTypeW(root.data()) != DRIVE_FIXED || std::wstring_view(fs.data()) != L"NTFS")
        return fail(ErrorCode::Unsupported, "replacement: first Windows profile requires NTFS");
    auto streams = ordinaryStreams(file.get(), path);
    if (!streams)
        return std::unexpected(streams.error());
    auto security = securityOf(file.get(), path);
    if (!security)
        return std::unexpected(security.error());
    result.security = std::move(*security);
    result.volume = std::to_string(id.VolumeSerialNumber);
    result.object.assign(reinterpret_cast<const char*>(id.FileId.Identifier), sizeof(id.FileId.Identifier));
#else
    static_cast<void>(privateObject);
    struct stat info{};
    if (::fstat(file.get(), &info) != 0)
        return std::unexpected(errnoError(errno, "replacement: inspect actual object", path));
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1)
        return fail(ErrorCode::Unsupported, "replacement: only regular single-link files admitted");
    struct statfs fs{};
    if (::fstatfs(file.get(), &fs) != 0)
        return std::unexpected(errnoError(errno, "replacement: inspect filesystem", path));
    if (static_cast<unsigned long>(fs.f_type) != 0xEF53UL)
        return fail(ErrorCode::Unsupported, "replacement: first POSIX profile requires ext4");
    const auto xattrs = ::flistxattr(file.get(), nullptr, 0);
    if (xattrs < 0)
        return std::unexpected(errnoError(errno, "replacement: inspect complete xattr-name set", path));
    if (xattrs != 0)
        return fail(ErrorCode::Unsupported,
                    "replacement: observed xattrs/ACL/security attributes outside first profile");
    if (::ioctl(file.get(), FS_IOC_GETFLAGS, &result.flags) != 0)
        return std::unexpected(errnoError(errno, "replacement: inspect inode flags", path));
    if ((result.flags & ~FS_EXTENT_FL) != 0 || (info.st_mode & (S_ISUID | S_ISGID | S_ISVTX)) != 0)
        return fail(ErrorCode::Unsupported, "replacement: observed special inode flags or mode outside first profile");
    result.volume = std::to_string(info.st_dev);
    result.object = std::to_string(info.st_ino);
    result.mode = info.st_mode & 0777;
    result.uid = info.st_uid;
    result.gid = info.st_gid;
    result.modified = info.st_mtim;
#endif
    return result;
}

bool sameObject(const Metadata& a, const Metadata& b) { return a.volume == b.volume && a.object == b.object; }

bool sameSupportedMetadata(const Metadata& a, const Metadata& b, bool includeSecurity = true) {
#ifdef _WIN32
    return a.basic.FileAttributes == b.basic.FileAttributes &&
           a.basic.CreationTime.QuadPart == b.basic.CreationTime.QuadPart &&
           a.basic.LastWriteTime.QuadPart == b.basic.LastWriteTime.QuadPart &&
           (!includeSecurity || a.security == b.security);
#else
    static_cast<void>(includeSecurity);
    return a.mode == b.mode && a.uid == b.uid && a.gid == b.gid && a.flags == b.flags &&
           a.modified.tv_sec == b.modified.tv_sec && a.modified.tv_nsec == b.modified.tv_nsec;
#endif
}

Result<void> absentEndpoint(std::string_view path) {
    auto file = openNative(path);
    if (file)
        return fail(ErrorCode::AlreadyExists, "transaction private/namespace endpoint became occupied");
    if (file.error().code() != ErrorCode::NotFound)
        return std::unexpected(file.error());
    return {};
}
Result<bool> actualLeafSpelling(const Native& file, std::string_view path) {
#ifdef _WIN32
    std::vector<std::byte> storage(sizeof(FILE_NAME_INFO) + 65536);
    if (!GetFileInformationByHandleEx(file.get(), FileNameInfo, storage.data(), static_cast<DWORD>(storage.size())))
        return std::unexpected(lastWin32Error("recovery: inspect exact native leaf spelling", path));
    const auto* info = reinterpret_cast<const FILE_NAME_INFO*>(storage.data());
    if (info->FileNameLength > 65536 || info->FileNameLength % sizeof(wchar_t) != 0)
        return fail(ErrorCode::Corrupt, "recovery: invalid native file-name observation");
    const std::wstring_view actual(info->FileName, info->FileNameLength / sizeof(wchar_t));
    auto declared = toWin32Path(path);
    if (!declared)
        return std::unexpected(declared.error());
    const auto a = actual.substr(actual.find_last_of(L"\\/") + 1);
    const std::wstring_view expected(*declared);
    return a == expected.substr(expected.find_last_of(L"\\/") + 1);
#else
    static_cast<void>(file);
    static_cast<void>(path);
    return true;
#endif
}
Result<void> emptyMoveEndpoint(std::string_view destination, std::string_view source, const Metadata& expected) {
    auto occupied = openNative(destination);
    if (!occupied) {
        if (occupied.error().code() == ErrorCode::NotFound)
            return {};
        return std::unexpected(occupied.error());
    }
    auto actual = metadataOf(*occupied, destination);
    if (!actual)
        return std::unexpected(actual.error());
    bool caseOnly = false;
#ifdef _WIN32
    auto from = toWin32Path(source);
    auto to = toWin32Path(destination);
    caseOnly = from && to &&
               CompareStringOrdinal(from->data(), static_cast<int>(from->size()), to->data(),
                                    static_cast<int>(to->size()), TRUE) == CSTR_EQUAL;
#else
    static_cast<void>(source);
#endif
    if (!caseOnly || !sameObject(*actual, expected))
        return fail(ErrorCode::AlreadyExists, "transaction movement endpoint is occupied");
    return {};
}

Result<std::string> readNative(const Native& file, std::size_t limit, std::string_view path) {
    std::string result;
    std::array<char, 65536> chunk{};
#ifdef _WIN32
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(file.get(), zero, nullptr, FILE_BEGIN))
        return std::unexpected(lastWin32Error("replacement: seek actual bytes", path));
#endif
    for (;;) {
        const auto want = std::min(chunk.size(), limit - result.size());
#ifdef _WIN32
        DWORD count = 0;
        if (!ReadFile(file.get(), chunk.data(), static_cast<DWORD>(want == 0 ? 1 : want), &count, nullptr))
            return std::unexpected(lastWin32Error("replacement: read actual bytes", path));
#else
        const auto count = ::pread(file.get(), chunk.data(), want == 0 ? 1 : want, static_cast<off_t>(result.size()));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return std::unexpected(errnoError(errno, "replacement: read actual bytes", path));
        }
#endif
        if (count == 0)
            return result;
        if (static_cast<std::size_t>(count) > limit - result.size())
            return fail(ErrorCode::OutOfRange, "replacement: actual bytes grew beyond captured limit");
        result.append(chunk.data(), static_cast<std::size_t>(count));
    }
}

Result<void> supportedMetadataTo(const Native& file, const Metadata& metadata, std::string_view path) {
#ifdef _WIN32
    FILE_BASIC_INFO basic{};
    basic.CreationTime = metadata.basic.CreationTime;
    basic.LastWriteTime = metadata.basic.LastWriteTime;
    basic.FileAttributes = metadata.basic.FileAttributes;
    if (!SetFileInformationByHandle(file.get(), FileBasicInfo, &basic, sizeof(basic)))
        return std::unexpected(lastWin32Error("replacement: stage supported basic metadata", path));
#else
    if (::fchown(file.get(), metadata.uid, metadata.gid) != 0 || ::fchmod(file.get(), metadata.mode) != 0)
        return std::unexpected(errnoError(errno, "replacement: stage exact owner/group/mode", path));
    timespec times[2]{};
    times[0].tv_nsec = UTIME_OMIT;
    times[1] = metadata.modified;
    if (::futimens(file.get(), times) != 0)
        return std::unexpected(errnoError(errno, "replacement: stage supported modification time", path));
#endif
    return {};
}

Result<void> flushNative(const Native& file, std::string_view path) {
#ifdef _WIN32
    if (!FlushFileBuffers(file.get()))
        return std::unexpected(lastWin32Error("replacement: flush private image", path));
#else
    if (::fsync(file.get()) != 0)
        return std::unexpected(errnoError(errno, "replacement: flush private image", path));
#endif
    return {};
}

Result<void> renameNoReplace(const Native& file, const Native& fromParent, std::string_view from,
                             const Native& toParent, std::string_view to) {
#ifdef _WIN32
    static_cast<void>(fromParent);
    static_cast<void>(from);
    static_cast<void>(toParent);
    auto wide = toWin32Path(to);
    if (!wide)
        return std::unexpected(wide.error());
    const auto bytes = wide->size() * sizeof(wchar_t);
    if (bytes > std::numeric_limits<DWORD>::max() - offsetof(FILE_RENAME_INFO, FileName) - sizeof(wchar_t))
        return fail(ErrorCode::OutOfRange, "replacement: native endpoint length");
    std::vector<std::byte> storage(offsetof(FILE_RENAME_INFO, FileName) + bytes + sizeof(wchar_t));
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->Flags = 0; // No replace, no silent overwrite of a foreign endpoint.
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(bytes);
    std::memcpy(rename->FileName, wide->data(), bytes);
    if (!SetFileInformationByHandle(file.get(), FileRenameInfoEx, rename, static_cast<DWORD>(storage.size())))
        return std::unexpected(lastWin32Error("replacement: no-replace handle publication", to));
#else
    static_cast<void>(file);
    const std::string fromLeaf(fileName(from)), toLeaf(fileName(to));
    if (::syscall(SYS_renameat2, fromParent.get(), fromLeaf.c_str(), toParent.get(), toLeaf.c_str(),
                  RENAME_NOREPLACE) != 0)
        return std::unexpected(errnoError(errno, "replacement: no-replace dirfd publication", to));
#endif
    return {};
}

Result<void> directoryAt(std::string_view path, const Native& expected, bool restrictedStore = false) {
    auto current = openNative(path, false, true);
    if (!current)
        return std::unexpected(current.error());
#ifdef _WIN32
    FILE_ID_INFO a{}, b{};
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(expected.get(), FileIdInfo, &a, sizeof(a)) ||
        !GetFileInformationByHandleEx(current->get(), FileIdInfo, &b, sizeof(b)) ||
        !GetFileInformationByHandleEx(current->get(), FileAttributeTagInfo, &tag, sizeof(tag)))
        return std::unexpected(lastWin32Error("replacement: inspect retained parent", path));
    if (a.VolumeSerialNumber != b.VolumeSerialNumber ||
        std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) != 0 ||
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return fail(ErrorCode::Busy, "replacement: parent changed or became reparse");
    if (restrictedStore)
        return privateStore(current->get(), path, true);
#else
    struct stat a{}, b{};
    if (::fstat(expected.get(), &a) != 0 || ::fstat(current->get(), &b) != 0)
        return std::unexpected(errnoError(errno, "replacement: inspect retained parent", path));
    if (a.st_dev != b.st_dev || a.st_ino != b.st_ino)
        return fail(ErrorCode::Busy, "replacement: parent changed");
    if (restrictedStore && (b.st_uid != ::geteuid() || (b.st_mode & 0777) != 0700))
        return fail(ErrorCode::Busy, "replacement: private-store ownership/mode changed");
#endif
    return {};
}

Result<FileImageObservation> directoryIdentity(const Native& file, std::string_view path) {
    FileImageObservation out;
#ifdef _WIN32
    FILE_ID_INFO id{};
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(file.get(), FileIdInfo, &id, sizeof(id)) ||
        !GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &tag, sizeof(tag)))
        return std::unexpected(lastWin32Error("create: directory identity", path));
    if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return fail(ErrorCode::Unsupported, "create: actual non-reparse directory required");
    out.volume = std::to_string(id.VolumeSerialNumber);
    out.object.assign(reinterpret_cast<const char*>(id.FileId.Identifier), sizeof(id.FileId.Identifier));
#else
    struct stat id{};
    if (::fstat(file.get(), &id) != 0)
        return std::unexpected(errnoError(errno, "create: directory identity", path));
    if (!S_ISDIR(id.st_mode))
        return fail(ErrorCode::Unsupported, "create: actual directory required");
    out.volume = std::to_string(id.st_dev);
    out.object = std::to_string(id.st_ino);
#endif
    return out;
}

std::string metadataDigest(const Metadata& metadata) {
#ifdef _WIN32
    const auto values = std::format("{}:{}:{}:", metadata.basic.FileAttributes, metadata.basic.CreationTime.QuadPart,
                                    metadata.basic.LastWriteTime.QuadPart);
    return sha256Hex(values +
                     std::string(reinterpret_cast<const char*>(metadata.security.data()), metadata.security.size()));
#else
    return sha256Hex(std::format("{}:{}:{}:{}:{}:{}", metadata.mode, metadata.uid, metadata.gid,
                                 metadata.modified.tv_sec, metadata.modified.tv_nsec, metadata.flags));
#endif
}

Result<void> objectAt(std::string_view path, const Metadata& expected, std::string_view bytes,
                      bool includeSecurity = true, bool privateObject = false) {
    auto file = openNative(path);
    if (!file)
        return std::unexpected(file.error());
    auto actual = metadataOf(*file, path, privateObject);
    if (!actual)
        return std::unexpected(actual.error());
    if (!sameObject(*actual, expected) || !sameSupportedMetadata(*actual, expected, includeSecurity))
        return fail(ErrorCode::Busy,
                    "replacement: object or supported metadata changed at '" + std::string(path) + "'");
    auto current = readNative(*file, bytes.size(), path);
    if (!current)
        return std::unexpected(current.error());
    if (*current != bytes)
        return fail(ErrorCode::Busy, "replacement: bytes changed at '" + std::string(path) + "'");
    return {};
}
} // namespace

struct PreparedFileReplacement::State {
    State() = default;
    explicit State(PreparedFileImages value)
        : images(std::move(value)), beforePayload(images->beforeBytes()), afterPayload(images->afterBytes()) {}
    std::optional<PreparedFileImages> images;
    std::string beforePayload, afterPayload;
    FileReplacementReceipt receipt;
    Metadata before;
    Metadata after;
    Native original;
    Native replacement;
    Native parent;
    Native store;
    std::string parentPath;
    std::string storePath;
    Native moveParent;
    std::string moveParentPath;
};

PreparedFileReplacement::PreparedFileReplacement(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {}
PreparedFileReplacement::~PreparedFileReplacement() = default;
PreparedFileReplacement::PreparedFileReplacement(PreparedFileReplacement&&) noexcept = default;
PreparedFileReplacement& PreparedFileReplacement::operator=(PreparedFileReplacement&&) noexcept = default;
const FileReplacementReceipt* PreparedFileReplacement::receipt() const noexcept {
    return m_state ? &m_state->receipt : nullptr;
}

Result<PreparedFileReplacement> prepareFileCreation(std::string_view target, std::string_view privateStorePath,
                                                    std::string_view after, std::size_t maxAfterBytes) {
    if (after.size() > maxAfterBytes)
        return fail(ErrorCode::OutOfRange, "create: image exceeds explicit limit");
    auto normalized = normalizePath(target);
    auto normalizedStore = normalizePath(privateStorePath);
    if (!normalized || !normalizedStore || !isAbsolutePath(*normalized) || !isAbsolutePath(*normalizedStore) ||
        fileName(*normalized).empty())
        return fail(ErrorCode::InvalidArgument, "create: absolute file/store paths required");
#ifdef _WIN32
    if (fileName(*normalized).find(':') != std::string_view::npos)
        return fail(ErrorCode::Unsupported, "create: alternate data stream destinations are unsupported");
#endif
    auto state = std::make_unique<PreparedFileReplacement::State>();
    state->parentPath = std::string(parentPath(*normalized));
    state->storePath = *normalizedStore;
    auto parent = openNative(state->parentPath, false, true);
    auto store = openNative(state->storePath, false, true);
    if (!parent)
        return std::unexpected(parent.error());
    if (!store)
        return std::unexpected(store.error());
    state->parent = std::move(*parent);
    state->store = std::move(*store);
    if (auto checked = directoryAt(state->storePath, state->store, true); !checked)
        return std::unexpected(checked.error());
    auto parentId = directoryIdentity(state->parent, state->parentPath);
    auto storeId = directoryIdentity(state->store, state->storePath);
    if (!parentId)
        return std::unexpected(parentId.error());
    if (!storeId)
        return std::unexpected(storeId.error());
    if (parentId->volume != storeId->volume ||
        (parentId->object == storeId->object && parentId->volume == storeId->volume))
        return fail(ErrorCode::Unsupported, "create: separate private same-volume store required");
    if (auto absent = absentEndpoint(*normalized); !absent)
        return std::unexpected(absent.error());
    state->afterPayload = after;
    auto& receipt = state->receipt;
    receipt.images.targetPath = *normalized;
    receipt.images.targetParent = *parentId;
    receipt.images.store = *storeId;
    receipt.images.afterSha256 = sha256Hex(after);
    // No Before object/hash: this represents absence, not an empty-file replacement.
    auto leaf = std::format(".frost-create-{}-{}", detail::currentProcessId(), detail::nextUniqueId());
    auto stage = joinPath(state->storePath, leaf);
    if (!stage)
        return std::unexpected(stage.error());
    receipt.replacementPath = *stage;
    receipt.images.stagePath = *stage;
    receipt.retainedBeforePath = *stage + ".before"; // Reserved absent endpoint, never fabricated.
    const auto retained = [&](Error error) -> std::unexpected<Error> {
        return std::unexpected(error.withContext("create: private image retained at '" + *stage + "'"));
    };
#ifdef _WIN32
    auto wide = toWin32Path(*stage);
    if (!wide)
        return std::unexpected(wide.error());
    // Store admission proves restrictive inheritable child ACL BEFORE the first byte is written.
    state->replacement.reset(CreateFileW(wide->c_str(), GENERIC_READ | GENERIC_WRITE | DELETE | READ_CONTROL,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!state->replacement.valid())
        return std::unexpected(lastWin32Error("create: exclusive stage", *stage));
    if (auto restricted = privateStore(state->replacement.get(), *stage); !restricted)
        return retained(restricted.error());
#else
    state->replacement.reset(
        ::openat(state->store.get(), leaf.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!state->replacement.valid())
        return std::unexpected(errnoError(errno, "create: exclusive stage", *stage));
#endif
    if (auto checked = directoryAt(state->storePath, state->store, true); !checked)
        return retained(checked.error());
    std::size_t done = 0;
    while (done < after.size()) {
        const auto want = std::min<std::size_t>(after.size() - done, 1024 * 1024);
#ifdef _WIN32
        DWORD count = 0;
        if (!WriteFile(state->replacement.get(), after.data() + done, static_cast<DWORD>(want), &count, nullptr))
            return retained(lastWin32Error("create: write private image", *stage));
#else
        const auto count = ::write(state->replacement.get(), after.data() + done, want);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return retained(errnoError(errno, "create: write private image", *stage));
        }
#endif
        if (count == 0)
            return retained(Error(ErrorCode::IoError, "create: zero-progress write"));
        done += static_cast<std::size_t>(count);
    }
    if (auto flushed = flushNative(state->replacement, *stage); !flushed)
        return retained(flushed.error());
    auto meta = metadataOf(state->replacement, *stage, true);
    if (!meta)
        return retained(meta.error());
    if (meta->volume != parentId->volume)
        return retained(Error(ErrorCode::Busy, "create: stage volume changed during preparation"));
    state->after = std::move(*meta);
    receipt.images.stage.volume = state->after.volume;
    receipt.images.stage.object = state->after.object;
    receipt.images.stage.size = after.size();
    receipt.privateAfterMetadataSha256 = metadataDigest(state->after);
    receipt.images.stage.metadata = receipt.privateAfterMetadataSha256;
    receipt.metadataProfile = "CREATE LIMITED: absent Before; restricted new regular single-link file, same native "
                              "observable metadata profile as replacement; no prior metadata/SACL preservation claim";
    receipt.images.metadataObservationProfile = receipt.metadataProfile;
    detail::observeReplacement(detail::ReplacementBoundary::ByteImagesPrepared, receipt);
    if (auto checked = objectAt(*stage, state->after, after, true, true); !checked)
        return retained(checked.error());
    if (auto checked = directoryAt(state->parentPath, state->parent); !checked)
        return retained(checked.error());
    if (auto checked = directoryAt(state->storePath, state->store, true); !checked)
        return retained(checked.error());
    if (auto absent = absentEndpoint(*normalized); !absent)
        return retained(absent.error());
    return PreparedFileReplacement(std::move(state));
}

Result<PreparedFileReplacement> prepareFileReplacement(std::string_view target, std::string_view privateStorePath,
                                                       std::string_view after, std::size_t maxBeforeBytes,
                                                       std::size_t maxAfterBytes) {
    if (after.size() > maxAfterBytes)
        return fail(ErrorCode::OutOfRange, "replacement: After exceeds limit before native preparation");
    auto original = openNative(target);
    if (!original)
        return std::unexpected(original.error());
    auto before = metadataOf(*original, target);
    if (!before)
        return std::unexpected(before.error());
    auto store = openNative(privateStorePath, false, true);
    if (!store)
        return std::unexpected(store.error());
#ifdef _WIN32
    if (auto restricted = privateStore(store->get(), privateStorePath, true); !restricted)
        return std::unexpected(restricted.error());
#endif
    FileReplacementReceipt capture;
    capture.images.targetPath = std::string(target);
    capture.beforeMetadataSha256 = metadataDigest(*before);
    detail::observeReplacement(detail::ReplacementBoundary::MetadataCaptured, capture);
    auto images = prepareFileImages(target, privateStorePath, after, maxBeforeBytes, maxAfterBytes);
    if (!images)
        return std::unexpected(images.error());
    auto state = std::make_unique<PreparedFileReplacement::State>(std::move(*images));
    state->receipt.images = *state->images->receipt();
    state->receipt.replacementPath = state->receipt.images.stagePath;
    state->receipt.retainedBeforePath = state->receipt.images.stagePath + ".before";
    detail::observeReplacement(detail::ReplacementBoundary::ByteImagesPrepared, state->receipt);
    state->original = std::move(*original);
    state->store = std::move(*store);
    state->before = std::move(*before);
    state->receipt.beforeMetadataSha256 = metadataDigest(state->before);
    state->parentPath = std::string(parentPath(state->receipt.images.targetPath));
    state->storePath = std::string(privateStorePath);
    const auto retainedError = [&](Error error) -> std::unexpected<Error> {
        return std::unexpected(Error(error.code(), error.describe() + "; original byte-stage receipt names '" +
                                                       state->receipt.replacementPath + "'; target was not retired"));
    };
    auto parent = openNative(state->parentPath, false, true);
    if (!parent)
        return retainedError(parent.error());
    state->parent = std::move(*parent);
    // Preserve the explicit capture boundary; do not adopt changed bytes or metadata here.
    if (auto unchanged = objectAt(target, state->before, state->beforePayload); !unchanged)
        return retainedError(unchanged.error());
    auto image = openNative(state->receipt.replacementPath, true);
    if (!image)
        return retainedError(image.error());
    state->replacement = std::move(*image);
    // Never metadata-mutate a name-swapped stage. The acquired native object must still be
    // the byte-Prepare receipt's object and exact After image before the first setter.
    auto originalStage = metadataOf(state->replacement, state->receipt.replacementPath, true);
    if (!originalStage)
        return retainedError(originalStage.error());
    const auto& expectedStage = state->receipt.images.stage;
    if (originalStage->volume != expectedStage.volume || originalStage->object != expectedStage.object)
        return retainedError(
            Error(ErrorCode::Busy, "replacement: acquired stage differs from original byte-Prepare receipt"));
    auto originalStageBytes =
        readNative(state->replacement, state->afterPayload.size(), state->receipt.replacementPath);
    if (!originalStageBytes)
        return retainedError(originalStageBytes.error());
    if (*originalStageBytes != state->afterPayload)
        return retainedError(Error(ErrorCode::Busy, "replacement: acquired stage bytes differ before metadata copy"));
#ifdef _WIN32
    if (auto restricted = privateStore(state->replacement.get(), state->receipt.replacementPath); !restricted)
        return retainedError(restricted.error());
#endif
    if (auto copied = supportedMetadataTo(state->replacement, state->before, state->receipt.replacementPath); !copied)
        return retainedError(copied.error());
    if (auto flushed = flushNative(state->replacement, state->receipt.replacementPath); !flushed)
        return retainedError(flushed.error());
    auto capturedAfter = metadataOf(state->replacement, state->receipt.replacementPath, true);
    if (!capturedAfter)
        return retainedError(capturedAfter.error());
    if (!sameSupportedMetadata(state->before, *capturedAfter, false))
        return fail(ErrorCode::IoError, "replacement: staged supported metadata differs; private image retained at '" +
                                            state->receipt.replacementPath + "'");
#ifdef _WIN32
    if (!sameSecurityOwners(state->before, *capturedAfter))
        return retainedError(Error(ErrorCode::Unsupported,
                                   "replacement: copied stage cannot reproduce source owner/group without privileges"));
#endif
    state->after = std::move(*capturedAfter);
    state->receipt.privateAfterMetadataSha256 = metadataDigest(state->after);
#ifdef _WIN32
    state->receipt.metadataProfile =
        "LIMITED NTFS: basic attributes/creation/write times, owner/group/DACL captured; private stage DACL until "
        "installation. SACL unobserved/unproven, not metadata-complete/Apply-ready. Observed "
        "EA/ADS/shortname/objectID/special attributes refuse.";
#else
    state->receipt.metadataProfile = "LIMITED ext4: mode/uid/gid/mtime and ordinary inode flags copied/verified; "
                                     "complete xattr name set must be empty. No atime/ctime/inode preservation "
                                     "promise; special inode flags/mode refuse. Not coordinator Apply-ready.";
#endif
    if (auto unchanged = objectAt(target, state->before, state->beforePayload); !unchanged)
        return retainedError(unchanged.error());
    if (auto parentCheck = directoryAt(state->parentPath, state->parent); !parentCheck)
        return retainedError(parentCheck.error());
    if (auto storeCheck = directoryAt(state->storePath, state->store, true); !storeCheck)
        return retainedError(storeCheck.error());
    if (auto staged = objectAt(state->receipt.replacementPath, state->after, state->afterPayload, true, true); !staged)
        return retainedError(staged.error());
    state->images.reset();
    return PreparedFileReplacement(std::move(state));
}

FileReplacementOutcome PreparedFileReplacement::retireOriginal() {
    if (!m_state)
        return {{}, Error(ErrorCode::InvalidArgument, "replacement: moved-from participant")};
    auto& state = *m_state;
    if (state.receipt.images.beforeSha256.empty()) {
        auto checked = validateCurrent(false);
        if (!checked)
            return {state.receipt, checked.error()};
        state.receipt.phase = FileReplacementPhase::OriginalRetained; // Verified absence; no original exists.
        return {state.receipt, std::nullopt};
    }
    const auto failed = [&](Error error) { return FileReplacementOutcome{state.receipt, std::move(error)}; };
    if (auto parent = directoryAt(state.parentPath, state.parent); !parent)
        return failed(parent.error());
    if (auto store = directoryAt(state.storePath, state.store, true); !store)
        return failed(store.error());
    if (state.receipt.phase != FileReplacementPhase::Prepared)
        return failed(Error(ErrorCode::InvalidArgument, "replacement: retirement requires Prepared phase"));
    if (auto current = objectAt(state.receipt.images.targetPath, state.before, state.beforePayload); !current)
        return failed(current.error());
    if (auto stage = objectAt(state.receipt.replacementPath, state.after, state.afterPayload, true, true); !stage)
        return failed(stage.error());
    if (auto renamed = renameNoReplace(state.original, state.parent, state.receipt.images.targetPath, state.store,
                                       state.receipt.retainedBeforePath);
        !renamed)
        return failed(renamed.error());
    state.receipt.phase = FileReplacementPhase::OriginalRetiredUnverified;
    detail::observeReplacement(detail::ReplacementBoundary::OriginalRetired, state.receipt);
    if (auto parent = directoryAt(state.parentPath, state.parent); !parent)
        return failed(parent.error());
    if (auto store = directoryAt(state.storePath, state.store, true); !store)
        return failed(store.error());
    if (auto retained = objectAt(state.receipt.retainedBeforePath, state.before, state.beforePayload, true, true);
        !retained)
        return failed(retained.error());
    state.receipt.phase = FileReplacementPhase::OriginalRetained;
    return {state.receipt, std::nullopt};
}

FileReplacementOutcome PreparedFileReplacement::installAfter() {
    if (!m_state)
        return {{}, Error(ErrorCode::InvalidArgument, "replacement: moved-from participant")};
    auto& state = *m_state;
    const auto failed = [&](Error error) { return FileReplacementOutcome{state.receipt, std::move(error)}; };
    if (auto parent = directoryAt(state.parentPath, state.parent); !parent)
        return failed(parent.error());
    if (auto store = directoryAt(state.storePath, state.store, true); !store)
        return failed(store.error());
    if (state.receipt.phase != FileReplacementPhase::OriginalRetained &&
        state.receipt.phase != FileReplacementPhase::AfterInstalledUnverified)
        return failed(Error(ErrorCode::InvalidArgument, "replacement: install requires verified retained Before"));
    if (state.receipt.phase == FileReplacementPhase::OriginalRetained) {
        if (state.receipt.images.beforeSha256.empty()) {
            if (auto absent = absentEndpoint(state.receipt.retainedBeforePath); !absent)
                return failed(absent.error());
        } else if (auto retained =
                       objectAt(state.receipt.retainedBeforePath, state.before, state.beforePayload, true, true);
                   !retained)
            return failed(retained.error());
        if (auto stage = objectAt(state.receipt.replacementPath, state.after, state.afterPayload, true, true); !stage)
            return failed(stage.error());
        if (auto renamed = renameNoReplace(state.replacement, state.store, state.receipt.replacementPath, state.parent,
                                           state.receipt.images.targetPath);
            !renamed)
            return failed(renamed.error());
        state.receipt.phase = FileReplacementPhase::AfterInstalledUnverified;
        detail::observeReplacement(detail::ReplacementBoundary::AfterInstalled, state.receipt);
        if (auto parent = directoryAt(state.parentPath, state.parent); !parent)
            return failed(parent.error());
        if (auto store = directoryAt(state.storePath, state.store, true); !store)
            return failed(store.error());
    }
#ifdef _WIN32
    if (!state.receipt.images.beforeSha256.empty()) {
        // Before broadening permissions, prove the named intended endpoint still owns this private
        // After object. A moved-away private image must never receive the workspace's broader DACL.
        if (auto endpoint = objectAt(state.receipt.images.targetPath, state.after, state.afterPayload, false);
            !endpoint)
            return failed(endpoint.error());
        auto securityState = metadataOf(state.replacement, state.receipt.images.targetPath);
        if (!securityState)
            return failed(securityState.error());
        if (securityState->security != state.after.security && securityState->security != state.before.security)
            return failed(Error(ErrorCode::Busy, "published After security changed outside the frozen transition"));
        // Stage permissions stay owner-private until the file is at the intended workspace endpoint.
        PSID owner = nullptr, group = nullptr;
        PACL acl = nullptr;
        BOOL defaulted = FALSE, present = FALSE;
        auto* descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(state.before.security.data());
        if (!GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) ||
            !GetSecurityDescriptorGroup(descriptor, &group, &defaulted) ||
            !GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) || !present)
            return failed(
                lastWin32Error("replacement: decode captured owner/group/DACL", state.receipt.images.targetPath));
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(descriptor, &control, &revision))
            return failed(lastWin32Error("replacement: decode captured DACL control", state.receipt.images.targetPath));
        const auto error = SetSecurityInfo(state.replacement.get(), SE_FILE_OBJECT,
                                           DACL_SECURITY_INFORMATION |
                                               ((control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION
                                                                              : UNPROTECTED_DACL_SECURITY_INFORMATION),
                                           nullptr, nullptr, acl, nullptr);
        if (error != ERROR_SUCCESS)
            return failed(win32Error(error, "replacement: install supported DACL; publication already occurred",
                                     state.receipt.images.targetPath));
        auto installed = metadataOf(state.replacement, state.receipt.images.targetPath);
        if (!installed)
            return failed(installed.error());
        if (!sameSupportedMetadata(*installed, state.before))
            return failed(
                Error(ErrorCode::IoError, "replacement: published supported metadata mismatch; retain both images"));
        state.after = std::move(*installed);
        state.receipt.installedAfterMetadataSha256 = metadataDigest(state.after);
    }
#endif
    if (auto installed = objectAt(state.receipt.images.targetPath, state.after, state.afterPayload); !installed)
        return failed(installed.error());
    state.receipt.phase = FileReplacementPhase::AfterInstalled;
    return {state.receipt, std::nullopt};
}

FileReplacementOutcome PreparedFileReplacement::restoreBefore() {
    if (!m_state)
        return {{}, Error(ErrorCode::InvalidArgument, "replacement: moved-from participant")};
    auto& state = *m_state;
    const auto failed = [&](Error error) { return FileReplacementOutcome{state.receipt, std::move(error)}; };
    if (state.receipt.images.beforeSha256.empty()) {
        if (auto checked = directoryAt(state.parentPath, state.parent); !checked)
            return failed(checked.error());
        if (auto checked = directoryAt(state.storePath, state.store, true); !checked)
            return failed(checked.error());
        if (auto absent = absentEndpoint(state.receipt.retainedBeforePath); !absent)
            return failed(absent.error());
        if (state.receipt.phase == FileReplacementPhase::AfterInstalled) {
            if (auto checked = validateCurrent(true); !checked)
                return failed(checked.error());
            if (auto empty = absentEndpoint(state.receipt.replacementPath); !empty)
                return failed(empty.error());
            auto renamed = renameNoReplace(state.replacement, state.parent, state.receipt.images.targetPath,
                                           state.store, state.receipt.replacementPath);
            if (!renamed)
                return failed(renamed.error());
            state.receipt.phase = FileReplacementPhase::AfterRetiredUnverified;
            detail::observeReplacement(detail::ReplacementBoundary::AfterRetired, state.receipt);
        } else if (state.receipt.phase != FileReplacementPhase::AfterRetiredUnverified &&
                   state.receipt.phase != FileReplacementPhase::AfterRetained)
            return failed(Error(ErrorCode::Busy, "create: inverse requires installed/retained After"));
        if (auto checked = directoryAt(state.parentPath, state.parent); !checked)
            return failed(checked.error());
        if (auto checked = directoryAt(state.storePath, state.store, true); !checked)
            return failed(checked.error());
        if (auto checked = objectAt(state.receipt.replacementPath, state.after, state.afterPayload, true, true);
            !checked)
            return failed(checked.error());
        if (auto absent = absentEndpoint(state.receipt.images.targetPath); !absent)
            return failed(absent.error());
        state.receipt.phase = FileReplacementPhase::BeforeRestored;
        return {state.receipt, std::nullopt};
    }
    if (auto parent = directoryAt(state.parentPath, state.parent); !parent)
        return failed(parent.error());
    if (auto store = directoryAt(state.storePath, state.store, true); !store)
        return failed(store.error());
    if (state.receipt.phase != FileReplacementPhase::AfterInstalled &&
        state.receipt.phase != FileReplacementPhase::AfterRetained)
        return failed(
            Error(ErrorCode::InvalidArgument, "replacement: inverse requires verified AfterInstalled/AfterRetained"));
    if (auto retained = objectAt(state.receipt.retainedBeforePath, state.before, state.beforePayload, true, true);
        !retained)
        return failed(retained.error());
    if (state.receipt.phase == FileReplacementPhase::AfterInstalled) {
        if (auto current = objectAt(state.receipt.images.targetPath, state.after, state.afterPayload); !current)
            return failed(current.error());
        if (auto renamed = renameNoReplace(state.replacement, state.parent, state.receipt.images.targetPath,
                                           state.store, state.receipt.replacementPath);
            !renamed)
            return failed(renamed.error());
        state.receipt.phase = FileReplacementPhase::AfterRetiredUnverified;
        detail::observeReplacement(detail::ReplacementBoundary::AfterRetired, state.receipt);
        if (auto parent = directoryAt(state.parentPath, state.parent); !parent)
            return failed(parent.error());
        if (auto store = directoryAt(state.storePath, state.store, true); !store)
            return failed(store.error());
        if (auto retained = objectAt(state.receipt.replacementPath, state.after, state.afterPayload, true, true);
            !retained)
            return failed(retained.error());
        state.receipt.phase = FileReplacementPhase::AfterRetained;
    }
    if (auto retained = objectAt(state.receipt.replacementPath, state.after, state.afterPayload, true, true); !retained)
        return failed(retained.error());
    if (auto renamed = renameNoReplace(state.original, state.store, state.receipt.retainedBeforePath, state.parent,
                                       state.receipt.images.targetPath);
        !renamed)
        return failed(renamed.error());
    state.receipt.phase = FileReplacementPhase::BeforeRestoredUnverified;
    detail::observeReplacement(detail::ReplacementBoundary::BeforeRestored, state.receipt);
    if (auto parent = directoryAt(state.parentPath, state.parent); !parent)
        return failed(parent.error());
    if (auto store = directoryAt(state.storePath, state.store, true); !store)
        return failed(store.error());
    if (auto restored = objectAt(state.receipt.images.targetPath, state.before, state.beforePayload); !restored)
        return failed(restored.error());
    state.receipt.phase = FileReplacementPhase::BeforeRestored;
    return {state.receipt, std::nullopt};
}

FileReplacementOutcome PreparedFileReplacement::settle() {
    if (!m_state)
        return {{}, Error(ErrorCode::InvalidArgument, "moved-from replacement")};
    auto& state = *m_state;
    if (state.receipt.images.beforeSha256.empty()) {
        if (state.receipt.phase == FileReplacementPhase::Prepared) {
            auto retired = retireOriginal();
            if (!retired.complete())
                return retired;
        }
        if (state.receipt.phase == FileReplacementPhase::OriginalRetained ||
            state.receipt.phase == FileReplacementPhase::AfterInstalledUnverified)
            return installAfter();
        if (state.receipt.phase == FileReplacementPhase::AfterRetiredUnverified ||
            state.receipt.phase == FileReplacementPhase::AfterRetained)
            return restoreBefore();
        auto checked = validateCurrent(state.receipt.phase == FileReplacementPhase::AfterInstalled);
        return {state.receipt, checked ? std::nullopt : std::optional(checked.error())};
    }
    auto verify = [&](std::string_view path, const Metadata& metadata, std::string_view bytes,
                      bool privateObject) -> std::optional<Error> {
        auto checked = objectAt(path, metadata, bytes, true, privateObject);
        if (!checked)
            return checked.error();
        return std::nullopt;
    };
    if (state.receipt.phase == FileReplacementPhase::OriginalRetiredUnverified) {
        if (auto error = verify(state.receipt.retainedBeforePath, state.before, state.beforePayload, true))
            return {state.receipt, std::move(error)};
        state.receipt.phase = FileReplacementPhase::OriginalRetained;
    }
    if (state.receipt.phase == FileReplacementPhase::Prepared) {
        auto retired = retireOriginal();
        if (!retired.complete())
            return retired;
    }
    if (state.receipt.phase == FileReplacementPhase::OriginalRetained ||
        state.receipt.phase == FileReplacementPhase::AfterInstalledUnverified)
        return installAfter();
    if (state.receipt.phase == FileReplacementPhase::AfterRetiredUnverified) {
        if (auto error = verify(state.receipt.replacementPath, state.after, state.afterPayload, true))
            return {state.receipt, std::move(error)};
        state.receipt.phase = FileReplacementPhase::AfterRetained;
    }
    if (state.receipt.phase == FileReplacementPhase::AfterRetained)
        return restoreBefore();
    if (state.receipt.phase == FileReplacementPhase::BeforeRestoredUnverified) {
        if (auto error = verify(state.receipt.images.targetPath, state.before, state.beforePayload, false))
            return {state.receipt, std::move(error)};
        state.receipt.phase = FileReplacementPhase::BeforeRestored;
    }
    if (state.receipt.phase == FileReplacementPhase::AfterInstalled) {
        if (auto error = verify(state.receipt.images.targetPath, state.after, state.afterPayload, false))
            return {state.receipt, std::move(error)};
    }
    return {state.receipt, std::nullopt};
}

FileReplacementOutcome PreparedFileReplacement::reapplyAfter() {
    if (!m_state)
        return {{}, Error(ErrorCode::InvalidArgument, "moved-from replacement")};
    auto& state = *m_state;
    if (state.receipt.images.beforeSha256.empty()) {
        if (state.receipt.phase != FileReplacementPhase::BeforeRestored)
            return {state.receipt, Error(ErrorCode::Busy, "create: Redo requires completed inverse")};
        auto checked = validateCurrent(false);
        if (!checked)
            return {state.receipt, checked.error()};
        state.receipt.phase = FileReplacementPhase::OriginalRetained;
        return installAfter();
    }
    if (state.receipt.phase != FileReplacementPhase::BeforeRestored)
        return {state.receipt, Error(ErrorCode::Busy, "Redo requires a completed inverse")};
    if (auto before = objectAt(state.receipt.images.targetPath, state.before, state.beforePayload); !before)
        return {state.receipt, before.error()};
    if (auto after = objectAt(state.receipt.replacementPath, state.after, state.afterPayload, true, true); !after)
        return {state.receipt, after.error()};
    state.receipt.phase = FileReplacementPhase::Prepared;
    return settle();
}

Result<void> PreparedFileReplacement::prepareMoveDestination(std::string_view destination) {
    if (m_state && m_state->receipt.images.beforeSha256.empty())
        return fail(ErrorCode::InvalidArgument, "create: absent Before cannot be moved");

    if (!m_state)
        return fail(ErrorCode::InvalidArgument, "moved-from replacement");
    auto& state = *m_state;
    auto normalized = normalizePath(destination);
    if (!normalized || !isAbsolutePath(normalized ? *normalized : std::string{}))
        return fail(ErrorCode::InvalidArgument, "move destination must be absolute");
    if (!state.receipt.moveDestination.empty()) {
        if (state.receipt.moveDestination != *normalized)
            return fail(ErrorCode::Busy, "frozen move destination changed");
        if (auto parent = directoryAt(state.moveParentPath, state.moveParent); !parent)
            return parent;
        if (state.receipt.phase == FileReplacementPhase::Prepared ||
            state.receipt.phase == FileReplacementPhase::MoveRestored)
            return emptyMoveEndpoint(*normalized, state.receipt.images.targetPath, state.before);
        return {};
    }
    if (state.receipt.phase != FileReplacementPhase::Prepared)
        return fail(ErrorCode::Busy, "move destination must be admitted before retirement");
    const auto parentPathValue = std::string(parentPath(*normalized));
    auto parent = openNative(parentPathValue, false, true);
    if (!parent)
        return std::unexpected(parent.error());
#ifdef _WIN32
    FILE_ID_INFO id{};
    if (!GetFileInformationByHandleEx(parent->get(), FileIdInfo, &id, sizeof(id)))
        return std::unexpected(lastWin32Error("move: inspect destination volume", destination));
    if (std::to_string(id.VolumeSerialNumber) != state.before.volume)
#else
    struct stat id{};
    if (::fstat(parent->get(), &id) != 0)
        return std::unexpected(errnoError(errno, "move: inspect destination volume", destination));
    if (std::to_string(id.st_dev) != state.before.volume)
#endif
        return fail(ErrorCode::Unsupported, "transaction movement crosses volumes");
    auto occupied = openNative(*normalized);
    if (occupied) {
        auto actual = metadataOf(*occupied, *normalized);
        if (!actual)
            return std::unexpected(actual.error());
        bool caseOnly = false;
#ifdef _WIN32
        auto from = toWin32Path(state.receipt.images.targetPath);
        auto to = toWin32Path(*normalized);
        caseOnly = from && to &&
                   CompareStringOrdinal(from->data(), static_cast<int>(from->size()), to->data(),
                                        static_cast<int>(to->size()), TRUE) == CSTR_EQUAL;
#endif
        if (!caseOnly || !sameObject(*actual, state.before))
            return fail(ErrorCode::AlreadyExists, "move destination is occupied");
    } else if (occupied.error().code() != ErrorCode::NotFound)
        return std::unexpected(occupied.error());
#ifdef _WIN32
    state.receipt.moveParent.volume = std::to_string(id.VolumeSerialNumber);
    state.receipt.moveParent.object.assign(reinterpret_cast<const char*>(id.FileId.Identifier),
                                           sizeof(id.FileId.Identifier));
#else
    state.receipt.moveParent.volume = std::to_string(id.st_dev);
    state.receipt.moveParent.object = std::to_string(id.st_ino);
#endif
    state.receipt.moveDestination = *normalized;
    state.moveParentPath = parentPathValue;
    state.moveParent = std::move(*parent);
    return {};
}

FileReplacementOutcome PreparedFileReplacement::moveOriginalTo(std::string_view destination) {
    if (!m_state)
        return {{}, Error(ErrorCode::InvalidArgument, "moved-from replacement")};
    auto& state = *m_state;
    const auto failWith = [&](Error error) { return FileReplacementOutcome{state.receipt, std::move(error)}; };
    auto normalized = normalizePath(destination);
    if (!normalized || !isAbsolutePath(normalized ? *normalized : std::string{}))
        return failWith(Error(ErrorCode::InvalidArgument, "move requires an absolute native destination"));
    if (auto admitted = prepareMoveDestination(*normalized); !admitted)
        return failWith(admitted.error());
    if (state.receipt.phase == FileReplacementPhase::OriginalRetiredUnverified) {
        if (auto retained = objectAt(state.receipt.retainedBeforePath, state.before, state.beforePayload, true, true);
            !retained)
            return failWith(retained.error());
        state.receipt.phase = FileReplacementPhase::OriginalRetained;
    }
    if (state.receipt.phase == FileReplacementPhase::Prepared) {
        auto retired = retireOriginal();
        if (!retired.complete())
            return retired;
    }
    if (auto parent = directoryAt(state.moveParentPath, state.moveParent); !parent)
        return failWith(parent.error());
    if (state.receipt.phase == FileReplacementPhase::OriginalRetained) {
        if (auto retained = objectAt(state.receipt.retainedBeforePath, state.before, state.beforePayload, true, true);
            !retained)
            return failWith(retained.error());
        if (auto moved = renameNoReplace(state.original, state.store, state.receipt.retainedBeforePath,
                                         state.moveParent, state.receipt.moveDestination);
            !moved)
            return failWith(moved.error());
        state.receipt.phase = FileReplacementPhase::OriginalMovedUnverified;
    }
    if (state.receipt.phase != FileReplacementPhase::OriginalMovedUnverified &&
        state.receipt.phase != FileReplacementPhase::OriginalMoved)
        return failWith(Error(ErrorCode::Busy, "move participant is in another operation"));
    if (auto current = objectAt(state.receipt.moveDestination, state.before, state.beforePayload); !current)
        return failWith(current.error());
    state.receipt.phase = FileReplacementPhase::OriginalMoved;
    return {state.receipt, std::nullopt};
}

FileReplacementOutcome PreparedFileReplacement::restoreMove() {
    if (!m_state)
        return {{}, Error(ErrorCode::InvalidArgument, "moved-from replacement")};
    auto& state = *m_state;
    const auto failWith = [&](Error error) { return FileReplacementOutcome{state.receipt, std::move(error)}; };
    if (auto parent = directoryAt(state.parentPath, state.parent); !parent)
        return failWith(parent.error());
    if (auto parent = directoryAt(state.moveParentPath, state.moveParent); !parent)
        return failWith(parent.error());
    if (auto store = directoryAt(state.storePath, state.store, true); !store)
        return failWith(store.error());
    if (state.receipt.phase == FileReplacementPhase::OriginalMoved) {
        if (auto current = objectAt(state.receipt.moveDestination, state.before, state.beforePayload); !current)
            return failWith(current.error());
        if (auto moved = renameNoReplace(state.original, state.moveParent, state.receipt.moveDestination, state.store,
                                         state.receipt.retainedBeforePath);
            !moved)
            return failWith(moved.error());
        state.receipt.phase = FileReplacementPhase::MoveRetainedForInverse;
    }
    if (state.receipt.phase == FileReplacementPhase::MoveRetainedForInverse) {
        if (auto retained = objectAt(state.receipt.retainedBeforePath, state.before, state.beforePayload, true, true);
            !retained)
            return failWith(retained.error());
        if (auto moved = renameNoReplace(state.original, state.store, state.receipt.retainedBeforePath, state.parent,
                                         state.receipt.images.targetPath);
            !moved)
            return failWith(moved.error());
        state.receipt.phase = FileReplacementPhase::MoveRestoredUnverified;
    }
    if (state.receipt.phase != FileReplacementPhase::MoveRestoredUnverified)
        return failWith(Error(ErrorCode::Busy, "inverse move requires the exact moved object"));
    if (auto current = objectAt(state.receipt.images.targetPath, state.before, state.beforePayload); !current)
        return failWith(current.error());
    state.receipt.phase = FileReplacementPhase::MoveRestored;
    return {state.receipt, std::nullopt};
}
FileReplacementOutcome PreparedFileReplacement::reapplyMove() {
    if (!m_state)
        return {{}, Error(ErrorCode::InvalidArgument, "moved-from replacement")};
    if (m_state->receipt.phase != FileReplacementPhase::MoveRestored)
        return {m_state->receipt, Error(ErrorCode::Busy, "redo move requires a completed inverse")};
    m_state->receipt.phase = FileReplacementPhase::Prepared;
    return moveOriginalTo(m_state->receipt.moveDestination);
}

Result<bool> PreparedFileReplacement::namesTargetEndpoint(std::string_view path) const {
    if (!m_state)
        return fail(ErrorCode::InvalidArgument, "moved-from replacement");
    auto normalized = normalizePath(path);
    if (!normalized || !isAbsolutePath(normalized ? *normalized : std::string{}))
        return fail(ErrorCode::InvalidArgument, "absolute endpoint spelling required");
    const auto leaf = fileName(*normalized), expected = fileName(m_state->receipt.images.targetPath);
#ifdef _WIN32
    auto a = toWide(leaf), b = toWide(expected);
    if (!a)
        return std::unexpected(a.error());
    if (!b)
        return std::unexpected(b.error());
    if (a->size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        b->size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return fail(ErrorCode::OutOfRange, "endpoint leaf exceeds native comparison bound");
    const auto same =
        CompareStringOrdinal(a->data(), static_cast<int>(a->size()), b->data(), static_cast<int>(b->size()), TRUE);
    if (!same)
        return std::unexpected(lastWin32Error("compare endpoint leaves", path));
    if (same != CSTR_EQUAL)
        return false;
#else
    if (leaf != expected)
        return false;
#endif
    // Comparison follows an open document's directory spelling, unlike the no-follow mutation
    // endpoint admission. The resulting native directory ID must equal the retained target parent.
    auto parent = openNative(parentPath(*normalized), false, true, true);
    if (!parent) {
        if (parent.error().code() == ErrorCode::NotFound)
            return false;
        return std::unexpected(parent.error());
    }
    auto id = directoryIdentity(*parent, parentPath(*normalized));
    if (!id)
        return std::unexpected(id.error());
    const auto& captured = m_state->receipt.images.targetParent;
    return id->volume == captured.volume && id->object == captured.object;
}

Result<void> PreparedFileReplacement::validateCurrent(bool after) const {
    if (!m_state)
        return fail(ErrorCode::InvalidArgument, "moved-from replacement");
    const auto& state = *m_state;
    if (state.receipt.images.beforeSha256.empty()) {
        if (auto checked = directoryAt(state.parentPath, state.parent); !checked)
            return checked;
        if (auto checked = directoryAt(state.storePath, state.store, true); !checked)
            return checked;
        if (auto empty = absentEndpoint(state.receipt.retainedBeforePath); !empty)
            return empty;
        if (after) {
            if (state.receipt.phase != FileReplacementPhase::AfterInstalled)
                return fail(ErrorCode::Busy, "create: installation is unfinished");
            if (auto empty = absentEndpoint(state.receipt.replacementPath); !empty)
                return empty;
            return objectAt(state.receipt.images.targetPath, state.after, state.afterPayload);
        }
        if (state.receipt.phase != FileReplacementPhase::Prepared &&
            state.receipt.phase != FileReplacementPhase::BeforeRestored)
            return fail(ErrorCode::Busy, "create: Before absence is unfinished");
        if (auto empty = absentEndpoint(state.receipt.images.targetPath); !empty)
            return empty;
        return objectAt(state.receipt.replacementPath, state.after, state.afterPayload, true, true);
    }
    if (auto parent = directoryAt(state.parentPath, state.parent); !parent)
        return std::unexpected(parent.error());
    if (auto store = directoryAt(state.storePath, state.store, true); !store)
        return std::unexpected(store.error());
    if (!state.receipt.moveDestination.empty()) {
        const auto expected = after ? FileReplacementPhase::OriginalMoved : FileReplacementPhase::MoveRestored;
        if (state.receipt.phase != expected && !(state.receipt.phase == FileReplacementPhase::Prepared && !after))
            return fail(ErrorCode::Busy, "file movement has unfinished effects");
        if (auto empty = emptyMoveEndpoint(after ? state.receipt.images.targetPath : state.receipt.moveDestination,
                                           after ? state.receipt.moveDestination : state.receipt.images.targetPath,
                                           state.before);
            !empty)
            return empty;
        if (!after) {
            if (auto empty = absentEndpoint(state.receipt.retainedBeforePath); !empty)
                return empty;
            if (auto staged = objectAt(state.receipt.replacementPath, state.after, state.afterPayload, true, true);
                !staged)
                return staged;
        }
        return objectAt(after ? state.receipt.moveDestination : state.receipt.images.targetPath, state.before,
                        state.beforePayload);
    }
    if (after) {
        if (state.receipt.phase != FileReplacementPhase::AfterInstalled)
            return fail(ErrorCode::Busy, "replacement has unfinished effects");
        if (auto original = objectAt(state.receipt.retainedBeforePath, state.before, state.beforePayload, true, true);
            !original)
            return std::unexpected(original.error());
        return objectAt(state.receipt.images.targetPath, state.after, state.afterPayload);
    }
    if (state.receipt.phase != FileReplacementPhase::Prepared &&
        state.receipt.phase != FileReplacementPhase::BeforeRestored)
        return fail(ErrorCode::Busy, "replacement is not at its Before state");
    if (auto original = objectAt(state.receipt.images.targetPath, state.before, state.beforePayload); !original)
        return std::unexpected(original.error());
    if (auto empty = absentEndpoint(state.receipt.retainedBeforePath); !empty)
        return empty;
    return objectAt(state.receipt.replacementPath, state.after, state.afterPayload, true, true);
}

Result<std::string> ensurePrivateTransactionStore(std::string_view target, std::string_view preferred,
                                                  std::string_view workspaceRoot) {
    auto targetFile = openNative(target);
    std::string targetVolume;
    if (targetFile) {
        auto identity = metadataOf(*targetFile, target);
        if (!identity)
            return std::unexpected(identity.error());
        targetVolume = identity->volume;
    } else {
        if (targetFile.error().code() != ErrorCode::NotFound)
            return std::unexpected(targetFile.error());
        auto targetParent = openNative(parentPath(target), false, true);
        if (!targetParent)
            return std::unexpected(targetParent.error());
        auto identity = directoryIdentity(*targetParent, parentPath(target));
        if (!identity)
            return std::unexpected(identity.error());
        targetVolume = identity->volume;
    }
    std::string chosen(preferred);
    auto parent = openNative(parentPath(chosen), false, true);
    bool sameVolume = false;
#ifdef _WIN32
    FILE_ID_INFO preferredId{};
    sameVolume = parent && GetFileInformationByHandleEx(parent->get(), FileIdInfo, &preferredId, sizeof(preferredId)) &&
                 std::to_string(preferredId.VolumeSerialNumber) == targetVolume;
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
        return std::unexpected(lastWin32Error("transaction store: current token", target));
    Native token(rawToken);
    DWORD length = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &length);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0 || length > 65536)
        return std::unexpected(lastWin32Error("transaction store: size current token", target));
    std::vector<unsigned char> bytes(length);
    if (!GetTokenInformation(token.get(), TokenUser, bytes.data(), length, &length))
        return std::unexpected(lastWin32Error("transaction store: current token", target));
    wchar_t* rawSid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &rawSid))
        return std::unexpected(lastWin32Error("transaction store: current SID", target));
    const std::wstring sid(rawSid);
    LocalFree(rawSid);
    const auto userTag =
        sha256Hex(std::string(reinterpret_cast<const char*>(sid.data()), sid.size() * sizeof(wchar_t))).substr(0, 16);
#else
    struct stat preferredId{};
    sameVolume =
        parent && ::fstat(parent->get(), &preferredId) == 0 && std::to_string(preferredId.st_dev) == targetVolume;
    const auto userTag = std::to_string(::geteuid());
#endif
    if (!sameVolume) {
        const auto base = workspaceRoot.empty() ? parentPath(target) : parentPath(workspaceRoot);
        auto fallback = joinPath(base, ".frost-refactoring-" + userTag);
        if (!fallback)
            return std::unexpected(fallback.error());
        chosen = std::move(*fallback);
    }
#ifdef _WIN32
    auto wide = toWin32Path(chosen);
    if (!wide)
        return std::unexpected(wide.error());
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const std::wstring sddl = L"D:P(A;OICI;FA;;;" + sid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        return std::unexpected(lastWin32Error("transaction store: private descriptor", chosen));
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const bool created = CreateDirectoryW(wide->c_str(), &attributes) != FALSE;
    const auto error = created ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    if (!created && error != ERROR_ALREADY_EXISTS)
        return std::unexpected(win32Error(error, "transaction store: exclusive private directory", chosen));
#else
    if (::mkdir(chosen.c_str(), 0700) != 0 && errno != EEXIST)
        return std::unexpected(errnoError(errno, "transaction store: exclusive private directory", chosen));
#endif
    auto directory = openNative(chosen, false, true);
    if (!directory)
        return std::unexpected(directory.error());
    if (auto checked = directoryAt(chosen, *directory, true); !checked)
        return std::unexpected(checked.error());
#ifdef _WIN32
    FILE_ID_INFO actual{};
    if (!GetFileInformationByHandleEx(directory->get(), FileIdInfo, &actual, sizeof(actual)))
        return std::unexpected(lastWin32Error("transaction store: verify volume", chosen));
    if (std::to_string(actual.VolumeSerialNumber) != targetVolume)
#else
    struct stat actual{};
    if (::fstat(directory->get(), &actual) != 0)
        return std::unexpected(errnoError(errno, "transaction store: verify volume", chosen));
    if (std::to_string(actual.st_dev) != targetVolume)
#endif
        return fail(ErrorCode::Unsupported, "transaction store is not on target volume");
    return chosen;
}

Result<PreparedFileReplacement> recoverFileReplacement(const FileReplacementReceipt& saved, std::string_view before,
                                                       std::string_view after, bool desiredBefore,
                                                       std::size_t maxBytes) {
    const bool creation = saved.images.beforeSha256.empty();
    if (creation &&
        (!before.empty() || saved.images.target != FileImageObservation{} || !saved.moveDestination.empty() ||
         !saved.beforeMetadataSha256.empty() || !saved.metadataProfile.starts_with("CREATE LIMITED:")))
        return fail(ErrorCode::Corrupt, "invalid absent-Before creation receipt");
    if (before.size() > maxBytes || after.size() > maxBytes ||
        (!creation && io::sha256Hex(before) != saved.images.beforeSha256) ||
        io::sha256Hex(after) != saved.images.afterSha256)
        return fail(ErrorCode::Corrupt, "recovery payload does not match frozen image hashes/limits");
    auto state = std::make_unique<PreparedFileReplacement::State>();
    state->receipt = saved;
    state->beforePayload = before;
    state->afterPayload = after;
    state->parentPath = std::string(parentPath(saved.images.targetPath));
    state->storePath = std::string(parentPath(saved.replacementPath));
    if (saved.replacementPath != saved.images.stagePath || parentPath(saved.retainedBeforePath) != state->storePath)
        return fail(ErrorCode::Corrupt, "recovery private artifact containment differs from manifest");
    auto parent = openNative(state->parentPath, false, true);
    if (!parent)
        return std::unexpected(parent.error());
    auto store = openNative(state->storePath, false, true);
    if (!store)
        return std::unexpected(store.error());
    if (auto checked = directoryAt(state->storePath, *store, true); !checked)
        return std::unexpected(checked.error());
    state->parent = std::move(*parent);
    state->store = std::move(*store);
    const auto parentMatches = [&](const Native& file, const FileImageObservation& observed) -> Result<void> {
#ifdef _WIN32
        FILE_ID_INFO id{};
        if (!GetFileInformationByHandleEx(file.get(), FileIdInfo, &id, sizeof(id)))
            return std::unexpected(lastWin32Error("recovery: retained directory identity", state->storePath));
        const std::string object(reinterpret_cast<const char*>(id.FileId.Identifier), sizeof(id.FileId.Identifier));
        if (std::to_string(id.VolumeSerialNumber) != observed.volume || object != observed.object)
#else
        struct stat id{};
        if (::fstat(file.get(), &id) != 0)
            return std::unexpected(errnoError(errno, "recovery: retained directory identity", state->storePath));
        if (std::to_string(id.st_dev) != observed.volume || std::to_string(id.st_ino) != observed.object)
#endif
            return fail(ErrorCode::Busy, "recovery parent/store object changed");
        return {};
    };
    if (auto checked = parentMatches(state->parent, saved.images.targetParent); !checked)
        return std::unexpected(checked.error());
    if (auto checked = parentMatches(state->store, saved.images.store); !checked)
        return std::unexpected(checked.error());
    std::string beforeLocation, afterLocation;
    if (!creation)
        for (const auto& path :
             std::array<std::string, 3>{saved.retainedBeforePath, saved.moveDestination, saved.images.targetPath}) {
            if (path.empty())
                continue;
            auto file = openNative(path);
            if (!file)
                continue;
            auto spelling = actualLeafSpelling(*file, path);
            if (!spelling)
                return std::unexpected(spelling.error());
            if (!*spelling)
                continue;
            auto meta = metadataOf(*file, path, path == saved.retainedBeforePath);
            if (!meta)
                continue;
            if (meta->volume != saved.images.target.volume || meta->object != saved.images.target.object)
                continue;
            auto bytes = readNative(*file, maxBytes, path);
            if (!bytes || *bytes != before || metadataDigest(*meta) != saved.beforeMetadataSha256)
                return fail(ErrorCode::Busy,
                            "retained Before data/metadata changed; recovery refuses inverse authority");
            state->original = std::move(*file);
            state->before = std::move(*meta);
            beforeLocation = path;
            break;
        }
    if (!creation && beforeLocation.empty())
        return fail(ErrorCode::Busy, "frozen Before object is unavailable; retain recovery records");
    for (const auto& path : std::array<std::string, 2>{saved.replacementPath, saved.images.targetPath}) {
        auto file = openNative(path, true);
        if (!file)
            file = openNative(path);
        if (!file)
            continue;
        auto meta = metadataOf(*file, path, path == saved.replacementPath);
        if (!meta)
            continue;
        if (meta->volume != saved.images.stage.volume || meta->object != saved.images.stage.object)
            continue;
        auto bytes = readNative(*file, maxBytes, path);
        const auto digest = metadataDigest(*meta);
        if (!bytes || *bytes != after ||
            (digest != saved.privateAfterMetadataSha256 && digest != saved.beforeMetadataSha256 &&
             (saved.installedAfterMetadataSha256.empty() || digest != saved.installedAfterMetadataSha256)))
            return fail(ErrorCode::Busy, "frozen After object was tampered; preserve conflict");
        state->replacement = std::move(*file);
        state->after = std::move(*meta);
        afterLocation = path;
        break;
    }
    if (creation) {
        if (afterLocation.empty())
            return fail(ErrorCode::Busy, "create recovery: frozen After unavailable; preserve artifacts");
        if (auto empty = absentEndpoint(saved.retainedBeforePath); !empty)
            return std::unexpected(empty.error());
        if (afterLocation == saved.images.targetPath) {
            if (auto empty = absentEndpoint(saved.replacementPath); !empty)
                return std::unexpected(empty.error());
            state->receipt.phase = FileReplacementPhase::AfterInstalled;
        } else {
            if (auto empty = absentEndpoint(saved.images.targetPath); !empty)
                return std::unexpected(empty.error());
            state->receipt.phase =
                desiredBefore ? FileReplacementPhase::BeforeRestored : FileReplacementPhase::Prepared;
        }
        return PreparedFileReplacement(std::move(state));
    }
    if (!saved.moveDestination.empty()) {
        state->moveParentPath = std::string(parentPath(saved.moveDestination));
        auto moveParent = openNative(state->moveParentPath, false, true);
        if (!moveParent)
            return std::unexpected(moveParent.error());
        state->moveParent = std::move(*moveParent);
        if (auto checked = parentMatches(state->moveParent, saved.moveParent); !checked)
            return std::unexpected(checked.error());
        if (beforeLocation == saved.moveDestination)
            state->receipt.phase = FileReplacementPhase::OriginalMoved;
        else if (beforeLocation == saved.retainedBeforePath)
            state->receipt.phase =
                desiredBefore ? FileReplacementPhase::MoveRetainedForInverse : FileReplacementPhase::OriginalRetained;
        else
            state->receipt.phase = desiredBefore ? FileReplacementPhase::MoveRestored : FileReplacementPhase::Prepared;
    } else {
        if (afterLocation.empty())
            return fail(ErrorCode::Busy, "frozen After object unavailable; preserve recovery material");
        if (beforeLocation == saved.images.targetPath && afterLocation == saved.replacementPath)
            state->receipt.phase =
                desiredBefore ? FileReplacementPhase::BeforeRestored : FileReplacementPhase::Prepared;
        else if (beforeLocation == saved.retainedBeforePath && afterLocation == saved.images.targetPath)
            state->receipt.phase = metadataDigest(state->after) == saved.beforeMetadataSha256
                                       ? FileReplacementPhase::AfterInstalled
                                       : FileReplacementPhase::AfterInstalledUnverified;
        else if (beforeLocation == saved.retainedBeforePath && afterLocation == saved.replacementPath)
            state->receipt.phase =
                desiredBefore ? FileReplacementPhase::AfterRetained : FileReplacementPhase::OriginalRetained;
        else
            return fail(ErrorCode::Busy, "contradictory native recovery layout; preserve all artifacts");
    }
    return PreparedFileReplacement(std::move(state));
}

struct TransactionStoreLease::State {
    Native file;
    Native directory;
};
TransactionStoreLease::TransactionStoreLease(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {}
TransactionStoreLease::~TransactionStoreLease() = default;
TransactionStoreLease::TransactionStoreLease(TransactionStoreLease&&) noexcept = default;
TransactionStoreLease& TransactionStoreLease::operator=(TransactionStoreLease&&) noexcept = default;
Result<TransactionStoreLease> acquireTransactionStoreLease(std::string_view storePath) {
    auto directory = openNative(storePath, false, true);
    if (!directory)
        return std::unexpected(directory.error());
    if (auto safe = directoryAt(storePath, *directory, true); !safe)
        return std::unexpected(safe.error());
    auto path = joinPath(storePath, ".coordinator-lock");
    if (!path)
        return std::unexpected(path.error());
    auto state = std::make_unique<TransactionStoreLease::State>();
    state->directory = std::move(*directory);
#ifdef _WIN32
    auto wide = toWin32Path(*path);
    if (!wide)
        return std::unexpected(wide.error());
    state->file.reset(CreateFileW(wide->c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!state->file.valid())
        return std::unexpected(lastWin32Error("transaction: acquire recovery owner", *path));
    FILE_ATTRIBUTE_TAG_INFO tag{};
    FILE_STANDARD_INFO info{};
    if (!GetFileInformationByHandleEx(state->file.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
        !GetFileInformationByHandleEx(state->file.get(), FileStandardInfo, &info, sizeof(info)))
        return std::unexpected(lastWin32Error("transaction: verify recovery lock", *path));
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || info.Directory || info.NumberOfLinks != 1)
        return fail(ErrorCode::Unsupported, "transaction recovery lock is not a private regular file");
#else
    state->file.reset(::open(path->c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600));
    if (!state->file.valid())
        return std::unexpected(errnoError(errno, "transaction: open recovery lock", *path));
    struct stat info{};
    if (::fstat(state->file.get(), &info) != 0)
        return std::unexpected(errnoError(errno, "transaction: inspect recovery lock", *path));
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_uid != ::geteuid() || (info.st_mode & 0777) != 0600)
        return fail(ErrorCode::Unsupported, "transaction recovery lock is not owner-private");
    if (::flock(state->file.get(), LOCK_EX | LOCK_NB) != 0)
        return std::unexpected(errnoError(errno, "transaction: acquire recovery owner", *path));
#endif
    return TransactionStoreLease(std::move(state));
}
Result<std::string> claimTransactionDirectory(std::string_view storePath, std::string_view leaf) {
    if (leaf.empty() || leaf.find_first_of("/\\:") != leaf.npos || leaf == "." || leaf == "..")
        return fail(ErrorCode::InvalidArgument, "transaction directory requires one ordinary leaf");
    auto parent = openNative(storePath, false, true);
    if (!parent)
        return std::unexpected(parent.error());
    if (auto safe = directoryAt(storePath, *parent, true); !safe)
        return std::unexpected(safe.error());
    auto path = joinPath(storePath, leaf);
    if (!path)
        return std::unexpected(path.error());
#ifdef _WIN32
    auto wide = toWin32Path(*path);
    if (!wide)
        return std::unexpected(wide.error());
    // Restricted inheritable parent was verified before creation, no permissive default-DACL fallback.
    if (!CreateDirectoryW(wide->c_str(), nullptr))
        return std::unexpected(lastWin32Error("transaction: claim exclusive recovery directory", *path));
#else
    if (::mkdirat(parent->get(), std::string(leaf).c_str(), 0700) != 0)
        return std::unexpected(errnoError(errno, "transaction: claim exclusive recovery directory", *path));
#endif
    auto owned = openNative(*path, false, true);
    if (!owned)
        return std::unexpected(owned.error());
    if (auto safe = directoryAt(*path, *owned, true); !safe)
        return std::unexpected(safe.error());
    return *path;
}
} // namespace frost::io
