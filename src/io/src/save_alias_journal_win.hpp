// SPDX-License-Identifier: Apache-2.0
// Process-kill recovery for an NTFS short name across copy publication.
#pragma once
#include "internal.hpp"
#include "save_metadata_win.hpp"
#include "save_reservation_win.hpp"

#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

namespace frost::io::detail {
struct AliasRecordHeader {
    std::uint32_t magic = 0x46414a31; // FAJ1
    std::uint32_t version = 1;
    SaveFileIdentity original{};
    SaveFileIdentity prepared{};
    std::uint32_t targetUnits = 0;
    std::uint32_t tempUnits = 0;
    std::uint32_t aliasUnits = 0;
};
struct AliasRecord {
    AliasRecordHeader header{};
    std::wstring target, temp, alias;
};
inline std::wstring aliasJournalPath(std::wstring_view target) {
    std::wstring folded(target);
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, target.data(), static_cast<int>(target.size()),
                       folded.data(), static_cast<int>(folded.size()), nullptr, nullptr, 0))
        return {};
    std::uint64_t hash = 14695981039346656037ull;
    for (const wchar_t c : folded) {
        hash ^= static_cast<std::uint16_t>(c);
        hash *= 1099511628211ull;
    }
    const auto slash = target.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos)
        return {};
    return std::wstring(target.substr(0, slash + 1)) + std::format(L".frost-alias-{:016x}.journal", hash);
}
inline DWORD writeAliasRecord(std::wstring_view path, AliasRecord record) {
    if (record.target.empty() || record.target.size() > 32767 || record.temp.empty() || record.temp.size() > 32767 ||
        record.alias.empty() || record.alias.size() > 12)
        return ERROR_INVALID_DATA;
    record.header.targetUnits = static_cast<std::uint32_t>(record.target.size());
    record.header.tempUnits = static_cast<std::uint32_t>(record.temp.size());
    record.header.aliasUnits = static_cast<std::uint32_t>(record.alias.size());
    std::vector<std::byte> bytes(sizeof(record.header) +
                                 (record.target.size() + record.temp.size() + record.alias.size()) * sizeof(wchar_t));
    std::memcpy(bytes.data(), &record.header, sizeof(record.header));
    std::size_t offset = sizeof(record.header);
    for (const auto* value : {&record.target, &record.temp, &record.alias}) {
        const std::size_t size = value->size() * sizeof(wchar_t);
        std::memcpy(bytes.data() + offset, value->data(), size);
        offset += size;
    }
    const std::wstring name(path);
    UniqueHandle file(CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr));
    if (!file.valid())
        return GetLastError();
    DWORD written = 0;
    if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
        return GetLastError();
    if (written != bytes.size())
        return ERROR_WRITE_FAULT;
    if (!FlushFileBuffers(file.get()))
        return GetLastError();
    return ERROR_SUCCESS;
}
inline DWORD readAliasRecord(std::wstring_view path, AliasRecord& record) {
    const std::wstring name(path);
    UniqueHandle file(CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file.valid())
        return GetLastError();
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size))
        return GetLastError();
    constexpr std::size_t maximum = sizeof(AliasRecordHeader) + (32767u * 2u + 12u) * sizeof(wchar_t);
    if (size.QuadPart < static_cast<LONGLONG>(sizeof(AliasRecordHeader)) ||
        size.QuadPart > static_cast<LONGLONG>(maximum))
        return ERROR_INVALID_DATA;
    std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr))
        return GetLastError();
    if (read != bytes.size())
        return ERROR_INVALID_DATA;
    std::memcpy(&record.header, bytes.data(), sizeof(record.header));
    const auto& h = record.header;
    if (h.magic != 0x46414a31 || h.version != 1 || !h.targetUnits || h.targetUnits > 32767 || !h.tempUnits ||
        h.tempUnits > 32767 || !h.aliasUnits || h.aliasUnits > 12 ||
        bytes.size() !=
            sizeof(h) + (static_cast<std::size_t>(h.targetUnits) + h.tempUnits + h.aliasUnits) * sizeof(wchar_t))
        return ERROR_INVALID_DATA;
    std::size_t offset = sizeof(h);
    auto string = [&](std::wstring& value, std::size_t count) {
        value.resize(count);
        std::memcpy(value.data(), bytes.data() + offset, count * sizeof(wchar_t));
        offset += count * sizeof(wchar_t);
    };
    string(record.target, h.targetUnits);
    string(record.temp, h.tempUnits);
    string(record.alias, h.aliasUnits);
    if (record.target.find(L'\0') != std::wstring::npos || record.temp.find(L'\0') != std::wstring::npos ||
        record.alias.find(L'\0') != std::wstring::npos || record.alias.find_first_of(L"\\/:") != std::wstring::npos)
        return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}
inline bool validAliasTempName(std::wstring_view name) {
    constexpr std::wstring_view marker = L".frost-tmp-";
    const auto at = name.rfind(marker);
    if (name.empty() || name.front() != L'.' || at == name.npos || at < 2)
        return false;
    const auto numbers = name.substr(at + marker.size());
    const auto dash = numbers.find(L'-');
    if (dash == 0 || dash == numbers.npos || dash > 10 || dash + 1 == numbers.size() || numbers.size() - dash - 1 > 20)
        return false;
    for (std::size_t i = 0; i < numbers.size(); ++i)
        if (i != dash && (numbers[i] < L'0' || numbers[i] > L'9'))
            return false;
    return true;
}
inline DWORD recoverAliasRecord(std::wstring_view target, const SaveFileIdentity& current) {
    const std::wstring journal = aliasJournalPath(target);
    if (journal.empty())
        return ERROR_INVALID_DATA;
    AliasRecord record;
    const DWORD error = readAliasRecord(journal, record);
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
        return ERROR_SUCCESS;
    if (error != ERROR_SUCCESS)
        return error;
    if (record.header.original == record.header.prepared)
        return ERROR_INVALID_DATA;
    if (_wcsicmp(std::wstring(target).c_str(), record.target.c_str()) != 0)
        return ERROR_INVALID_DATA;
    // Temp must be a sibling with the existing private temp naming grammar;
    // identity is checked before deletion and absent is safe before publication.
    const auto targetSlash = target.find_last_of(L"\\/");
    const auto tempSlash = record.temp.find_last_of(L"\\/");
    if (targetSlash == std::wstring_view::npos || tempSlash == std::wstring::npos || targetSlash != tempSlash ||
        // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): comparison consumes the bounded parent length.
        _wcsnicmp(target.data(), record.temp.c_str(), targetSlash + 1) != 0 ||
        !validAliasTempName(std::wstring_view(record.temp).substr(tempSlash + 1)))
        return ERROR_INVALID_DATA;
    if (current == record.header.original) {
        UniqueHandle temp(CreateFileW(record.temp.c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (temp.valid()) {
            SaveFileIdentity identity{};
            if (!saveFileIdentity(temp.get(), identity) || !(identity == record.header.prepared))
                return ERROR_INVALID_DATA;
            temp.reset();
            if (!DeleteFileW(record.temp.c_str()))
                return GetLastError();
        } else {
            const DWORD e = GetLastError();
            if (e != ERROR_FILE_NOT_FOUND)
                return e;
        }
    } else if (current == record.header.prepared) {
        UniqueHandle file(CreateFileW(record.target.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (!file.valid())
            return GetLastError();
        SaveFileIdentity identity{};
        if (!saveFileIdentity(file.get(), identity) || !(identity == current))
            return ERROR_INVALID_DATA;
        if (const DWORD e = save_metadata::restoreShortName(file.get(), record.alias); e != ERROR_SUCCESS)
            return e;
    } else
        return ERROR_INVALID_DATA;
    if (!DeleteFileW(journal.c_str()))
        return GetLastError();
    return ERROR_SUCCESS;
}
// A killed publish can temporarily remove the old 8.3 spelling. Before a
// missing spelling is created, inspect owned journals in the canonical parent.
inline DWORD findMissingAliasRecord(std::wstring_view directory, std::wstring_view missingName,
                                    std::wstring& conflict) {
    if (directory.empty() || missingName.empty() || missingName.find_first_of(L"\\/:") != missingName.npos)
        return ERROR_INVALID_NAME;
    std::wstring dir(directory);
    if (dir.back() != L'\\')
        dir.push_back(L'\\');
    const std::wstring pattern = dir + L".frost-alias-*.journal";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : e;
    }
    DWORD result = ERROR_SUCCESS;
    std::size_t scanned = 0;
    constexpr std::size_t kMaxAliasRecords = 1024;
    do {
        if (++scanned > kMaxAliasRecords) {
            result = ERROR_BUFFER_OVERFLOW;
            break;
        }
        const std::wstring_view name(data.cFileName);
        constexpr std::wstring_view prefix = L".frost-alias-", suffix = L".journal";
        if (name.size() != prefix.size() + 16 + suffix.size() || !name.starts_with(prefix) || !name.ends_with(suffix))
            continue;
        bool valid = true;
        for (wchar_t c : name.substr(prefix.size(), 16))
            if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) {
                valid = false;
                break;
            }
        if (!valid)
            continue;
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            result = ERROR_INVALID_DATA;
            break;
        }
        const std::wstring path = dir + std::wstring(name);
        AliasRecord record;
        if (const DWORD e = readAliasRecord(path, record); e != ERROR_SUCCESS) {
            result = e;
            break;
        }
        const auto slash = record.target.find_last_of(L"\\/");
        if (slash == std::wstring::npos || slash + 1 != dir.size() ||
            _wcsnicmp(record.target.c_str(), dir.c_str(), dir.size()) != 0 || aliasJournalPath(record.target) != path) {
            result = ERROR_INVALID_DATA;
            break;
        }
        if (_wcsicmp(record.alias.c_str(), std::wstring(missingName).c_str()) == 0) {
            conflict = path;
            break;
        }
    } while (FindNextFileW(find, &data));
    if (result == ERROR_SUCCESS && conflict.empty()) {
        const DWORD e = GetLastError();
        if (e != ERROR_NO_MORE_FILES)
            result = e;
    }
    FindClose(find);
    return result;
}
} // namespace frost::io::detail
