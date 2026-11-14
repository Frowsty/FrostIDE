// SPDX-License-Identifier: Apache-2.0
// Scratch candidate for guarded save metadata; caller owns all handles and exclusion.
#pragma once
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <windows.h>

#include <winioctl.h>

namespace frost::io::save_metadata {
struct Metadata {
    bool hasObjectId = false;
    std::array<unsigned char, sizeof(FILE_OBJECTID_BUFFER)> objectId{};
    std::wstring shortName;
};
namespace detail {
inline DWORD sameFile(HANDLE original, const std::wstring& path) {
    HANDLE named =
        CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (named == INVALID_HANDLE_VALUE)
        return GetLastError();
    FILE_ID_INFO a{}, b{};
    DWORD error = ERROR_SUCCESS;
    if (!GetFileInformationByHandleEx(original, FileIdInfo, &a, sizeof(a)) ||
        !GetFileInformationByHandleEx(named, FileIdInfo, &b, sizeof(b)))
        error = GetLastError();
    else if (a.VolumeSerialNumber != b.VolumeSerialNumber ||
             std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) != 0)
        error = ERROR_FILE_INVALID;
    CloseHandle(named);
    return error;
}
} // namespace detail
// The original handle must be synchronous. The path must name that exact file; the caller
// must exclude metadata writers, retain its ancestry/name exclusion and revalidate through use. Identity checks around
// FindFirstFileW detect a persistent replacement, not transient replacement/ABA. This API does
// not establish namespace exclusion. Unsupported object-ID queries are errors, not absence.
// Output is changed only on complete success. Does not create or change an object ID.
inline DWORD queryMetadata(HANDLE original, const std::wstring& path, Metadata& out) {
    if (!original || original == INVALID_HANDLE_VALUE || path.empty() || path.find(L'\0') != path.npos)
        return ERROR_INVALID_PARAMETER;
    const auto prefix = path.starts_with(L"\\\\?\\") ? std::size_t{4} : std::size_t{0};
    if (path.find_first_of(L"*?", prefix) != path.npos)
        return ERROR_INVALID_NAME;
    if (const auto e = detail::sameFile(original, path); e != ERROR_SUCCESS)
        return e;
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileW(path.c_str(), &found);
    if (search == INVALID_HANDLE_VALUE)
        return GetLastError();
    FindClose(search);
    if (const auto e = detail::sameFile(original, path); e != ERROR_SUCCESS)
        return e;
    Metadata result;
    result.shortName = found.cAlternateFileName;
    FILE_OBJECTID_BUFFER object{};
    DWORD count = 0;
    if (DeviceIoControl(original, FSCTL_GET_OBJECT_ID, nullptr, 0, &object, sizeof(object), &count, nullptr)) {
        if (count != sizeof(object))
            return ERROR_INVALID_DATA;
        result.hasObjectId = true;
        std::memcpy(result.objectId.data(), &object, sizeof(object));
    } else {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND)
            return error;
    }
    if (const auto e = detail::sameFile(original, path); e != ERROR_SUCCESS)
        return e;
    out = std::move(result);
    return ERROR_SUCCESS;
}
// targetWithDelete must be opened with DELETE and FILE_FLAG_BACKUP_SEMANTICS. Empty name
// removes a short name; invalid names, collisions and unsupported volumes return Win32 errors.
inline DWORD restoreShortName(HANDLE targetWithDelete, std::wstring_view name) {
    if (!targetWithDelete || targetWithDelete == INVALID_HANDLE_VALUE || name.find(L'\0') != name.npos)
        return ERROR_INVALID_PARAMETER;
    const std::wstring terminated(name);
    if (!SetFileShortNameW(targetWithDelete, terminated.c_str()))
        return GetLastError();
    return ERROR_SUCCESS;
}
} // namespace frost::io::save_metadata
