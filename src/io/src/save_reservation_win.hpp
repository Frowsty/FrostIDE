// SPDX-License-Identifier: Apache-2.0
// Cooperative per-path and per-file reservations for Windows saves.
#pragma once

#include "internal.hpp"
#include "save_metadata_win.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace frost::io::detail {

struct SaveFileIdentity {
    DWORD volume = 0;
    DWORD indexHigh = 0;
    DWORD indexLow = 0;
    friend bool operator==(const SaveFileIdentity&, const SaveFileIdentity&) = default;
};

inline bool saveFileIdentity(HANDLE handle, SaveFileIdentity& identity) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info))
        return false;
    identity = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow};
    return true;
}

// Serialize cooperative saves through every alias of an original file. The
// caller must compare its path's identity again after acquiring this mutex.
class SaveFileReservation {
public:
    explicit SaveFileReservation(const SaveFileIdentity& identity)
        : m_mutex(CreateMutexW(nullptr, FALSE,
                               std::format(L"Global\\FrostIDE-save-file-{:08x}-{:08x}-{:08x}", identity.volume,
                                           identity.indexHigh, identity.indexLow)
                                   .c_str())) {
        if (!m_mutex.valid()) {
            m_error = GetLastError();
            return;
        }
        const DWORD result = WaitForSingleObject(m_mutex.get(), 5000);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED)
            m_owned = true;
        else
            m_error = result == WAIT_TIMEOUT ? ERROR_SHARING_VIOLATION : GetLastError();
    }
    ~SaveFileReservation() {
        if (m_owned)
            ReleaseMutex(m_mutex.get());
    }
    SaveFileReservation(const SaveFileReservation&) = delete;
    SaveFileReservation& operator=(const SaveFileReservation&) = delete;
    [[nodiscard]] bool valid() const { return m_owned; }
    [[nodiscard]] DWORD error() const { return m_error; }

private:
    UniqueHandle m_mutex;
    bool m_owned = false;
    DWORD m_error = ERROR_SUCCESS;
};

// Path reservation stays held across publication, when the pathname's file ID
// changes. The caller acquires path first, identity second, and holds both until
// publication or a fail-closed return.
class SavePathReservation {
public:
    explicit SavePathReservation(std::wstring_view path) {
        std::wstring folded(path);
        if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, path.data(), static_cast<int>(path.size()),
                           folded.data(), static_cast<int>(folded.size()), nullptr, nullptr, 0)) {
            m_error = GetLastError();
            return;
        }
        std::uint64_t hash = 14695981039346656037ull;
        for (const wchar_t c : folded) {
            hash ^= static_cast<std::uint16_t>(c);
            hash *= 1099511628211ull;
        }
        m_mutex.reset(CreateMutexW(nullptr, FALSE, std::format(L"Global\\FrostIDE-save-path-{:016x}", hash).c_str()));
        if (!m_mutex.valid()) {
            m_error = GetLastError();
            return;
        }
        const DWORD result = WaitForSingleObject(m_mutex.get(), 5000);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED)
            m_owned = true;
        else
            m_error = result == WAIT_TIMEOUT ? ERROR_SHARING_VIOLATION : GetLastError();
    }
    ~SavePathReservation() {
        if (m_owned)
            ReleaseMutex(m_mutex.get());
    }
    SavePathReservation(const SavePathReservation&) = delete;
    SavePathReservation& operator=(const SavePathReservation&) = delete;
    [[nodiscard]] bool valid() const { return m_owned; }
    [[nodiscard]] DWORD error() const { return m_error; }

private:
    UniqueHandle m_mutex;
    bool m_owned = false;
    DWORD m_error = ERROR_SUCCESS;
};

inline DWORD publishCopy(HANDLE prepared, std::wstring_view target) {
    if (target.size() > (MAXDWORD / sizeof(wchar_t)) - 1)
        return ERROR_FILENAME_EXCED_RANGE;
    const DWORD bytes = static_cast<DWORD>(target.size() * sizeof(wchar_t));
    std::vector<std::byte> storage(offsetof(FILE_RENAME_INFO, FileName) + bytes + sizeof(wchar_t));
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info->RootDirectory = nullptr;
    info->FileNameLength = bytes;
    std::memcpy(info->FileName, target.data(), bytes);
    if (!SetFileInformationByHandle(prepared, FileRenameInfoEx, info, static_cast<DWORD>(storage.size())))
        return GetLastError();
    return ERROR_SUCCESS;
}
} // namespace frost::io::detail
