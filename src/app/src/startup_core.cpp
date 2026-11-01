// SPDX-License-Identifier: Apache-2.0
//
// Implementation of frost/app/startup_core.hpp: process hardening and the launcher's
// install-directory search order. Compiled into frost.exe (static CRT) and into frost::app
// (shared CRT), so it uses only the standard library and Win32 (see the header).

#include "frost/app/startup_core.hpp"

#include <algorithm>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frost::app {
namespace {

// The bit of each ProcessHardening field in encodeHardening()'s set.
constexpr unsigned kDefaultDllDirectoriesBit = 1u << 0;
constexpr unsigned kDllDirectoryClearedBit = 1u << 1;
constexpr unsigned kSafeSearchPathModeBit = 1u << 2;
constexpr unsigned kExeSearchSkipsCurrentDirectoryBit = 1u << 3;
constexpr unsigned kInstallDirectoryExtendedBit = 1u << 4;

} // namespace

bool ProcessHardening::complete() const noexcept {
#ifdef _WIN32
    return defaultDllDirectories && dllDirectoryCleared && safeSearchPathMode && exeSearchSkipsCurrentDirectory;
#else
    return true;
#endif
}

ProcessHardening hardenProcess() {
    ProcessHardening state;
#ifdef _WIN32
    state.defaultDllDirectories = SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) != FALSE;
    state.dllDirectoryCleared = SetDllDirectoryW(L"") != FALSE;
    // Repeating the same permanent mode succeeds. Any other mode after a permanent one fails
    // with ERROR_ACCESS_DENIED, and since PERMANENT can only be combined with safe mode,
    // that failure also means safe mode is in force.
    state.safeSearchPathMode =
        SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT) != FALSE ||
        GetLastError() == ERROR_ACCESS_DENIED;
    // Children inherit the variable; whether user build scripts keep it is decided per
    // launch (the TODO(M2) on ProcessSpec::environment in frost/io/process.hpp).
    SetEnvironmentVariableW(L"NoDefaultCurrentDirectoryInExePath", L"1");
    state.exeSearchSkipsCurrentDirectory = NeedCurrentDirectoryForExePathW(L"frost-probe.exe") == FALSE;
#endif
    return state;
}

unsigned encodeHardening(const ProcessHardening& hardening) noexcept {
    unsigned bits = 0;
    bits |= hardening.defaultDllDirectories ? kDefaultDllDirectoriesBit : 0u;
    bits |= hardening.dllDirectoryCleared ? kDllDirectoryClearedBit : 0u;
    bits |= hardening.safeSearchPathMode ? kSafeSearchPathModeBit : 0u;
    bits |= hardening.exeSearchSkipsCurrentDirectory ? kExeSearchSkipsCurrentDirectoryBit : 0u;
    bits |= hardening.installDirectoryExtended ? kInstallDirectoryExtendedBit : 0u;
    return bits;
}

ProcessHardening decodeHardening(unsigned bits) noexcept {
    ProcessHardening hardening;
    hardening.defaultDllDirectories = (bits & kDefaultDllDirectoriesBit) != 0;
    hardening.dllDirectoryCleared = (bits & kDllDirectoryClearedBit) != 0;
    hardening.safeSearchPathMode = (bits & kSafeSearchPathModeBit) != 0;
    hardening.exeSearchSkipsCurrentDirectory = (bits & kExeSearchSkipsCurrentDirectoryBit) != 0;
    hardening.installDirectoryExtended = (bits & kInstallDirectoryExtendedBit) != 0;
    return hardening;
}

#ifdef _WIN32

std::wstring modulePath(const void* module) {
    // The longest path Windows supports, with its NUL.
    constexpr std::size_t kMaxUnits = 32768;
    // HMODULE is a pointer to the module's base; the header avoids <windows.h>.
    auto* handle = static_cast<HMODULE>(const_cast<void*>(module));
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(handle, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        if (path.size() >= kMaxUnits) {
            return {};
        }
        path.resize(std::min(path.size() * 2, kMaxUnits));
    }
}

std::wstring withoutExtendedPrefix(std::wstring path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) {
        return L"\\\\" + path.substr(8);
    }
    if (path.starts_with(L"\\\\?\\")) {
        return path.substr(4);
    }
    return path;
}

std::wstring extendedPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

bool searchInstallDirectoryExtended(const std::wstring& extendedDirectory) {
    if (AddDllDirectory(extendedDirectory.c_str()) == nullptr) {
        return false;
    }
    // DEFAULT_DIRS is APPLICATION_DIR | SYSTEM32 | USER_DIRS; the \\?\ user directory just added
    // stands in for the plain application directory, which the loader searches first and which
    // ends the search with ERROR_FILENAME_EXCED_RANGE on a long plain start.
    return SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32) != FALSE;
}

#endif

} // namespace frost::app
