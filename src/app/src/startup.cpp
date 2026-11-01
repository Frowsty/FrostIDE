// SPDX-License-Identifier: Apache-2.0
//
// Implementation of frost's start-up guards and plugin pinning (see frost/app/startup.hpp).
// The hardening itself is in startup_core.cpp. Platform specifics are confined to the
// #ifdef _WIN32 blocks.

#include "frost/app/startup.hpp"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
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

// The handler installCommandLineFatalHandler() replaced, and the code page it restores. Written
// on the main thread before QApplication exists; read by whichever thread reports a message.
constinit std::atomic<QtMessageHandler> previousMessageHandler{nullptr};
constinit std::atomic<const unsigned*> fatalCodePageToRestore{nullptr};
constinit std::atomic<bool> fatalHandlerInstalled{false};

void commandLineMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (type == QtFatalMsg) {
        const QByteArray text = message.toUtf8();
        std::fprintf(stderr, "frost: Qt fatal error: %s\n", text.constData());
        std::fflush(stdout);
        std::fflush(stderr);
#ifdef _WIN32
        if (const unsigned* codePage = fatalCodePageToRestore.load(); codePage != nullptr && *codePage != 0) {
            SetConsoleOutputCP(*codePage);
        }
#endif
        std::_Exit(4);
    }
    if (QtMessageHandler previous = previousMessageHandler.load(); previous != nullptr) {
        previous(type, context, message);
    }
}

#ifdef _WIN32

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

std::u16string toU16(std::wstring_view text) { return {text.begin(), text.end()}; }

// FormatMessageW's text for a Win32 error in UTF-8, without the trailing line break.
std::string win32Message(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD units =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring text = units > 0 && buffer != nullptr ? std::wstring(buffer, units) : std::wstring(L"unknown error");
    LocalFree(buffer);
    while (!text.empty() &&
           (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ' || text.back() == L'.')) {
        text.pop_back();
    }
    return toUtf8(toU16(text));
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
    return name.size() > 4 && equalsAsciiNoCase(toU16(name.substr(name.size() - 4)), ".dll");
}

// The install directory (frost.exe's) in the form GetModuleFileNameW reports, without a
// \\?\ prefix, or "" if it cannot be determined.
std::wstring installDirectory() {
    const std::wstring exe = withoutExtendedPrefix(modulePath());
    const std::size_t slash = exe.find_last_of(L'\\');
    return exe.empty() || slash == std::wstring::npos ? std::wstring() : exe.substr(0, slash);
}

// `path` (absolute) with "." and ".." components resolved, as Win32 resolves them in a plain
// path; the \\?\ form would take them literally. `path` itself if GetFullPathNameW fails.
std::wstring resolvedPath(const std::wstring& path) {
    const DWORD units = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (units == 0) {
        return path;
    }
    std::wstring out(units, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), units, out.data(), nullptr);
    if (written == 0 || written >= units) {
        return path;
    }
    out.resize(written);
    return out;
}

// The platform of `platforms` whose plugin `candidate` is (q<p>.dll or q<p>d.dll), or "".
std::string platformOf(std::u16string_view candidate, std::span<const std::string> platforms) {
    const std::size_t slash = candidate.find_last_of(u"\\/");
    const std::u16string_view file = slash == std::u16string_view::npos ? candidate : candidate.substr(slash + 1);
    for (const std::string& platform : platforms) {
        if (equalsAsciiNoCase(file, "q" + platform + ".dll") || equalsAsciiNoCase(file, "q" + platform + "d.dll")) {
            return platform;
        }
    }
    return {};
}

// The search flags for plugins (see loadPlatformPlugin()).
constexpr DWORD kPluginLoadFlags = LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32;

// The WH_CBT hook of guardCommandLineDialogs(): refuses dialog windows.
LRESULT CALLBACK refuseDialogs(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HCBT_CREATEWND) {
        auto* window = reinterpret_cast<HWND>(wParam);
        wchar_t className[16] = {};
        if (GetClassNameW(window, className, 16) > 0 && std::wstring_view(className) == L"#32770") {
            const auto* create = reinterpret_cast<const CBT_CREATEWNDW*>(lParam);
            std::wstring title;
            if (create != nullptr && create->lpcs != nullptr && create->lpcs->lpszName != nullptr &&
                !IS_INTRESOURCE(create->lpcs->lpszName)) {
                title = create->lpcs->lpszName;
            }
            const std::string text = toUtf8(toU16(title));
            std::fprintf(stderr, "frost: refused to show a dialog in a command-line mode: %s\n", text.c_str());
            std::fflush(stderr);
            // A non-zero result fails the creation, so MessageBoxW returns 0 without waiting.
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

#endif

} // namespace

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

bool guardCommandLineDialogs() {
#ifdef _WIN32
    return SetWindowsHookExW(WH_CBT, refuseDialogs, nullptr, GetCurrentThreadId()) != nullptr;
#else
    return true;
#endif
}

void installCommandLineFatalHandler(const unsigned* consoleCodePageToRestore) {
    fatalCodePageToRestore.store(consoleCodePageToRestore);
    if (!fatalHandlerInstalled.exchange(true)) {
        // Qt returns its default handler when none was installed, so `previous` is never null.
        previousMessageHandler.store(qInstallMessageHandler(commandLineMessageHandler));
    }
}

bool pluginPathExceedsLoaderLimit(std::size_t dirUnits, std::size_t longestRelativeUnits) noexcept {
    return dirUnits + 1 + longestRelativeUnits > kLoaderPlainPathLimitUnits;
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

std::vector<std::u16string> platformPluginCandidates(std::span<const std::u16string> dirs,
                                                     std::span<const std::string> platforms, bool debugQt) {
    std::vector<std::u16string> candidates;
    candidates.reserve(dirs.size() * platforms.size());
    for (const std::string& platform : platforms) {
        // Platform names are ASCII (Qt's plugin keys), so widening byte by byte is exact.
        const std::string file = "q" + platform + (debugQt ? "d.dll" : ".dll");
        for (const std::u16string& dir : dirs) {
            std::u16string candidate = dir;
            candidate += u'\\';
            candidate.append(file.begin(), file.end());
            candidates.push_back(std::move(candidate));
        }
    }
    return candidates;
}

std::string missingPlatformPluginMessage(std::span<const std::string> platforms, std::u16string_view dir) {
    return std::format("Qt's platform plugin {} is missing: {}\\platforms holds no {}. Reinstall FrostIDE or copy "
                       "the whole installation directory, not only frost.exe.",
                       quotedNames(platforms), toUtf8(dir), pluginFileNames(platforms));
}

Result<std::u16string> loadPlatformPlugin(std::span<const std::u16string> candidates,
                                          std::span<const std::string> platforms) {
#ifdef _WIN32
    for (const std::u16string& candidate : candidates) {
        const std::wstring extended = extendedPath(std::wstring(candidate.begin(), candidate.end()));
        if (GetFileAttributesW(extended.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        if (LoadLibraryExW(extended.c_str(), nullptr, kPluginLoadFlags) != nullptr) {
            return candidate;
        }
        const DWORD error = GetLastError();
        return fail(ErrorCode::IoError,
                    std::format("Qt's platform plugin \"{}\" cannot be loaded from {}: {} (win32 error {})",
                                platformOf(candidate, platforms), toUtf8(candidate), win32Message(error), error));
    }
    return fail(ErrorCode::NotFound, std::format("no plugin for Qt's platform {} exists", quotedNames(platforms)));
#else
    static_cast<void>(candidates);
    static_cast<void>(platforms);
    return fail(ErrorCode::Unsupported, "frost pins Qt's platform plugin on Windows only");
#endif
}

Result<PlatformPluginReport> preparePlatformPlugin() {
#ifdef _WIN32
    const std::wstring dir = installDirectory();
    if (dir.empty()) {
        return fail(ErrorCode::IoError, "cannot determine the path of frost's executable");
    }
    const std::vector<std::string> platforms =
        platformPluginNames(toUtf8(toU16(environmentVariable(L"QT_QPA_PLATFORM"))));
    // Qt loads only plugins built like itself; the debug build's DLL names end in "d".
    const bool debugQt = GetModuleHandleW(L"Qt6Cored.dll") != nullptr;
    // Qt's order: QT_QPA_PLATFORM_PLUGIN_PATH first, then <library path>\platforms.
    std::vector<std::u16string> dirs;
    std::wstring custom = environmentVariable(L"QT_QPA_PLATFORM_PLUGIN_PATH");
    std::replace(custom.begin(), custom.end(), L'/', L'\\');
    if (custom.starts_with(L"\\\\?\\")) {
        custom = withoutExtendedPrefix(custom);
    }
    while (custom.size() > 3 && custom.back() == L'\\') {
        custom.pop_back();
    }
    const bool absolute = (custom.size() >= 3 && custom[1] == L':' && custom[2] == L'\\') ||
                          (custom.size() > 2 && custom.starts_with(L"\\\\"));
    if (absolute) {
        // The pin goes through \\?\, which takes "." and ".." literally where Qt resolves them.
        dirs.push_back(toU16(custom.find(L"\\.") != std::wstring::npos ? resolvedPath(custom) : custom));
    }
    dirs.push_back(toU16(dir + L"\\platforms"));
    const std::vector<std::u16string> candidates = platformPluginCandidates(dirs, platforms, debugQt);
    auto loaded = loadPlatformPlugin(candidates, platforms);
    if (loaded) {
        return PlatformPluginReport{"loaded Qt's platform plugin " + toUtf8(*loaded)};
    }
    if (loaded.error().code() != ErrorCode::NotFound) {
        return std::unexpected(std::move(loaded.error()));
    }
    // Qt also finds plugins through these; with any of them set, it may not need ours.
    const bool redirected = !environmentVariable(L"QT_PLUGIN_PATH").empty() ||
                            !environmentVariable(L"QT_QPA_PLATFORM_PLUGIN_PATH").empty() ||
                            GetFileAttributesW((extendedPath(dir) + L"\\qt.conf").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (redirected) {
        return PlatformPluginReport{std::format("platform plugin {} not checked: QT_PLUGIN_PATH, "
                                                "QT_QPA_PLATFORM_PLUGIN_PATH or qt.conf may supply it",
                                                quotedNames(platforms))};
    }
    return fail(ErrorCode::NotFound, missingPlatformPluginMessage(platforms, toU16(dir)));
#else
    return PlatformPluginReport{"not checked on this platform: Qt reports a missing plugin itself"};
#endif
}

PluginPreloadReport preloadOtherPlugins() {
#ifdef _WIN32
    const std::wstring dir = installDirectory();
    if (dir.empty()) {
        return {false, "cannot determine the path of frost's executable"};
    }
    const std::wstring extended = extendedPath(dir);
    // Qt looks for plugins in per-type directories below the install directory (styles,
    // imageformats, ...); the platform plugin is preparePlatformPlugin()'s.
    std::vector<std::wstring> plugins;
    std::size_t longest = 0;
    forEachEntry(extended + L"\\*", [&](std::wstring_view sub, bool isDirectory) {
        if (!isDirectory || equalsAsciiNoCase(toU16(sub), "platforms")) {
            return;
        }
        const std::wstring type(sub);
        forEachEntry(extended + L"\\" + type + L"\\*.dll", [&](std::wstring_view file, bool fileIsDirectory) {
            if (!fileIsDirectory && hasDllExtension(file)) {
                plugins.push_back(type + L"\\" + std::wstring(file));
                longest = std::max(longest, plugins.back().size());
            }
        });
    });
    if (plugins.empty()) {
        return {true, "no other plugins are deployed"};
    }
    const std::size_t longestPath = dir.size() + 1 + longest;
    if (!pluginPathExceedsLoaderLimit(dir.size(), longest)) {
        return {true, std::format("not needed: {} plugin(s), the longest path {} UTF-16 units, within the "
                                  "loader's {}-unit limit",
                                  plugins.size(), longestPath, kLoaderPlainPathLimitUnits)};
    }
    std::size_t loaded = 0;
    std::string failures;
    for (const std::wstring& plugin : plugins) {
        if (LoadLibraryExW((extended + L"\\" + plugin).c_str(), nullptr, kPluginLoadFlags) != nullptr) {
            ++loaded;
        } else {
            const DWORD error = GetLastError();
            failures += std::format("{}{} ({}, win32 error {})", failures.empty() ? "" : ", ", toUtf8(toU16(plugin)),
                                    win32Message(error), error);
        }
    }
    return {loaded == plugins.size(),
            std::format("loaded {} of {} plugin(s) through \\\\?\\ (the longest path is {} UTF-16 units, beyond the "
                        "loader's {}-unit limit){}{}",
                        loaded, plugins.size(), longestPath, kLoaderPlainPathLimitUnits,
                        failures.empty() ? "" : "; Qt will skip ", failures)};
#else
    return {true, "not needed on this platform"};
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
