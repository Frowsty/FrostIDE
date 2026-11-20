// SPDX-License-Identifier: Apache-2.0
// Private file admission is checked on the open OS handle, not on a path checked before opening.
#include "agent_private.hpp"

#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"

#include <array>
#include <vector>
#ifdef _WIN32
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#else
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>
#endif
namespace frost::app {
namespace {
constexpr std::size_t kMaximum = 64 * 1024;
#ifdef _WIN32
struct Security {
    HANDLE token = nullptr;
    std::vector<std::byte> user;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ~Security() {
        if (token)
            CloseHandle(token);
        if (descriptor)
            LocalFree(descriptor);
    }
    PSID sid() { return reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid; }
    bool initialize() {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        if (!size)
            return false;
        user.resize(size);
        if (!GetTokenInformation(token, TokenUser, user.data(), size, &size))
            return false;
        LPWSTR sidText = nullptr;
        if (!ConvertSidToStringSidW(sid(), &sidText))
            return false;
        const auto sddl = L"D:P(A;;GA;;;" + std::wstring(sidText) + L")";
        LocalFree(sidText);
        return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                                    nullptr);
    }
};
bool privatelyOwned(HANDLE file, Security& security) {
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
                        &acl, nullptr, &descriptor) != ERROR_SUCCESS)
        return false;
    bool okay = owner && EqualSid(owner, security.sid()) && acl;
    if (okay)
        for (DWORD i = 0; i < acl->AceCount; ++i) {
            void* raw = nullptr;
            if (!GetAce(acl, i, &raw)) {
                okay = false;
                break;
            }
            const auto* header = static_cast<ACE_HEADER*>(raw);
            if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
                const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
                if (!EqualSid(const_cast<DWORD*>(&ace->SidStart), security.sid())) {
                    okay = false;
                    break;
                }
            } else if (header->AceType != ACCESS_DENIED_ACE_TYPE) {
                okay = false;
                break;
            }
        }
    LocalFree(descriptor);
    BY_HANDLE_FILE_INFORMATION info{};
    return okay && GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           info.nNumberOfLinks == 1;
}
#else
bool privatelyOwned(int file, bool directory) {
    struct stat info{};
    return fstat(file, &info) == 0 && info.st_uid == geteuid() && (info.st_mode & 0077) == 0 &&
           (directory ? S_ISDIR(info.st_mode) : (S_ISREG(info.st_mode) && info.st_nlink == 1));
}
#endif
} // namespace
Result<void> ensureAgentPrivateDirectory(std::string_view path) {
    if (!io::isAbsolutePath(path))
        return fail(ErrorCode::InvalidArgument, "private agent directory must be absolute");
    auto created = io::realFileSystem().createDirectories(path);
    if (!created)
        return created;
#ifdef _WIN32
    Security security;
    const auto name = io::toWin32Path(path);
    if (!security.initialize() || !name)
        return fail(ErrorCode::PermissionDenied, "cannot identify private agent owner");
    const HANDLE directory =
        CreateFileW(name->c_str(), READ_CONTROL | WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE)
        return fail(ErrorCode::PermissionDenied, "cannot open private agent directory");
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR existing = nullptr;
    const bool owned = GetSecurityInfo(directory, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr,
                                       nullptr, &existing) == ERROR_SUCCESS &&
                       EqualSid(owner, security.sid());
    if (existing)
        LocalFree(existing);
    BOOL present = FALSE, defaulted = FALSE;
    PACL acl = nullptr;
    GetSecurityDescriptorDacl(security.descriptor, &present, &acl, &defaulted);
    const bool okay =
        owned &&
        SetSecurityInfo(directory, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                        nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS &&
        privatelyOwned(directory, security);
    CloseHandle(directory);
#else
    const int directory = open(std::string(path).c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat info{};
    const bool okay = directory >= 0 && fstat(directory, &info) == 0 && info.st_uid == geteuid() &&
                      fchmod(directory, 0700) == 0 && privatelyOwned(directory, true);
    if (directory >= 0)
        close(directory);
#endif
    return okay ? Result<void>{} : fail(ErrorCode::PermissionDenied, "agent directory is not current-user private");
}
Result<void> writeAgentPrivateFile(std::string_view path, std::string_view bytes) {
    if (!io::isAbsolutePath(path) || bytes.size() > kMaximum)
        return fail(ErrorCode::InvalidArgument, "invalid private agent file");
#ifdef _WIN32
    Security security;
    const auto name = io::toWin32Path(path);
    if (!security.initialize() || !name)
        return fail(ErrorCode::PermissionDenied, "cannot protect agent file");
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.descriptor, FALSE};
    const HANDLE file =
        CreateFileW(name->c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return fail(ErrorCode::IoError, "cannot create private agent file");
    DWORD written = 0;
    const bool okay = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                      written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
#else
    const int file = open(std::string(path).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0)
        return fail(ErrorCode::IoError, "cannot create private agent file");
    const bool okay = write(file, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()) && fsync(file) == 0;
    close(file);
#endif
    if (!okay) {
        (void)io::realFileSystem().removeFile(path);
        return fail(ErrorCode::IoError, "cannot flush private agent file");
    }
    return {};
}
Result<std::string> readAgentPrivateFile(std::string_view path) {
    if (!io::isAbsolutePath(path))
        return fail(ErrorCode::InvalidArgument, "private agent file must be absolute");
    std::string bytes;
    std::array<char, 4096> buffer{};
#ifdef _WIN32
    Security security;
    const auto name = io::toWin32Path(path);
    if (!security.initialize() || !name)
        return fail(ErrorCode::PermissionDenied, "cannot identify agent file owner");
    const HANDLE file = CreateFileW(name->c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return fail(ErrorCode::IoError, "cannot read private agent file");
    if (!privatelyOwned(file, security)) {
        CloseHandle(file);
        return fail(ErrorCode::PermissionDenied, "agent discovery file is not private");
    }
    bool okay = true;
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            okay = false;
            break;
        }
        if (!count)
            break;
        if (bytes.size() + count > kMaximum) {
            okay = false;
            break;
        }
        bytes.append(buffer.data(), count);
    }
    CloseHandle(file);
#else
    const int file = open(std::string(path).c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (file < 0)
        return fail(ErrorCode::IoError, "cannot read private agent file");
    if (!privatelyOwned(file, false)) {
        close(file);
        return fail(ErrorCode::PermissionDenied, "agent discovery file is not private");
    }
    bool okay = true;
    for (;;) {
        const auto count = read(file, buffer.data(), buffer.size());
        if (count < 0) {
            okay = false;
            break;
        }
        if (!count)
            break;
        if (bytes.size() + static_cast<std::size_t>(count) > kMaximum) {
            okay = false;
            break;
        }
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    close(file);
#endif
    if (!okay)
        return fail(ErrorCode::IoError, "invalid or oversized private agent file");
    return bytes;
}
} // namespace frost::app
