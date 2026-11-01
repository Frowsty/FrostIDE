// SPDX-License-Identifier: Apache-2.0
//
// Implementation of frost's start-up hardening and checks (see
// frost/app/startup.hpp). Platform specifics are confined to the #ifdef _WIN32 blocks.

#include "frost/app/startup.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <crtdbg.h>
#include <stdlib.h>
#endif

namespace frost::app {
namespace {

// UTF-8 for an error message. Unpaired surrogates become U+FFFD; paths from Win32 can
// contain them, and a message must still be printable.
std::string toUtf8(std::u16string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t c = text[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

constexpr char16_t asciiLower(char16_t c) noexcept {
    return c >= u'A' && c <= u'Z' ? static_cast<char16_t>(c + (u'a' - u'A')) : c;
}

// Whether `text` equals the ASCII string `ascii`, ignoring ASCII case.
bool equalsAsciiNoCase(std::u16string_view text, std::string_view ascii) noexcept {
    if (text.size() != ascii.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (asciiLower(text[i]) != asciiLower(static_cast<unsigned char>(ascii[i]))) {
            return false;
        }
    }
    return true;
}

// Whether `path` is a plugin for one of `platforms` (see evaluatePluginPaths()).
bool isPlatformPlugin(std::u16string_view path, std::span<const std::string> platforms) {
    const std::size_t slash = path.find_last_of(u"\\/");
    if (slash == std::u16string_view::npos || slash == 0) {
        return false;
    }
    const std::size_t parentSlash = path.find_last_of(u"\\/", slash - 1);
    if (parentSlash == std::u16string_view::npos ||
        !equalsAsciiNoCase(path.substr(parentSlash + 1, slash - parentSlash - 1), "platforms")) {
        return false;
    }
    const std::u16string_view file = path.substr(slash + 1);
    for (const std::string& platform : platforms) {
        if (equalsAsciiNoCase(file, "q" + platform + ".dll") || equalsAsciiNoCase(file, "q" + platform + "d.dll")) {
            return true;
        }
    }
    return false;
}

// "qa.dll or qad.dll" for {"a"}; "qa.dll, qad.dll, qb.dll or qbd.dll" for {"a", "b"}.
std::string pluginFileNames(std::span<const std::string> platforms) {
    std::vector<std::string> names;
    for (const std::string& platform : platforms) {
        names.push_back("q" + platform + ".dll");
        names.push_back("q" + platform + "d.dll");
    }
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        out += i == 0 ? "" : (i + 1 == names.size() ? " or " : ", ");
        out += names[i];
    }
    return out;
}

// "\"a\"" for {"a"}; "\"a\", \"b\"" for {"a", "b"}.
std::string quotedNames(std::span<const std::string> platforms) {
    std::string out;
    for (const std::string& platform : platforms) {
        out += std::format("{}\"{}\"", out.empty() ? "" : ", ", platform);
    }
    return out;
}

#ifdef _WIN32

// This executable's path as GetModuleFileNameW reports it (possibly \\?\-prefixed, or with
// 8.3 short components, when the process was started through such a path), or "" on failure.
std::wstring modulePath() {
    // The longest path Windows supports, with its NUL.
    constexpr std::size_t kMaxUnits = 32768;
    std::wstring path(MAX_PATH, L'\0');
    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
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

// `path` without a \\?\ or \\?\UNC\ prefix: the form Qt and other Win32 callers use.
std::wstring withoutExtendedPrefix(std::wstring path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) {
        return L"\\\\" + path.substr(8);
    }
    if (path.starts_with(L"\\\\?\\")) {
        return path.substr(4);
    }
    return path;
}

// The \\?\ form of an absolute path, which Win32 accepts at any length.
std::wstring extendedPath(const std::wstring& path) {
    if (path.starts_with(L"\\\\")) {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

// The value of environment variable `name`, or "" if it is unset or empty.
std::wstring environmentVariable(const wchar_t* name) {
    const DWORD units = GetEnvironmentVariableW(name, nullptr, 0);
    if (units <= 1) {
        return {};
    }
    std::wstring value(units, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), units);
    value.resize(written < units ? written : 0);
    return value;
}

// Calls `onEntry(name, isDirectory)` for each entry matching `pattern` (a \\?\ path with
// a wildcard), skipping "." and "..".
template<typename F>
void forEachEntry(const std::wstring& pattern, const F& onEntry) {
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        const std::wstring_view name = data.cFileName;
        if (name != L"." && name != L"..") {
            onEntry(name, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        }
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
}

// Whether `name` ends in ".dll" (ASCII case-insensitive). A "*.dll" pattern alone also
// matches longer extensions such as ".dllx" through their 8.3 short names.
bool hasDllExtension(std::wstring_view name) noexcept {
    return name.size() > 4 && equalsAsciiNoCase(std::u16string(name.end() - 4, name.end()), ".dll");
}

#endif

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

void suppressCrashDialogs() {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // frost is a GUI-subsystem program, where CRT runtime errors default to a message box.
    _set_error_mode(_OUT_TO_STDERR);
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#ifdef _DEBUG
    // The debug CRT's report hooks; release builds have none (the calls compile to nothing).
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
#endif
}

std::vector<std::string> platformPluginNames(std::string_view qpaPlatform) {
    std::vector<std::string> names;
    while (true) {
        const std::size_t end = qpaPlatform.find(';');
        std::string_view entry = qpaPlatform.substr(0, end);
        entry = entry.substr(0, entry.find(':'));
        if (!entry.empty()) {
            names.emplace_back(entry);
        }
        if (end == std::string_view::npos) {
            break;
        }
        qpaPlatform.remove_prefix(end + 1);
    }
    if (names.empty()) {
        names.emplace_back("windows");
    }
    return names;
}

Result<PluginPathReport> evaluatePluginPaths(std::span<const std::u16string> pluginPaths,
                                             std::span<const std::string> platforms, std::u16string_view dir,
                                             bool requirePresence, bool longPathsEffective) {
    const std::u16string* shortestPlatform = nullptr;
    const std::u16string* longestOther = nullptr;
    std::size_t othersTooLong = 0;
    for (const auto& path : pluginPaths) {
        if (isPlatformPlugin(path, platforms)) {
            if (shortestPlatform == nullptr || path.size() < shortestPlatform->size()) {
                shortestPlatform = &path;
            }
        } else if (!longPathsEffective && path.size() > kQtPluginPathLimitUnits) {
            ++othersTooLong;
            if (longestOther == nullptr || path.size() > longestOther->size()) {
                longestOther = &path;
            }
        }
    }

    PluginPathReport report;
    if (shortestPlatform == nullptr) {
        if (requirePresence) {
            return fail(ErrorCode::NotFound,
                        std::format("Qt's platform plugin {} is missing: {}\\platforms holds no {}. Reinstall "
                                    "FrostIDE or copy the whole installation directory, not only frost.exe.",
                                    quotedNames(platforms), toUtf8(dir), pluginFileNames(platforms)));
        }
        report.detail = std::format("platform plugin {} not checked: QT_PLUGIN_PATH, QT_QPA_PLATFORM_PLUGIN_PATH "
                                    "or qt.conf may supply it",
                                    quotedNames(platforms));
    } else if (!longPathsEffective && shortestPlatform->size() > kQtPluginPathLimitUnits) {
        return fail(ErrorCode::Unsupported,
                    std::format("Qt cannot load its platform plugin {} ({} characters, more than the {} it can "
                                "load a plugin from while long paths are disabled system-wide). Enable "
                                "LongPathsEnabled under HKLM\\SYSTEM\\CurrentControlSet\\Control\\FileSystem "
                                "(needs administrator rights) or start FrostIDE from a shorter directory.",
                                toUtf8(*shortestPlatform), shortestPlatform->size(), kQtPluginPathLimitUnits));
    } else {
        report.detail = std::format("Qt can load its platform plugin {}", toUtf8(*shortestPlatform));
    }
    if (longestOther != nullptr) {
        report.warning =
            std::format("Qt will skip {} plugin(s) longer than the {} characters it can load a plugin "
                        "from while long paths are disabled system-wide, the longest being {} ({} "
                        "characters); FrostIDE starts without them",
                        othersTooLong, kQtPluginPathLimitUnits, toUtf8(*longestOther), longestOther->size());
    }
    return report;
}

Result<PluginPathReport> checkPluginPaths() {
#ifdef _WIN32
    const std::wstring exe = withoutExtendedPrefix(modulePath());
    const std::size_t slash = exe.find_last_of(L'\\');
    if (exe.empty() || slash == std::wstring::npos) {
        return fail(ErrorCode::IoError, "cannot determine the path of frost's executable");
    }
    // Measured as GetModuleFileNameW reports it, short 8.3 components included: Qt derives
    // its plugin paths from the same call (FrostProcess.RunsThroughShortNamesOfALongPath).
    const std::wstring dir = exe.substr(0, slash);
    // Qt looks for plugins in per-type directories below the executable's directory
    // (platforms, styles, ...), so one level is enough.
    std::vector<std::u16string> plugins;
    forEachEntry(extendedPath(dir) + L"\\*", [&](std::wstring_view sub, bool isDirectory) {
        if (!isDirectory) {
            return;
        }
        const std::wstring subdir = dir + L"\\" + std::wstring(sub);
        forEachEntry(extendedPath(subdir) + L"\\*.dll", [&](std::wstring_view file, bool fileIsDirectory) {
            if (!fileIsDirectory && hasDllExtension(file)) {
                const std::wstring full = subdir + L"\\" + std::wstring(file);
                plugins.emplace_back(full.begin(), full.end());
            }
        });
    });
    // Qt also finds plugins through these; with any of them set, it may not need ours.
    const bool redirected = !environmentVariable(L"QT_PLUGIN_PATH").empty() ||
                            !environmentVariable(L"QT_QPA_PLATFORM_PLUGIN_PATH").empty() ||
                            GetFileAttributesW((extendedPath(dir) + L"\\qt.conf").c_str()) != INVALID_FILE_ATTRIBUTES;
    const std::wstring platform = environmentVariable(L"QT_QPA_PLATFORM");
    const std::vector<std::string> platforms =
        platformPluginNames(toUtf8(std::u16string(platform.begin(), platform.end())));
    return evaluatePluginPaths(plugins, platforms, std::u16string(dir.begin(), dir.end()), !redirected,
                               longPathsEffective());
#else
    return PluginPathReport{"not checked on this platform: Qt reports a missing plugin itself", {}};
#endif
}

bool longPathsEffective() noexcept {
#ifdef _WIN32
    // RtlAreLongPathsEnabled needs both the manifest entry and the system-wide
    // LongPathsEnabled value. ntdll exports it on every supported Windows 11 build,
    // but the SDK headers do not declare it.
    using Fn = BOOLEAN(NTAPI*)();
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return false;
    }
    auto* fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlAreLongPathsEnabled")));
    return fn != nullptr && fn() != FALSE;
#else
    return true;
#endif
}

} // namespace frost::app
