// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost's start-up hardening and checks (frost/app/startup.hpp): which platform
// plugin Qt will use and the path-length rule for loading it, that the hardened process
// no longer loads a DLL planted in the current directory (and what it does not cover),
// and that suppressCrashDialogs() switches every dialog source off. A separate binary
// from app_tests, because hardenProcess() changes process-wide state for good.

#include "frost/app/startup.hpp"
#include "frost/io/path.hpp"

#include "support/scratch_dir.hpp"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <crtdbg.h>
#include <stdlib.h>
#endif

namespace {

using frost::app::evaluatePluginPaths;
using frost::app::kQtPluginPathLimitUnits;
using frost::app::platformPluginNames;

const std::vector<std::string> kWindows{"windows"};

// An absolute Windows-style path of exactly `units` UTF-16 code units ending in `tail`.
std::u16string pathOfLength(std::size_t units, std::u16string_view tail) {
    std::u16string path = u"C:\\";
    path.append(units - path.size() - tail.size(), u'p');
    path += tail;
    return path;
}

// A path of exactly `units` units to the plugin `file` in a "platforms" directory.
std::u16string platformPluginOfLength(std::size_t units, std::u16string_view file = u"qwindows.dll") {
    return pathOfLength(units, std::u16string(u"\\platforms\\") + std::u16string(file));
}

TEST(StartupPlatformNames, DefaultsToWindows) {
    EXPECT_THAT(platformPluginNames(""), testing::ElementsAre("windows"));
    EXPECT_THAT(platformPluginNames(";:x;"), testing::ElementsAre("windows"));
}

TEST(StartupPlatformNames, SplitsFallbacksAndDropsOptions) {
    EXPECT_THAT(platformPluginNames("offscreen"), testing::ElementsAre("offscreen"));
    EXPECT_THAT(platformPluginNames("windows:darkmode=2,fontengine=freetype"), testing::ElementsAre("windows"));
    EXPECT_THAT(platformPluginNames("direct2d;;windows:nodirectwrite"), testing::ElementsAre("direct2d", "windows"));
}

TEST(StartupPluginPaths, AcceptsThePlatformPluginUpToTheLimit) {
    const std::vector<std::u16string> paths{platformPluginOfLength(40),
                                            platformPluginOfLength(kQtPluginPathLimitUnits)};
    const auto report = evaluatePluginPaths(paths, kWindows, u"C:\\frost", true, false);
    ASSERT_TRUE(report.has_value()) << report.error().message();
    EXPECT_THAT(report->detail, testing::StartsWith("Qt can load its platform plugin C:\\pp"));
    EXPECT_EQ(report->warning, "");
}

TEST(StartupPluginPaths, RejectsOneUnitBeyondTheLimitAndNamesTheShortestCandidate) {
    // Release and debug builds of the plugin, both too long: the message names the
    // shortest, which tells how much shorter the directory must be.
    const std::vector<std::u16string> paths{platformPluginOfLength(kQtPluginPathLimitUnits + 5, u"qwindowsd.dll"),
                                            platformPluginOfLength(kQtPluginPathLimitUnits + 1, u"qwindows.dll")};
    const auto result = evaluatePluginPaths(paths, kWindows, u"C:\\frost", true, false);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), frost::ErrorCode::Unsupported);
    EXPECT_THAT(result.error().message(), testing::HasSubstr("platforms\\qwindows.dll (251 characters"));
    EXPECT_THAT(result.error().message(), testing::HasSubstr("more than the 250"));
    EXPECT_THAT(result.error().message(), testing::HasSubstr("LongPathsEnabled"));
}

TEST(StartupPluginPaths, OneCandidateWithinTheLimitIsEnough) {
    // QT_QPA_PLATFORM="direct2d;windows": Qt falls back to the second one.
    const std::vector<std::string> platforms{"direct2d", "windows"};
    const std::vector<std::u16string> paths{platformPluginOfLength(kQtPluginPathLimitUnits + 1, u"qdirect2d.dll"),
                                            platformPluginOfLength(kQtPluginPathLimitUnits, u"qwindows.dll")};
    EXPECT_TRUE(evaluatePluginPaths(paths, platforms, u"C:\\frost", true, false).has_value());
}

TEST(StartupPluginPaths, IgnoresLengthWhenLongPathsAreEffective) {
    const std::vector<std::u16string> paths{platformPluginOfLength(1000),
                                            pathOfLength(1000, u"\\imageformats\\qjpeg.dll")};
    const auto report = evaluatePluginPaths(paths, kWindows, u"C:\\frost", true, true);
    ASSERT_TRUE(report.has_value()) << report.error().message();
    EXPECT_EQ(report->warning, "");
}

TEST(StartupPluginPaths, OtherPluginsBeyondTheLimitOnlyWarn) {
    // A long image-format plugin, and the offscreen plugin while Qt will use windows: Qt
    // starts without them, so they must not stop frost.
    const std::vector<std::u16string> paths{platformPluginOfLength(200),
                                            platformPluginOfLength(kQtPluginPathLimitUnits + 3, u"qoffscreen.dll"),
                                            pathOfLength(kQtPluginPathLimitUnits + 9, u"\\imageformats\\qjpeg.dll"),
                                            pathOfLength(kQtPluginPathLimitUnits, u"\\styles\\qmodern.dll")};
    const auto report = evaluatePluginPaths(paths, kWindows, u"C:\\frost", true, false);
    ASSERT_TRUE(report.has_value()) << report.error().message();
    EXPECT_THAT(report->warning, testing::HasSubstr("skip 2 plugin(s)"));
    EXPECT_THAT(report->warning, testing::HasSubstr("imageformats\\qjpeg.dll (259 characters"));
}

TEST(StartupPluginPaths, CountsUtf16UnitsAndReportsUtf8) {
    // U+1F600 is two UTF-16 code units and four UTF-8 bytes.
    const std::u16string fits = pathOfLength(kQtPluginPathLimitUnits, u"\U0001F600\\platforms\\qwindows.dll");
    EXPECT_TRUE(evaluatePluginPaths(std::vector{fits}, kWindows, u"C:\\frost", true, false).has_value());
    const std::u16string tooLong =
        pathOfLength(kQtPluginPathLimitUnits + 1, u"\u00fc\U0001F600\\platforms\\qwindows.dll");
    const auto result = evaluatePluginPaths(std::vector{tooLong}, kWindows, u"C:\\frost", true, false);
    ASSERT_FALSE(result.has_value());
    EXPECT_THAT(result.error().message(),
                testing::HasSubstr("\xC3\xBC\xF0\x9F\x98\x80\\platforms\\qwindows.dll (251 characters"));
}

TEST(StartupPluginPaths, ReplacesUnpairedSurrogatesInTheMessage) {
    std::u16string path = platformPluginOfLength(kQtPluginPathLimitUnits + 1);
    path[3] = u'\xD800';
    const auto result = evaluatePluginPaths(std::vector{path}, kWindows, u"C:\\frost", true, false);
    ASSERT_FALSE(result.has_value());
    EXPECT_THAT(result.error().message(), testing::StartsWith("Qt cannot load its platform plugin C:\\\xEF\xBF\xBDp"));
}

TEST(StartupPlatformPlugin, FoundInPlatformsDirectoryCaseInsensitively) {
    const std::vector<std::u16string> windows{u"C:\\frost\\styles\\qmodernwindowsstyle.dll",
                                              u"C:\\frost\\platforms\\qwindows.dll"};
    EXPECT_TRUE(evaluatePluginPaths(windows, kWindows, u"C:\\frost", true, false).has_value());
    const std::vector<std::u16string> upper{u"C:\\frost\\Platforms\\QOffscreenD.DLL"};
    EXPECT_TRUE(
        evaluatePluginPaths(upper, std::vector<std::string>{"offscreen"}, u"C:\\frost", true, false).has_value());
    const std::vector<std::u16string> slashes{u"C:/frost/platforms/qwindowsd.dll"};
    EXPECT_TRUE(evaluatePluginPaths(slashes, kWindows, u"C:/frost", true, false).has_value());
}

TEST(StartupPlatformPlugin, MissingWhenNoPluginForThePlatformQtWillUse) {
    // Lookalikes: a directory whose name only ends in "platforms", a DLL named after it,
    // a path too short to hold the directory, and the plugin of another platform.
    const std::vector<std::u16string> paths{u"C:\\frost\\styles\\qmodernwindowsstyle.dll",
                                            u"C:\\frost\\myplatforms\\qoffscreen.dll",
                                            u"C:\\frost\\imageformats\\platforms.dll",
                                            u"q.dll",
                                            u"platforms\\qoffscreen.dll",
                                            u"C:\\frost\\platforms\\qwindows.dll",
                                            u"C:\\frost\\platforms\\qoffscreenx.dll"};
    const auto result =
        evaluatePluginPaths(paths, std::vector<std::string>{"offscreen"}, u"C:\\fr\u00f8st", true, false);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), frost::ErrorCode::NotFound);
    EXPECT_THAT(result.error().message(),
                testing::HasSubstr("Qt's platform plugin \"offscreen\" is missing: C:\\fr\xC3\xB8st\\platforms "
                                   "holds no qoffscreen.dll or qoffscreend.dll"));
    const auto none = evaluatePluginPaths({}, std::vector<std::string>{"a", "b"}, u"C:\\frost", true, false);
    ASSERT_FALSE(none.has_value());
    EXPECT_THAT(none.error().message(), testing::HasSubstr("\"a\", \"b\" is missing"));
    EXPECT_THAT(none.error().message(), testing::HasSubstr("holds no qa.dll, qad.dll, qb.dll or qbd.dll"));
}

TEST(StartupPlatformPlugin, PresenceNotRequiredButLengthStillChecked) {
    // QT_PLUGIN_PATH and friends may supply the plugin, so its absence is fine...
    const auto absent = evaluatePluginPaths({}, kWindows, u"C:\\frost", false, false);
    ASSERT_TRUE(absent.has_value());
    EXPECT_THAT(absent->detail, testing::HasSubstr("not checked"));
    // ...but the one next to the executable is still measured.
    const std::vector<std::u16string> paths{platformPluginOfLength(kQtPluginPathLimitUnits + 1)};
    const auto tooLong = evaluatePluginPaths(paths, kWindows, u"C:\\frost", false, false);
    ASSERT_FALSE(tooLong.has_value());
    EXPECT_EQ(tooLong.error().code(), frost::ErrorCode::Unsupported);
}

#ifdef _WIN32

// Sets environment variable `name` for one test and restores its previous state.
class ScopedEnvironment {
public:
    ScopedEnvironment(const wchar_t* name, const wchar_t* value) : m_name(name) {
        const DWORD units = GetEnvironmentVariableW(name, nullptr, 0);
        if (units > 0) {
            m_previous.resize(units);
            m_previous.resize(GetEnvironmentVariableW(name, m_previous.data(), units));
            m_wasSet = true;
        }
        EXPECT_NE(SetEnvironmentVariableW(name, value), FALSE);
    }
    ScopedEnvironment(const ScopedEnvironment&) = delete ("restores exactly once");
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete ("restores exactly once");
    ~ScopedEnvironment() { SetEnvironmentVariableW(m_name, m_wasSet ? m_previous.c_str() : nullptr); }

private:
    const wchar_t* m_name;
    std::wstring m_previous;
    bool m_wasSet = false;
};

TEST(StartupPluginPaths, ReportsTheMissingPluginForQtQpaPlatform) {
    // Test binaries have no platforms/ next to them (only frost.exe's directory gets one).
    const ScopedEnvironment pluginPath(L"QT_PLUGIN_PATH", nullptr);
    const ScopedEnvironment platformPath(L"QT_QPA_PLATFORM_PLUGIN_PATH", nullptr);
    const ScopedEnvironment platform(L"QT_QPA_PLATFORM", L"offscreen:fontengine=freetype");
    const auto result = frost::app::checkPluginPaths();
    ASSERT_FALSE(result.has_value()) << result->detail;
    EXPECT_EQ(result.error().code(), frost::ErrorCode::NotFound);
    EXPECT_THAT(result.error().message(), testing::HasSubstr("\"offscreen\" is missing"));
}

#endif

TEST(StartupPluginPaths, PresenceCheckDefersToQtPluginPath) {
#ifdef _WIN32
    // QT_PLUGIN_PATH tells Qt to look elsewhere, so the presence check stands down.
    const ScopedEnvironment pluginPath(L"QT_PLUGIN_PATH", L"C:\\frost-elsewhere");
#endif
    // The directory is far below the limit; on Linux the check always passes.
    const auto result = frost::app::checkPluginPaths();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_THAT(result->detail, testing::HasSubstr("not checked"));
}

#ifdef _WIN32

/*
 * The planted-binary checks, run in a fresh child process (see the test below). Prints
 * every failed check to stderr and returns the child's exit status: 0 when all passed.
 * Everything the child creates is removed before it returns, because the child ends
 * with std::exit and never unwinds the test's stack.
 */
int plantedBinaryChecks() {
    int failures = 0;
    const auto check = [&failures](bool ok, const char* what, DWORD error) {
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "check failed: %s (last error %lu)\n", what, error);
        }
    };
    std::wstring original(GetCurrentDirectoryW(0, nullptr), L'\0');
    original.resize(GetCurrentDirectoryW(static_cast<DWORD>(original.size()), original.data()));
    {
        const frost::test::ScratchDir scratch("app-startup-planted");
        // Any valid DLL serves as the planted one; version.dll is small and has no state.
        wchar_t system[MAX_PATH];
        if (GetSystemDirectoryW(system, MAX_PATH) == 0) {
            std::fprintf(stderr, "GetSystemDirectoryW failed\n");
            return 1;
        }
        const std::filesystem::path dir = frost::io::toStdPath(scratch.path()).value();
        const std::filesystem::path plantedDll = dir / L"frost_planted_probe.dll";
        const std::filesystem::path plantedTool = dir / L"frost-planted-tool.exe";
        std::error_code error;
        std::filesystem::copy_file(std::filesystem::path(system) / L"version.dll", plantedDll, error);
        if (!error) {
            std::filesystem::copy_file(plantedDll, plantedTool, error);
        }
        if (error || SetCurrentDirectoryW(dir.c_str()) == FALSE) {
            std::fprintf(stderr, "setup failed: %s\n", error.message().c_str());
            return 1;
        }
        // Some parents (Claude Code's shells, for one) already export the variable; the
        // control half needs Windows' default.
        SetEnvironmentVariableW(L"NoDefaultCurrentDirectoryInExePath", nullptr);

        // Control: before hardening, both searches use the current directory.
        HMODULE before = LoadLibraryW(L"frost_planted_probe.dll");
        check(before != nullptr, "the unhardened loader finds the planted DLL (else this test proves nothing)",
              GetLastError());
        if (before != nullptr) {
            FreeLibrary(before);
        }
        check(NeedCurrentDirectoryForExePathW(L"frost-probe.exe") != FALSE,
              "unhardened, executable names resolve in the current directory", GetLastError());

        const frost::app::ProcessHardening hardening = frost::app::hardenProcess();
        check(hardening.defaultDllDirectories, "SetDefaultDllDirectories applied", 0);
        check(hardening.dllDirectoryCleared, "SetDllDirectoryW applied", 0);
        check(hardening.safeSearchPathMode, "SetSearchPathMode applied", 0);
        check(hardening.exeSearchSkipsCurrentDirectory, "NoDefaultCurrentDirectoryInExePath applied", 0);

        HMODULE byName = LoadLibraryW(L"frost_planted_probe.dll");
        check(byName == nullptr && GetLastError() == static_cast<DWORD>(ERROR_MOD_NOT_FOUND),
              "hardened LoadLibraryW no longer finds the planted DLL", GetLastError());
        HMODULE byNameEx = LoadLibraryExW(L"frost_planted_probe.dll", nullptr, 0);
        check(byNameEx == nullptr, "hardened LoadLibraryExW no longer finds the planted DLL", GetLastError());
        check(NeedCurrentDirectoryForExePathW(L"frost-probe.exe") == FALSE,
              "hardened, executable names no longer resolve in the current directory", GetLastError());
        // What the hardening does not cover, and why frost-lint's exe-search rule bans
        // SearchPathW: safe search mode only moves the current directory behind the
        // system directories, so a planted tool is still found.
        wchar_t found[MAX_PATH];
        const DWORD foundUnits = SearchPathW(nullptr, L"frost-planted-tool.exe", nullptr, MAX_PATH, found, nullptr);
        check(foundUnits > 0 && foundUnits < MAX_PATH && std::filesystem::equivalent(found, plantedTool, error),
              "SearchPathW still finds the tool planted in the current directory", GetLastError());
        // Loading by absolute path, as Qt does for plugins, still works.
        HMODULE absolute = LoadLibraryW(plantedDll.c_str());
        check(absolute != nullptr, "loading the planted DLL by absolute path works", GetLastError());
        // System DLLs are still found by name.
        HMODULE system32 = LoadLibraryW(L"version.dll");
        check(system32 != nullptr, "System32 DLLs load by name", GetLastError());
        // Idempotent: a second call reports the same complete state.
        check(frost::app::hardenProcess().complete(), "a second hardenProcess() reports a complete state", 0);
        for (HMODULE module : {byName, byNameEx, absolute, system32}) {
            if (module != nullptr) {
                FreeLibrary(module);
            }
        }
        // Leave the scratch directory, so ScratchDir can remove it.
        SetCurrentDirectoryW(original.c_str());
    }
    return failures == 0 ? 0 : 1;
}

TEST(StartupHardeningDeathTest, CurrentDirectoryNoLongerSuppliesDlls) {
    // hardenProcess() cannot be undone, and the control half needs an unhardened process,
    // so the checks run in a fresh child each time (a death test re-runs this binary), also
    // under --gtest_repeat or when one process runs every test. The child's stderr lists
    // the failed checks.
    EXPECT_EXIT(std::exit(plantedBinaryChecks()), testing::ExitedWithCode(0), "");
}

constexpr unsigned kAbortBits = _WRITE_ABORT_MSG | _CALL_REPORTFAULT;
constexpr UINT kSemFlags = SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX;
#ifdef _DEBUG
constexpr int kReportTypes[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
#endif

// Every setting suppressCrashDialogs() changes, saved on construction and restored on destruction.
class SavedCrashSettings {
public:
    SavedCrashSettings() {
#ifdef _DEBUG
        for (int i = 0; i < 3; ++i) {
            m_reportMode[i] = _CrtSetReportMode(kReportTypes[i], _CRTDBG_REPORT_MODE);
            m_reportFile[i] = _CrtSetReportFile(kReportTypes[i], _CRTDBG_REPORT_FILE);
        }
#endif
    }
    SavedCrashSettings(const SavedCrashSettings&) = delete ("restores exactly once");
    SavedCrashSettings& operator=(const SavedCrashSettings&) = delete ("restores exactly once");
    ~SavedCrashSettings() {
        _set_abort_behavior(m_abortBehavior, kAbortBits);
        _set_error_mode(m_errorMode);
        SetErrorMode(m_semFlags);
#ifdef _DEBUG
        for (int i = 0; i < 3; ++i) {
            _CrtSetReportMode(kReportTypes[i], m_reportMode[i]);
            _CrtSetReportFile(kReportTypes[i], m_reportFile[i]);
        }
#endif
    }

private:
    unsigned m_abortBehavior = _set_abort_behavior(0, 0); // (0, 0) reads without changing.
    int m_errorMode = _set_error_mode(_REPORT_ERRMODE);
    UINT m_semFlags = GetErrorMode();
#ifdef _DEBUG
    int m_reportMode[3] = {};
    _HFILE m_reportFile[3] = {};
#endif
};

/*
 * GoogleTest itself clears _WRITE_ABORT_MSG and _CALL_REPORTFAULT, sends CRT errors to
 * stderr and sets the SEM_* flags in every test process and death-test child, so an abort
 * test cannot tell whether suppressCrashDialogs() does anything. This test therefore
 * restores the dialog-showing defaults, calls it, and reads every setting back. Nothing
 * aborts, so a regression fails an assertion instead of opening a dialog; the process's
 * previous settings are restored at the end.
 */
TEST(StartupCrashDialogs, SwitchesEveryDialogSourceOff) {
#ifdef _DEBUG
    // A report file that cannot be mistaken for stderr. Declared before `saved`, so it is
    // closed only after the previous report files are back.
    struct NullDevice {
        HANDLE handle = CreateFileW(L"NUL", GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        NullDevice() = default;
        NullDevice(const NullDevice&) = delete ("closes exactly once");
        NullDevice& operator=(const NullDevice&) = delete ("closes exactly once");
        ~NullDevice() {
            if (handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
            }
        }
    } const nullDevice;
    ASSERT_NE(nullDevice.handle, INVALID_HANDLE_VALUE) << GetLastError();
#endif
    const SavedCrashSettings saved;

    // The defaults of a GUI-subsystem program, where each of these would show a dialog.
    _set_abort_behavior(kAbortBits, kAbortBits);
    _set_error_mode(_OUT_TO_MSGBOX);
    SetErrorMode(0);
#ifdef _DEBUG
    for (const int type : kReportTypes) {
        _CrtSetReportMode(type, _CRTDBG_MODE_WNDW);
        _CrtSetReportFile(type, nullDevice.handle);
    }
#endif

    frost::app::suppressCrashDialogs();

    EXPECT_EQ(_set_abort_behavior(0, 0) & kAbortBits, 0u);
    EXPECT_EQ(_set_error_mode(_REPORT_ERRMODE), _OUT_TO_STDERR);
    EXPECT_EQ(GetErrorMode() & kSemFlags, kSemFlags);
#ifdef _DEBUG
    for (const int type : kReportTypes) {
        EXPECT_EQ(_CrtSetReportMode(type, _CRTDBG_REPORT_MODE), _CRTDBG_MODE_FILE) << "report type " << type;
        // The CRT stores _CRTDBG_FILE_STDERR as the stderr handle it stands for.
        const _HFILE file = _CrtSetReportFile(type, _CRTDBG_REPORT_FILE);
        EXPECT_TRUE(file == _CRTDBG_FILE_STDERR || file == GetStdHandle(STD_ERROR_HANDLE))
            << "report type " << type << ": " << file;
    }
#endif
}

TEST(StartupCrashDialogsDeathTest, AbortExitsWithoutMessage) {
    // A smoke test only: GoogleTest has already masked the abort message and the dialogs in
    // the child (see SwitchesEveryDialogSourceOff), so this passes even if
    // suppressCrashDialogs() did nothing.
    EXPECT_EXIT(
        {
            frost::app::suppressCrashDialogs();
            std::abort();
        },
        testing::ExitedWithCode(3), testing::Not(testing::HasSubstr("abort() has been called")));
}

#endif

} // namespace
