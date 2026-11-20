// SPDX-License-Identifier: Apache-2.0
#include "process_sandbox_internal.hpp"
#include "frost/io/os_error.hpp"
#include "frost/io/utf.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <set>
#include <sddl.h>
#include <userenv.h>
#include <bcrypt.h>
namespace frost::io {
namespace {
struct Sid { PSID value = nullptr; ~Sid() { if (value) FreeSid(value); } };
struct Local { void* value = nullptr; ~Local() { if (value) LocalFree(value); } };
struct Handle { HANDLE value = INVALID_HANDLE_VALUE; ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
Result<std::wstring> wide(std::string_view text) {
    auto value = utf8ToUtf16(text); if (!value) return std::unexpected(value.error());
    return std::wstring(reinterpret_cast<const wchar_t*>(value->data()), value->size());
}
Result<std::string> sidText(PSID sid) {
    LPWSTR raw = nullptr;
    if (!ConvertSidToStringSidW(sid, &raw)) return std::unexpected(lastWin32Error("Sandbox SID text"));
    Local cleanup{raw}; return utf16ToUtf8(std::u16string_view(reinterpret_cast<const char16_t*>(raw)));
}
Result<std::string> currentUserSid() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return std::unexpected(lastWin32Error("Sandbox current user"));
    DWORD size = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<std::byte> buffer(size);
    if (!GetTokenInformation(token.value, TokenUser, buffer.data(), size, &size)) return std::unexpected(lastWin32Error("Sandbox user SID"));
    return sidText(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
}
Result<void> pinLocalPath(std::string_view input, std::vector<HANDLE>& handles) {
    auto normalized = normalizePath(input, PathStyle::Windows); if (!normalized) return std::unexpected(normalized.error());
    if (normalized->size() < 3 || (*normalized)[1] != ':' || (*normalized)[2] != '\\')
        return fail(ErrorCode::Unsupported, "Sandbox staging requires local drive paths; UNC and device paths are unsupported");
    std::size_t end = 3;
    for (;;) {
        if (handles.size() >= 4096) return fail(ErrorCode::OutOfRange, "Sandbox path pin budget exceeded");
        auto value = wide(normalized->substr(0, end)); if (!value) return std::unexpected(value.error());
        HANDLE handle = CreateFileW(value->c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return std::unexpected(lastWin32Error("Pin sandbox path"));
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            CloseHandle(handle); return fail(ErrorCode::PermissionDenied, "Sandbox staging paths cannot traverse reparse points");
        }
        handles.push_back(handle);
        if (end == normalized->size()) return {};
        const auto next = normalized->find('\\', end + 1);
        end = next == normalized->npos ? normalized->size() : next;
    }
}
struct Pins { std::vector<HANDLE> values; ~Pins() { for (auto handle : values) CloseHandle(handle); } };
Result<void> security(std::string_view user, std::string_view app, DWORD appRights, bool writable, Local& descriptor) {
    char mask[16]{}; std::snprintf(mask, sizeof(mask), "0x%08lx", static_cast<unsigned long>(appRights));
    const auto formatted = "O:" + std::string(user) + "D:P(A;;FA;;;SY)(A;;FA;;;" + std::string(user) + ")(A;;" +
        std::string(mask) + ";;;" + std::string(app) + ")" + (writable ? "S:(ML;;NW;;;LW)" : "");
    auto text = wide(formatted); if (!text) return std::unexpected(text.error());
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(text->c_str(), SDDL_REVISION_1, &raw, nullptr))
        return std::unexpected(lastWin32Error("Create private sandbox security descriptor"));
    descriptor.value = raw; return {};
}
DWORD rights(SandboxAccess access, bool directory) {
    DWORD mask = FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    if (hasSandboxAccess(access, SandboxAccess::Read)) mask |= directory ? FILE_LIST_DIRECTORY | FILE_TRAVERSE : FILE_GENERIC_READ;
    if (hasSandboxAccess(access, SandboxAccess::Execute)) mask |= FILE_GENERIC_EXECUTE;
    if (hasSandboxAccess(access, SandboxAccess::Write)) mask |= directory ? FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY : FILE_GENERIC_WRITE;
    return mask;
}
}
Result<ProcessSpec> stageWindowsSandboxProcess(const ProcessSpec& original, std::span<const SandboxStagingFile> files, std::string_view privateParent) {
    if (!original.sandbox) return fail(ErrorCode::InvalidArgument, "Private staging requires an explicit sandbox profile");
    if (auto valid = validateProcessSandbox(original, true, false); !valid) return std::unexpected(valid.error());
    if (files.empty() || files.size() > 256) return fail(ErrorCode::InvalidArgument, "Sandbox staging requires 1..256 captured files");
    Pins parentPins;
    if (auto pinned = pinLocalPath(privateParent, parentPins.values); !pinned) return std::unexpected(pinned.error());
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return fail(ErrorCode::IoError, "Cannot create private sandbox identity");
    constexpr char hex[] = "0123456789abcdef"; std::string nonce;
    for (auto byte : random) { nonce += hex[byte >> 4]; nonce += hex[byte & 15]; }
    const auto name = "frost.extension." + nonce;
    auto moniker = wide(name); if (!moniker) return std::unexpected(moniker.error());
    Sid app;
    const auto created = CreateAppContainerProfile(moniker->c_str(), L"FrostIDE extension", L"Private explicit extension payload", nullptr, 0, &app.value);
    // A fresh random identity must not silently adopt an existing profile or foreign ACL state.
    if (FAILED(created)) return fail(ErrorCode::Unsupported, "Cannot create private AppContainer profile; no ordinary launch fallback");
    const auto root = std::string(privateParent) + "\\" + nonce;
    auto appSid = sidText(app.value), user = currentUserSid();
    if (!appSid) return std::unexpected(appSid.error()); if (!user) return std::unexpected(user.error());
    Local directoryDescriptor;
    if (auto secured = security(*user, *appSid, FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, false, directoryDescriptor); !secured) return std::unexpected(secured.error());
    SECURITY_ATTRIBUTES directorySecurity{sizeof(SECURITY_ATTRIBUTES), directoryDescriptor.value, FALSE};
    auto rootWide = wide(root); if (!rootWide) return std::unexpected(rootWide.error());
    if (!CreateDirectoryW(rootWide->c_str(), &directorySecurity)) return std::unexpected(lastWin32Error("Create NEW private sandbox staging"));
    ProcessSpec mapped = original; mapped.workingDirectory = root;
    mapped.sandbox->windowsStagingRoot = root; mapped.sandbox->windowsAppContainerName = name; mapped.sandbox->paths.clear();
    std::map<std::string, std::string> mapping; std::set<std::string> destinations, directories{root}; std::size_t total = 0;
    for (const auto& file : files) {
        if (file.contents.size() > 64 * 1024 * 1024 || file.contents.size() > 256 * 1024 * 1024 - total ||
            file.relativePath.empty() || file.relativePath.find(':') != file.relativePath.npos || file.relativePath.starts_with('/') || file.relativePath.starts_with('\\'))
            return fail(ErrorCode::InvalidArgument, "Sandbox staging files exceed bounds or have invalid relative layout");
        total += file.contents.size();
        if (!sandboxAllowsPath(*original.sandbox, file.sourcePath, file.access, PathStyle::Windows))
            return fail(ErrorCode::PermissionDenied, "Staged input exceeds the explicitly granted content rights");
        auto destination = normalizePath(root + "\\" + file.relativePath, PathStyle::Windows);
        ProcessSandboxProfile scope; scope.paths.push_back({root, file.access, true});
        if (!destination || !sandboxAllowsPath(scope, *destination, file.access, PathStyle::Windows))
            return fail(ErrorCode::InvalidArgument, "Sandbox staging layout escapes its private generation");
        std::string folded = *destination; std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : static_cast<char>(c); });
        if (!destinations.insert(folded).second || mapping.contains(file.sourcePath)) return fail(ErrorCode::InvalidArgument, "Duplicate sandbox staging identity");
        auto slash = destination->find_last_of('\\');
        const auto parent = destination->substr(0, slash);
        for (auto end = root.size(); end < parent.size();) {
            end = parent.find('\\', end + 1); if (end == parent.npos) end = parent.size();
            const auto dir = parent.substr(0, end);
            if (directories.insert(dir).second) {
                auto text = wide(dir); if (!text) return std::unexpected(text.error());
                if (!CreateDirectoryW(text->c_str(), &directorySecurity)) return std::unexpected(lastWin32Error("Create private sandbox subdirectory"));
            }
        }
        Local descriptor;
        if (auto secured = security(*user, *appSid, rights(file.access, false), hasSandboxAccess(file.access, SandboxAccess::Write), descriptor); !secured) return std::unexpected(secured.error());
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor.value, FALSE};
        auto text = wide(*destination); if (!text) return std::unexpected(text.error());
        Handle output{CreateFileW(text->c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (output.value == INVALID_HANDLE_VALUE) return std::unexpected(lastWin32Error("Create private staged file"));
        DWORD done = 0;
        if ((!file.contents.empty() && (!WriteFile(output.value, file.contents.data(), static_cast<DWORD>(file.contents.size()), &done, nullptr) || done != file.contents.size())) || !FlushFileBuffers(output.value))
            return std::unexpected(lastWin32Error("Write private staged bytes"));
        mapping.emplace(file.sourcePath, *destination); mapped.sandbox->paths.push_back({*destination, file.access, false});
    }
    for (const auto& directory : directories) mapped.sandbox->paths.push_back({directory, SandboxAccess::Read, false});
    const auto executable = mapping.find(original.executable);
    if (executable == mapping.end()) return fail(ErrorCode::PermissionDenied, "Sandbox executable is absent from captured staged inputs");
    mapped.executable = executable->second;
    for (auto& argument : mapped.arguments) if (const auto found = mapping.find(argument); found != mapping.end()) argument = found->second;
    if (auto valid = validateProcessSpec(mapped, true); !valid) return std::unexpected(valid.error());
    return mapped;
}
namespace detail {
Result<std::unique_ptr<WindowsSandboxAttributes>> prepareWindowsSandboxAttributes(const ProcessSpec& spec) {
    if (!spec.sandbox) return std::unique_ptr<WindowsSandboxAttributes>{};
#if defined(PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY)
    if (auto valid = validateProcessSandbox(spec, true); !valid) return std::unexpected(valid.error());
    auto attributes = std::make_unique<WindowsSandboxAttributes>();
    auto name = wide(spec.sandbox->windowsAppContainerName); if (!name) return std::unexpected(name.error());
    if (FAILED(DeriveAppContainerSidFromAppContainerName(name->c_str(), &attributes->appSid)))
        return fail(ErrorCode::Unsupported, "Cannot derive staged LPAC identity");
    attributes->capabilities.AppContainerSid = attributes->appSid;
    if (spec.sandbox->network == SandboxNetwork::InternetClient) {
        DWORD size = sizeof(attributes->networkSid);
        if (!CreateWellKnownSid(WinCapabilityInternetClientSid, nullptr, attributes->networkSid, &size))
            return fail(ErrorCode::Unsupported, "Explicit Internet-client sandbox capability unavailable");
        attributes->network = {attributes->networkSid, SE_GROUP_ENABLED};
        attributes->capabilities.CapabilityCount = 1; attributes->capabilities.Capabilities = &attributes->network;
    }
    std::vector<HANDLE> pins;
    for (const auto& grant : spec.sandbox->paths) {
        if (auto pinned = pinLocalPath(grant.path, pins); !pinned) { for (auto handle : pins) CloseHandle(handle); return std::unexpected(pinned.error()); }
    }
    attributes->pinnedPaths = std::move(pins);
    return attributes;
#else
    return fail(ErrorCode::Unsupported, "Build lacks required LPAC enforcement; no ordinary launch fallback");
#endif
}
}
}
