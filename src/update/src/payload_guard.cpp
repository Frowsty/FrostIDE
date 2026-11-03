// SPDX-License-Identifier: Apache-2.0
// Read oplock and held path ancestry protect bytes through installer consumption.
#include <frost/update/payload_guard.hpp>

#include <array>
#include <cstring>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <sodium.h>
#include <winioctl.h>
#endif

namespace frost::update {
struct PayloadGuard::Impl {
    std::string path;
#ifdef _WIN32
    struct Handle {
        HANDLE value = INVALID_HANDLE_VALUE;
        explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
        ~Handle() {
            if (value && value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
        }
        Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
    };
    Handle file;
    Handle event;
    std::vector<Handle> directories;
    FILE_ID_INFO identity{};
    std::uint64_t size{};
    OVERLAPPED overlapped{};
    REQUEST_OPLOCK_INPUT_BUFFER request{};
    REQUEST_OPLOCK_OUTPUT_BUFFER response{};
    bool pending = false;
    ~Impl() {
        if (pending) {
            // Pending request memory belongs to this object until cancellation
            // has actually completed. Local driver stalls may delay destruction.
            CancelIoEx(file.value, &overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(file.value, &overlapped, &ignored, TRUE);
        }
    }
#endif
};
PayloadGuard::PayloadGuard(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
PayloadGuard::~PayloadGuard() = default;
PayloadGuard::PayloadGuard(PayloadGuard&&) noexcept = default;
PayloadGuard& PayloadGuard::operator=(PayloadGuard&&) noexcept = default;

#ifdef _WIN32
namespace {
Result<std::wstring> localPath(const std::filesystem::path& input) {
    auto path = input.native();
    for (auto& character : path)
        if (character == L'/')
            character = L'\\';
    if (path.size() < 4 || path.size() > 32000 || path.find(L'\0') != std::wstring::npos ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) || path[1] != L':' ||
        path[2] != L'\\' || path.find(L':', 2) != std::wstring::npos)
        return fail(ErrorCode::InvalidArgument, "Update payload requires an absolute local drive path");
    const std::wstring drive = path.substr(0, 3);
    const auto kind = GetDriveTypeW(drive.c_str());
    if (kind != DRIVE_FIXED && kind != DRIVE_REMOVABLE && kind != DRIVE_RAMDISK)
        return fail(ErrorCode::Unsupported, "Update payload requires local storage");
    std::array<wchar_t, 1024> device{};
    const auto driveName = path.substr(0, 2);
    if (!QueryDosDeviceW(driveName.c_str(), device.data(), static_cast<DWORD>(device.size())) ||
        !std::wstring_view(device.data()).starts_with(L"\\Device\\"))
        return fail(ErrorCode::Unsupported, "Update payload drive aliases are unsupported");
    std::size_t start = 3;
    while (start < path.size()) {
        auto end = path.find(L'\\', start);
        if (end == std::wstring::npos)
            end = path.size();
        const auto part = std::wstring_view(path).substr(start, end - start);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ')
            return fail(ErrorCode::InvalidArgument, "Update payload path is ambiguous");
        start = end + 1;
    }
    if (path.back() == L'\\')
        return fail(ErrorCode::InvalidArgument, "Update payload path names a directory");
    return path;
}
bool regular(HANDLE file, FILE_ID_INFO* identity, std::uint64_t size) {
    FILE_STANDARD_INFO standard{};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    return GetFileType(file) == FILE_TYPE_DISK &&
           GetFileInformationByHandleEx(file, FileIdInfo, identity, sizeof(*identity)) &&
           GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) &&
           GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
           !standard.Directory && standard.NumberOfLinks == 1 && !standard.DeletePending &&
           standard.EndOfFile.QuadPart >= 0 && static_cast<std::uint64_t>(standard.EndOfFile.QuadPart) == size &&
           !(attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
bool same(const FILE_ID_INFO& left, const FILE_ID_INFO& right) {
    return left.VolumeSerialNumber == right.VolumeSerialNumber &&
           std::memcmp(left.FileId.Identifier, right.FileId.Identifier, sizeof(left.FileId.Identifier)) == 0;
}
bool directory(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    return GetFileType(handle) == FILE_TYPE_DISK &&
           GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) &&
           (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}
} // namespace
#endif

Result<PayloadGuard> PayloadGuard::acquire(const std::filesystem::path& input, const Release& release) {
    if (release.size == 0 || release.size > 2ULL * 1024 * 1024 * 1024)
        return fail(ErrorCode::InvalidArgument, "Update payload size is invalid");
#ifdef _WIN32
    auto parsed = localPath(input);
    if (!parsed)
        return std::unexpected(parsed.error());
    auto impl = std::make_unique<Impl>();
    impl->size = release.size;
    const auto holdDirectories = [&](const std::wstring& path) -> Result<void> {
        std::size_t end = 2;
        for (;;) {
            const auto prefix = L"\\\\?\\" + path.substr(0, end + 1);
            Impl::Handle held(CreateFileW(prefix.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (held.value == INVALID_HANDLE_VALUE || !directory(held.value))
                return fail(ErrorCode::PermissionDenied, "Cannot retain a physical update payload directory");
            impl->directories.push_back(std::move(held));
            end = path.find(L'\\', end + 1);
            if (end == std::wstring::npos)
                break;
        }
        return {};
    };
    if (auto held = holdDirectories(*parsed); !held)
        return std::unexpected(held.error());
    const auto extended = L"\\\\?\\" + *parsed;
    impl->file.value = CreateFileW(extended.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED, nullptr);
    if (impl->file.value == INVALID_HANDLE_VALUE || !regular(impl->file.value, &impl->identity, release.size))
        return fail(ErrorCode::PermissionDenied, "Cannot retain a regular single-link update payload");
    std::array<wchar_t, 32> filesystem{};
    if (!GetVolumeInformationByHandleW(impl->file.value, nullptr, 0, nullptr, nullptr, nullptr, filesystem.data(),
                                       static_cast<DWORD>(filesystem.size())) ||
        std::wstring_view(filesystem.data()) != L"NTFS")
        return fail(ErrorCode::Unsupported, "Update payload ownership currently requires NTFS");
    impl->event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl->event.value)
        return fail(ErrorCode::IoError, "Cannot create update payload guard event");
    impl->overlapped.hEvent = impl->event.value;
    impl->request.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
    impl->request.StructureLength = sizeof(impl->request);
    impl->request.RequestedOplockLevel = OPLOCK_LEVEL_CACHE_READ;
    impl->request.Flags = REQUEST_OPLOCK_INPUT_FLAG_REQUEST;
    // A read oplock grant requires no existing writable user-mapped sections.
    // https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/granting-oplocks
    const auto granted = DeviceIoControl(impl->file.value, FSCTL_REQUEST_OPLOCK, &impl->request, sizeof(impl->request),
                                         &impl->response, sizeof(impl->response), nullptr, &impl->overlapped);
    if (granted || GetLastError() != ERROR_IO_PENDING)
        return fail(ErrorCode::PermissionDenied, "Update payload read oplock was not granted");
    impl->pending = true;
    std::array<wchar_t, 32768> physical{};
    const auto length = GetFinalPathNameByHandleW(
        impl->file.value, physical.data(), static_cast<DWORD>(physical.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (length < 4 || length >= physical.size() || !std::wstring_view(physical.data(), length).starts_with(L"\\\\?\\"))
        return fail(ErrorCode::IoError, "Cannot resolve update payload path");
    auto finalPath = localPath(std::filesystem::path(std::wstring(physical.data() + 4, length - 4)));
    if (!finalPath)
        return std::unexpected(finalPath.error());
    if (auto held = holdDirectories(*finalPath); !held)
        return std::unexpected(held.error());
    const std::wstring finalExtended = L"\\\\?\\" + *finalPath;
    const int utf8Size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, finalExtended.data(),
                                             static_cast<int>(finalExtended.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Size <= 0)
        return fail(ErrorCode::InvalidArgument, "Update payload path is not valid Unicode");
    impl->path.resize(static_cast<std::size_t>(utf8Size));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, finalExtended.data(), static_cast<int>(finalExtended.size()),
                            impl->path.data(), utf8Size, nullptr, nullptr) != utf8Size)
        return fail(ErrorCode::IoError, "Cannot encode update payload path");
    // This synchronous reader coexists with the continuously held read oplock.
    Impl::Handle reader(CreateFileW(finalExtended.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    FILE_ID_INFO readerIdentity{};
    if (reader.value == INVALID_HANDLE_VALUE || !regular(reader.value, &readerIdentity, release.size) ||
        !same(readerIdentity, impl->identity) || sodium_init() < 0)
        return fail(ErrorCode::PermissionDenied, "Update payload reader identity changed");
    crypto_hash_sha256_state hash{};
    crypto_hash_sha256_init(&hash);
    std::array<unsigned char, 65536> buffer{};
    std::uint64_t received = 0;
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(reader.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
            return fail(ErrorCode::IoError, "Cannot hash update payload");
        if (count == 0)
            break;
        if (count > release.size - received)
            return fail(ErrorCode::Corrupt, "Update payload exceeds its signed size");
        crypto_hash_sha256_update(&hash, buffer.data(), count);
        received += count;
    }
    Sha256 digest{};
    crypto_hash_sha256_final(&hash, digest.data());
    if (received != release.size || sodium_memcmp(digest.data(), release.digest.data(), digest.size()) != 0)
        return fail(ErrorCode::Corrupt, "Update payload does not match its signed digest");
    PayloadGuard guard(std::move(impl));
    if (auto checked = guard.validate(); !checked)
        return std::unexpected(checked.error());
    return guard;
#else
    (void)input;
    return fail(ErrorCode::Unsupported, "Windows update payload guard is unavailable");
#endif
}
Result<void> PayloadGuard::validate() const {
    if (!m_impl)
        return fail(ErrorCode::InvalidArgument, "Update payload guard is empty");
#ifdef _WIN32
    if (WaitForSingleObject(m_impl->event.value, 0) != WAIT_TIMEOUT)
        return fail(ErrorCode::PermissionDenied, "Update payload read oplock is no longer held");
    FILE_ID_INFO current{};
    if (!regular(m_impl->file.value, &current, m_impl->size) || !same(current, m_impl->identity))
        return fail(ErrorCode::PermissionDenied, "Update payload identity changed");
    for (const auto& held : m_impl->directories)
        if (!directory(held.value))
            return fail(ErrorCode::PermissionDenied, "Update payload directory changed");
    return {};
#else
    return fail(ErrorCode::Unsupported, "Windows update payload guard is unavailable");
#endif
}
std::string_view PayloadGuard::installerPath() const noexcept { return m_impl ? m_impl->path : std::string_view{}; }
} // namespace frost::update
