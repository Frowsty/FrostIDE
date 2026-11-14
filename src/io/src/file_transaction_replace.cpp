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
#else
#include <linux/fs.h>
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

Result<Native> openNative(std::string_view path, bool writable = false, bool directory = false) {
#ifdef _WIN32
    auto wide = toWin32Path(path);
    if (!wide)
        return std::unexpected(wide.error());
    DWORD access = GENERIC_READ | READ_CONTROL;
    if (!directory)
        access |= DELETE;
    if (writable)
        access |= GENERIC_WRITE | WRITE_DAC | FILE_WRITE_ATTRIBUTES;
    Native file(CreateFileW(wide->c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!file.valid())
        return std::unexpected(lastWin32Error("replacement: open native object", path));
#else
    int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
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
    explicit State(PreparedFileImages value) : images(std::move(value)) {}
    PreparedFileImages images;
    FileReplacementReceipt receipt;
    Metadata before;
    Metadata after;
    Native original;
    Native replacement;
    Native parent;
    Native store;
    std::string parentPath;
    std::string storePath;
};

PreparedFileReplacement::PreparedFileReplacement(std::unique_ptr<State> state) noexcept : m_state(std::move(state)) {}
PreparedFileReplacement::~PreparedFileReplacement() = default;
PreparedFileReplacement::PreparedFileReplacement(PreparedFileReplacement&&) noexcept = default;
PreparedFileReplacement& PreparedFileReplacement::operator=(PreparedFileReplacement&&) noexcept = default;
const FileReplacementReceipt* PreparedFileReplacement::receipt() const noexcept {
    return m_state ? &m_state->receipt : nullptr;
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
    state->receipt.images = *state->images.receipt();
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
    if (auto unchanged = objectAt(target, state->before, state->images.beforeBytes()); !unchanged)
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
        readNative(state->replacement, state->images.afterBytes().size(), state->receipt.replacementPath);
    if (!originalStageBytes)
        return retainedError(originalStageBytes.error());
    if (*originalStageBytes != state->images.afterBytes())
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
    if (auto unchanged = objectAt(target, state->before, state->images.beforeBytes()); !unchanged)
        return retainedError(unchanged.error());
    if (auto parentCheck = directoryAt(state->parentPath, state->parent); !parentCheck)
        return retainedError(parentCheck.error());
    if (auto storeCheck = directoryAt(state->storePath, state->store, true); !storeCheck)
        return retainedError(storeCheck.error());
    if (auto staged = objectAt(state->receipt.replacementPath, state->after, state->images.afterBytes(), true, true);
        !staged)
        return retainedError(staged.error());
    return PreparedFileReplacement(std::move(state));
}

FileReplacementOutcome PreparedFileReplacement::retireOriginal() {
    if (!m_state)
        return {{}, Error(ErrorCode::InvalidArgument, "replacement: moved-from participant")};
    auto& state = *m_state;
    const auto failed = [&](Error error) { return FileReplacementOutcome{state.receipt, std::move(error)}; };
    if (auto parent = directoryAt(state.parentPath, state.parent); !parent)
        return failed(parent.error());
    if (auto store = directoryAt(state.storePath, state.store, true); !store)
        return failed(store.error());
    if (state.receipt.phase != FileReplacementPhase::Prepared)
        return failed(Error(ErrorCode::InvalidArgument, "replacement: retirement requires Prepared phase"));
    if (auto current = objectAt(state.receipt.images.targetPath, state.before, state.images.beforeBytes()); !current)
        return failed(current.error());
    if (auto stage = objectAt(state.receipt.replacementPath, state.after, state.images.afterBytes(), true, true);
        !stage)
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
    if (auto retained =
            objectAt(state.receipt.retainedBeforePath, state.before, state.images.beforeBytes(), true, true);
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
    if (state.receipt.phase != FileReplacementPhase::OriginalRetained)
        return failed(Error(ErrorCode::InvalidArgument, "replacement: install requires verified retained Before"));
    if (auto retained =
            objectAt(state.receipt.retainedBeforePath, state.before, state.images.beforeBytes(), true, true);
        !retained)
        return failed(retained.error());
    if (auto stage = objectAt(state.receipt.replacementPath, state.after, state.images.afterBytes(), true, true);
        !stage)
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
#ifdef _WIN32
    // Before broadening permissions, prove the named intended endpoint still owns this private
    // After object. A moved-away private image must never receive the workspace's broader DACL.
    if (auto endpoint = objectAt(state.receipt.images.targetPath, state.after, state.images.afterBytes()); !endpoint)
        return failed(endpoint.error());
    // Stage permissions stay owner-private until the file is at the intended workspace endpoint.
    PSID owner = nullptr, group = nullptr;
    PACL acl = nullptr;
    BOOL defaulted = FALSE, present = FALSE;
    auto* descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(state.before.security.data());
    if (!GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) ||
        !GetSecurityDescriptorGroup(descriptor, &group, &defaulted) ||
        !GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) || !present)
        return failed(lastWin32Error("replacement: decode captured owner/group/DACL", state.receipt.images.targetPath));
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
#endif
    if (auto installed = objectAt(state.receipt.images.targetPath, state.after, state.images.afterBytes()); !installed)
        return failed(installed.error());
    state.receipt.phase = FileReplacementPhase::AfterInstalled;
    return {state.receipt, std::nullopt};
}

FileReplacementOutcome PreparedFileReplacement::restoreBefore() {
    if (!m_state)
        return {{}, Error(ErrorCode::InvalidArgument, "replacement: moved-from participant")};
    auto& state = *m_state;
    const auto failed = [&](Error error) { return FileReplacementOutcome{state.receipt, std::move(error)}; };
    if (auto parent = directoryAt(state.parentPath, state.parent); !parent)
        return failed(parent.error());
    if (auto store = directoryAt(state.storePath, state.store, true); !store)
        return failed(store.error());
    if (state.receipt.phase != FileReplacementPhase::AfterInstalled &&
        state.receipt.phase != FileReplacementPhase::AfterRetained)
        return failed(
            Error(ErrorCode::InvalidArgument, "replacement: inverse requires verified AfterInstalled/AfterRetained"));
    if (auto retained =
            objectAt(state.receipt.retainedBeforePath, state.before, state.images.beforeBytes(), true, true);
        !retained)
        return failed(retained.error());
    if (state.receipt.phase == FileReplacementPhase::AfterInstalled) {
        if (auto current = objectAt(state.receipt.images.targetPath, state.after, state.images.afterBytes()); !current)
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
        if (auto retained = objectAt(state.receipt.replacementPath, state.after, state.images.afterBytes(), true, true);
            !retained)
            return failed(retained.error());
        state.receipt.phase = FileReplacementPhase::AfterRetained;
    }
    if (auto retained = objectAt(state.receipt.replacementPath, state.after, state.images.afterBytes(), true, true);
        !retained)
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
    if (auto restored = objectAt(state.receipt.images.targetPath, state.before, state.images.beforeBytes()); !restored)
        return failed(restored.error());
    state.receipt.phase = FileReplacementPhase::BeforeRestored;
    return {state.receipt, std::nullopt};
}
} // namespace frost::io
