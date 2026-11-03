// SPDX-License-Identifier: Apache-2.0
// Read one source via held, no-reparse Windows ancestry. The owning parent must enforce
// workspace trust and terminate this process on cancellation/timeout; this helper does not.
#ifdef _WIN32
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include <winternl.h>

namespace {
struct Handles {
    std::vector<HANDLE> values;
    ~Handles() {
        for (HANDLE h : values)
            CloseHandle(h);
    }
};
using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                        ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
// Adapted from the reviewed Rename held-lineage implementation (tree cdc2676a).
HANDLE openChild(NtCreateFileFn open, HANDLE parent, std::wstring_view name, bool directory) {
    if (name.empty() || name.size() > 32766)
        return INVALID_HANDLE_VALUE;
    struct Pending {
        std::wstring name;
        UNICODE_STRING unicode{};
        OBJECT_ATTRIBUTES attributes{};
        IO_STATUS_BLOCK status{};
        HANDLE parent = nullptr;
        HANDLE result = nullptr;
        ~Pending() {
            if (result && result != INVALID_HANDLE_VALUE)
                CloseHandle(result);
            if (parent && parent != INVALID_HANDLE_VALUE)
                CloseHandle(parent);
        }
    };
    auto p = std::make_unique<Pending>();
    p->name = name;
    if (!DuplicateHandle(GetCurrentProcess(), parent, GetCurrentProcess(), &p->parent, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return INVALID_HANDLE_VALUE;
    p->unicode.Buffer = p->name.data();
    p->unicode.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    p->unicode.MaximumLength = p->unicode.Length;
    p->attributes.Length = sizeof(p->attributes);
    p->attributes.RootDirectory = p->parent;
    p->attributes.ObjectName = &p->unicode;
    p->attributes.Attributes = OBJ_CASE_INSENSITIVE | 0x1000; // OBJ_DONT_REPARSE
    p->status.Status = 0x103;
    constexpr ULONG openReparse = 0x00200000;
    constexpr ULONG synchronous = 0x20;
    const ULONG options = openReparse | synchronous | (directory ? 0x1 : 0x40);
    NTSTATUS status = open(&p->result, GENERIC_READ | SYNCHRONIZE, &p->attributes, &p->status, nullptr,
                           FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, 1, options, nullptr, 0);
    if (status == 0x103) {
        const bool ready = p->result && p->result != INVALID_HANDLE_VALUE &&
                           WaitForSingleObject(p->result, 5000) == WAIT_OBJECT_0 && p->status.Status != 0x103;
        if (!ready) {
            (void)p.release();
            return INVALID_HANDLE_VALUE;
        }
        status = p->status.Status;
    }
    if (status < 0 || !p->result || p->result == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;
    return std::exchange(p->result, nullptr);
}
bool directory(HANDLE h) {
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_CASE_SENSITIVE_INFO sensitive{};
    return GetFileType(h) == FILE_TYPE_DISK && GetFileInformationByHandle(h, &info) &&
           (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
           GetFileInformationByHandleEx(h, FileCaseSensitiveInfo, &sensitive, sizeof(sensitive)) &&
           !(sensitive.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR);
}
bool validPath(std::wstring_view path) {
    if (path.size() < 4 || !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        path[1] != L':' || path[2] != L'\\' || path.back() == L'\\')
        return false;
    std::size_t start = 3;
    while (start < path.size()) {
        const auto end = path.find(L'\\', start);
        const auto part = path.substr(start, (end == path.npos ? path.size() : end) - start);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ' ||
            part.find_first_of(L"/:*?\"<>|") != part.npos)
            return false;
        if (end == path.npos)
            break;
        start = end + 1;
    }
    return true;
}
bool underRoot(std::wstring_view root, std::wstring_view file) {
    return file.size() > root.size() && file[root.size()] == L'\\' &&
           CompareStringOrdinal(root.data(), static_cast<int>(root.size()), file.data(), static_cast<int>(root.size()),
                                TRUE) == CSTR_EQUAL;
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc != 4)
        return 2;
    std::wstring root(argv[1]), path(argv[2]);
    std::replace(root.begin(), root.end(), L'/', L'\\');
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (!validPath(root) || !validPath(path) || !underRoot(root, path))
        return 2;
    wchar_t* end = nullptr;
    const unsigned long limit = std::wcstoul(argv[3], &end, 10);
    if (!end || end == argv[3] || *end || limit > 4 * 1024 * 1024)
        return 2;
    const std::wstring volume = path.substr(0, 3);
    if (GetDriveTypeW(volume.c_str()) != DRIVE_FIXED)
        return 2;
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto open = ntdll ? reinterpret_cast<NtCreateFileFn>(GetProcAddress(ntdll, "NtCreateFile")) : nullptr;
    if (!open)
        return 1;
    Handles held;
    HANDLE parent = CreateFileW(volume.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (parent == INVALID_HANDLE_VALUE)
        return 1;
    held.values.push_back(parent);
    if (!directory(parent))
        return 1;
    std::size_t start = 3;
    for (;;) {
        const auto endPos = path.find(L'\\', start);
        const bool isDirectory = endPos != std::wstring::npos;
        const auto component = std::wstring_view(path).substr(start, (isDirectory ? endPos : path.size()) - start);
        if (!isDirectory) {
            for (HANDLE ancestor : held.values)
                if (!directory(ancestor))
                    return 1;
        }
        parent = openChild(open, parent, component, isDirectory);
        if (parent == INVALID_HANDLE_VALUE)
            return 1;
        held.values.push_back(parent);
        if (isDirectory) {
            if (!directory(parent))
                return 1;
            start = endPos + 1;
            continue;
        }
        break;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(parent) != FILE_TYPE_DISK || !GetFileInformationByHandle(parent, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks != 1)
        return 1;
    const std::uint64_t size = (std::uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
    if (size > limit)
        return 1;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    DWORD read = 0;
    if (size && (!ReadFile(parent, bytes.data(), static_cast<DWORD>(size), &read, nullptr) || read != size))
        return 1;
    DWORD written = 0;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (size && (!WriteFile(output, bytes.data(), static_cast<DWORD>(size), &written, nullptr) || written != size))
        return 1;
    return 0;
}
#endif
