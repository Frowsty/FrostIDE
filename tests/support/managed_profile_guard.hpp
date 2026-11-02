// SPDX-License-Identifier: Apache-2.0
// Guard SDK tests against creating a Windows user-profile NuGet migration marker.
#pragma once
#include <filesystem>
#ifdef _WIN32
#include <windows.h>

#include <shlobj.h>
#endif
namespace frost::test {
inline bool nugetMigrationMarkerPresent(const std::filesystem::path& localAppData) {
    std::error_code error;
    return std::filesystem::is_regular_file(localAppData / "NuGet" / "Migrations" / "1", error) && !error;
}
inline bool sdkTestMayStart() {
#ifdef _WIN32
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &path)) || !path)
        return false;
    const bool present = nugetMigrationMarkerPresent(path);
    CoTaskMemFree(path);
    return present;
#else
    return true;
#endif
}
} // namespace frost::test
