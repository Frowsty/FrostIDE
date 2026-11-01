// SPDX-License-Identifier: Apache-2.0
//
// Tests for frost's start-up hardening, guards and plugin pinning (frost/app/startup.hpp and
// startup_core.hpp): which platform plugin files Qt will load and in which order, the report
// for a missing one, QT_QPA_PLATFORM_PLUGIN_PATH's normalisation and pin, the loader's 250-unit
// plain-path limit itself, the pin through \\?\ that satisfies Qt's later plain-path load from
// a long directory (and the failing unpinned load), the launcher's GUI or command-line rule and
// its normalised \\?\ install directory, the hand-over of the hardening state to frost_app.dll,
// that the hardened process no longer loads a DLL planted in the current directory (and what it
// does not cover), that the launcher's \\?\ install directory serves later loads by name, that
// suppressCrashDialogs() switches every dialog source off, that the command-line dialog guard
// refuses dialog windows, and that a Qt fatal error in a command-line mode ends the process
// with status 4.
// hardenProcess(), the guard and the fatal handler run only in death-test children, because
// they cannot be undone; the binary is separate from app_tests so those children start without
// a QApplication.

#include "frost/app/startup.hpp"
#include "frost/io/path.hpp"
#include "frost/io/utf.hpp"

#include "support/scratch_dir.hpp"
#include <QString>
#include <QtGlobal>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <crtdbg.h>
#include <stdlib.h>
#endif

namespace {

using frost::app::platformPluginCandidates;
using frost::app::platformPluginNames;

TEST(StartupPlatformNames, DefaultsToWindows) {
    EXPECT_THAT(platformPluginNames(""), testing::ElementsAre("windows"));
    EXPECT_THAT(platformPluginNames(";:x;"), testing::ElementsAre("windows"));
}

TEST(StartupPlatformNames, SplitsFallbacksAndDropsOptions) {
    EXPECT_THAT(platformPluginNames("offscreen"), testing::ElementsAre("offscreen"));
    EXPECT_THAT(platformPluginNames("windows:darkmode=2,fontengine=freetype"), testing::ElementsAre("windows"));
    EXPECT_THAT(platformPluginNames("direct2d;;windows:nodirectwrite"), testing::ElementsAre("direct2d", "windows"));
}

TEST(StartupPlatformPlugin, CandidatesFollowQtsOrder) {
    // QT_QPA_PLATFORM="direct2d;windows" with QT_QPA_PLATFORM_PLUGIN_PATH set: Qt tries each
    // platform in turn, and for each one its plugin directories in order.
    const std::vector<std::u16string> dirs{u"C:\\custom", u"C:\\fr\u00f8st\\platforms"};
    const std::vector<std::string> platforms{"direct2d", "windows"};
    EXPECT_THAT(platformPluginCandidates(dirs, platforms, false),
                testing::ElementsAre(u"C:\\custom\\qdirect2d.dll", u"C:\\fr\u00f8st\\platforms\\qdirect2d.dll",
                                     u"C:\\custom\\qwindows.dll", u"C:\\fr\u00f8st\\platforms\\qwindows.dll"));
    EXPECT_THAT(platformPluginCandidates({}, platforms, false), testing::IsEmpty());
}

TEST(StartupPlatformPlugin, DebugQtUsesTheDSuffix) {
    // Qt loads only plugins of its own build, so the debug build never pins a release plugin.
    const std::vector<std::u16string> dirs{u"C:\\frost\\platforms"};
    const std::vector<std::string> offscreen{"offscreen"};
    EXPECT_THAT(platformPluginCandidates(dirs, offscreen, true),
                testing::ElementsAre(u"C:\\frost\\platforms\\qoffscreend.dll"));
    EXPECT_THAT(platformPluginCandidates(dirs, offscreen, false),
                testing::ElementsAre(u"C:\\frost\\platforms\\qoffscreen.dll"));
}

TEST(StartupPlatformPlugin, MissingWhenNoPluginForThePlatformQtWillUse) {
    // Only the files Qt would load are candidates: never a lookalike directory, another
    // platform's plugin, or a longer name.
    const std::vector<std::u16string> dirs{u"C:\\fr\u00f8st\\platforms"};
    const std::vector<std::string> offscreen{"offscreen"};
    EXPECT_THAT(platformPluginCandidates(dirs, offscreen, false),
                testing::ElementsAre(u"C:\\fr\u00f8st\\platforms\\qoffscreen.dll"));
    EXPECT_EQ(frost::app::missingPlatformPluginMessage(offscreen, u"C:\\fr\u00f8st"),
              "Qt's platform plugin \"offscreen\" is missing: C:\\fr\xC3\xB8st\\platforms holds no qoffscreen.dll or "
              "qoffscreend.dll. Reinstall FrostIDE or copy the whole installation directory, not only frost.exe.");
    const std::string two = frost::app::missingPlatformPluginMessage(std::vector<std::string>{"a", "b"}, u"C:\\frost");
    EXPECT_THAT(two, testing::HasSubstr("\"a\", \"b\" is missing"));
    EXPECT_THAT(two, testing::HasSubstr("holds no qa.dll, qad.dll, qb.dll or qbd.dll"));
}

TEST(StartupPlatformPlugin, MissingMessageReplacesUnpairedSurrogates) {
    // Win32 paths may hold unpaired surrogates; the message must still be valid UTF-8. U+1F600
    // (a pair) stays intact.
    std::u16string dir = u"C:\\fr\U0001F600st";
    dir[2] = u'\xD800';
    EXPECT_THAT(frost::app::missingPlatformPluginMessage(std::vector<std::string>{"windows"}, dir),
                testing::HasSubstr("is missing: C:\xEF\xBF\xBD"
                                   "fr\xF0\x9F\x98\x80st\\platforms holds no"));
}

TEST(StartupPluginPreload, NeededOnlyBeyondTheLoadersPlainPathLimit) {
    // The loader's limit for DLLs embedding RT_MANIFEST 2 (measured: 250 loads, 251 fails).
    EXPECT_EQ(frost::app::kLoaderPlainPathLimitUnits, 250u);
    // "imageformats\qsvg.dll" is 21 units: 228 + 1 + 21 = 250 loads plain, 229 does not.
    EXPECT_FALSE(frost::app::pluginPathExceedsLoaderLimit(228, 21));
    EXPECT_TRUE(frost::app::pluginPathExceedsLoaderLimit(229, 21));
    EXPECT_FALSE(frost::app::pluginPathExceedsLoaderLimit(0, 0));
}

TEST(StartupHardeningBits, RoundTripEveryField) {
    // The launcher hands the hardening state to frost_app.dll as bits; every field must
    // survive alone and together, and unknown bits must not set anything.
    using frost::app::ProcessHardening;
    const auto fields = std::vector<bool ProcessHardening::*>{
        &ProcessHardening::defaultDllDirectories, &ProcessHardening::dllDirectoryCleared,
        &ProcessHardening::safeSearchPathMode, &ProcessHardening::exeSearchSkipsCurrentDirectory,
        &ProcessHardening::installDirectoryExtended};
    unsigned seen = 0;
    ProcessHardening all;
    for (const auto field : fields) {
        ProcessHardening one;
        one.*field = true;
        all.*field = true;
        const unsigned bits = frost::app::encodeHardening(one);
        EXPECT_EQ(bits & (bits - 1), 0u) << "one field, one bit";
        EXPECT_EQ(bits & seen, 0u) << "each field has its own bit";
        seen |= bits;
        const ProcessHardening back = frost::app::decodeHardening(bits);
        for (const auto other : fields) {
            EXPECT_EQ(back.*other, other == field);
        }
    }
    const ProcessHardening back = frost::app::decodeHardening(frost::app::encodeHardening(all));
    for (const auto field : fields) {
        EXPECT_TRUE(back.*field);
    }
    const ProcessHardening unknown = frost::app::decodeHardening(~seen);
    for (const auto field : fields) {
        EXPECT_FALSE(unknown.*field);
    }
}

#ifdef _WIN32

// launcherCommandLineMode() of `arguments` (argv without the program name).
bool launcherMode(std::initializer_list<const wchar_t*> arguments) {
    return frost::app::launcherCommandLineMode(std::span<const wchar_t* const>(arguments.begin(), arguments.size()));
}

TEST(StartupLauncherMode, OptionsBeforeDoubleDashSelectACommandLineMode) {
    // The launcher shows its box only in GUI mode, so it must read options as frost_app.dll's
    // parser does (command_line.cpp): '-' starts an option, also an unknown one (a usage error,
    // a command-line mode), and everything after "--" is a path.
    EXPECT_FALSE(launcherMode({}));
    EXPECT_FALSE(launcherMode({L"file.txt", L"C:\\fr\u00f8st dir"}));
    EXPECT_FALSE(launcherMode({L""}));
    EXPECT_FALSE(launcherMode({nullptr}));
    EXPECT_TRUE(launcherMode({L"--version"}));
    EXPECT_TRUE(launcherMode({L"file.txt", L"--selftest"}));
    EXPECT_TRUE(launcherMode({L"-"}));
    EXPECT_TRUE(launcherMode({L"---"}));
    EXPECT_TRUE(launcherMode({L"--bogus"}));
    EXPECT_TRUE(launcherMode({L"--selftest", L"--", L"-platform"}));
    // `frost -- <path>`, the way to open a path that starts with '-' (and a file-association
    // template), is a GUI start, so a failure there must still show the box.
    EXPECT_FALSE(launcherMode({L"--", L"-file.txt", L"--version"}));
    EXPECT_FALSE(launcherMode({L"file.txt", L"--", L"--help"}));
}

TEST(StartupInstallDirectory, ExtendedDirectoryNormalisesThePlainSpelling) {
    /*
     * GetModuleFileNameW reports frost.exe's path as the parent spelled it, and \\?\ takes
     * every character literally: AddDllDirectory of "\\?\C:\x\sub\..\bin" fails, and "\\.\C:\..."
     * with a \\?\UNC\ prefix becomes a lookup of the server "." (win32 error 53), as the review
     * of 4c5533d measured. Plain and \\.\ spellings are therefore normalised as Win32 opens them;
     * \\?\ ones are already literal and stay. 8.3 short components are not expanded.
     */
    using frost::app::extendedDirectoryOf;
    EXPECT_EQ(extendedDirectoryOf(L"C:\\fr\u00f8st dir\\bin\\frost.exe"), L"\\\\?\\C:\\fr\u00f8st dir\\bin");
    EXPECT_EQ(extendedDirectoryOf(L"C:\\x\\sub\\..\\bin\\.\\frost.exe"), L"\\\\?\\C:\\x\\bin");
    EXPECT_EQ(extendedDirectoryOf(L"C:\\x\\bin.\\frost.exe"), L"\\\\?\\C:\\x\\bin");
    EXPECT_EQ(extendedDirectoryOf(L"C:/x//bin/frost.exe"), L"\\\\?\\C:\\x\\bin");
    EXPECT_EQ(extendedDirectoryOf(L"\\\\.\\C:\\x\\..\\bin\\frost.exe"), L"\\\\?\\C:\\bin");
    EXPECT_EQ(extendedDirectoryOf(L"\\\\.\\UNC\\server\\share\\x\\..\\bin\\frost.exe"),
              L"\\\\?\\UNC\\server\\share\\bin");
    EXPECT_EQ(extendedDirectoryOf(L"\\\\server\\share\\bin\\frost.exe"), L"\\\\?\\UNC\\server\\share\\bin");
    EXPECT_EQ(extendedDirectoryOf(L"\\\\?\\C:\\x\\..\\bin\\frost.exe"), L"\\\\?\\C:\\x\\..\\bin");
    EXPECT_EQ(extendedDirectoryOf(L"\\\\?\\UNC\\server\\share\\bin\\frost.exe"), L"\\\\?\\UNC\\server\\share\\bin");
    EXPECT_EQ(extendedDirectoryOf(L"C:\\PROGRA~1\\Frost\\frost.exe"), L"\\\\?\\C:\\PROGRA~1\\Frost");
    EXPECT_EQ(extendedDirectoryOf(L"frost.exe").substr(0, 4), L"\\\\?\\");
    EXPECT_EQ(extendedDirectoryOf(L"\\\\?\\frost.exe"), L"");
    // Beyond MAX_PATH: unchanged apart from the prefix.
    const std::wstring longDir =
        L"C:\\" + std::wstring(100, L'a') + L"\\" + std::wstring(100, L'b') + L"\\" + std::wstring(100, L'c');
    EXPECT_EQ(extendedDirectoryOf(longDir + L"\\frost.exe"), L"\\\\?\\" + longDir);
}

TEST(StartupPlatformPlugin, OverrideDirectoryIsNormalisedAsQtResolvesIt) {
    // QT_QPA_PLATFORM_PLUGIN_PATH as users and scripts write it; the pin goes through \\?\,
    // which would take every one of these spellings literally.
    using frost::app::platformPluginOverrideDirectory;
    EXPECT_EQ(platformPluginOverrideDirectory(u""), u"");
    EXPECT_EQ(platformPluginOverrideDirectory(u"plugins\\platforms"), u"") << "relative: Qt's to resolve";
    EXPECT_EQ(platformPluginOverrideDirectory(u"C:"), u"");
    EXPECT_EQ(platformPluginOverrideDirectory(u"C:platforms"), u"");
    EXPECT_EQ(platformPluginOverrideDirectory(u"C:/fr\u00f8st/plugins/platforms/"),
              u"C:\\fr\u00f8st\\plugins\\platforms");
    EXPECT_EQ(platformPluginOverrideDirectory(u"//?/C:/qt/platforms"), u"C:\\qt\\platforms");
    EXPECT_EQ(platformPluginOverrideDirectory(u"\\\\?\\C:\\qt\\platforms\\\\"), u"C:\\qt\\platforms");
    EXPECT_EQ(platformPluginOverrideDirectory(u"C:\\qt\\.\\x\\..\\platforms"), u"C:\\qt\\platforms");
    EXPECT_EQ(platformPluginOverrideDirectory(u"C:\\qt\\platforms."), u"C:\\qt\\platforms");
    EXPECT_EQ(platformPluginOverrideDirectory(u"C:\\qt//platforms"), u"C:\\qt\\platforms");
    EXPECT_EQ(platformPluginOverrideDirectory(u"\\\\server\\share\\platforms\\"), u"\\\\server\\share\\platforms");
    EXPECT_EQ(platformPluginOverrideDirectory(u"\\\\?\\UNC\\server\\share\\platforms"),
              u"\\\\server\\share\\platforms");
    EXPECT_EQ(platformPluginOverrideDirectory(u"\\\\.\\C:\\qt\\x\\..\\platforms"), u"C:\\qt\\platforms");
    EXPECT_EQ(platformPluginOverrideDirectory(u"C:\\"), u"C:\\");
}

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
    const auto result = frost::app::preparePlatformPlugin();
    ASSERT_FALSE(result.has_value()) << result->detail;
    EXPECT_EQ(result.error().code(), frost::ErrorCode::NotFound);
    EXPECT_THAT(result.error().message(), testing::HasSubstr("Qt's platform plugin \"offscreen\" is missing: "));
}

#endif

TEST(StartupPluginPaths, PresenceCheckDefersToQtPluginPath) {
#ifdef _WIN32
    // QT_PLUGIN_PATH tells Qt to look elsewhere, so a missing plugin is left to Qt.
    const ScopedEnvironment pluginPath(L"QT_PLUGIN_PATH", L"C:\\frost-elsewhere");
    const ScopedEnvironment platformPath(L"QT_QPA_PLATFORM_PLUGIN_PATH", nullptr);
#endif
    // On Linux the check always passes.
    const auto result = frost::app::preparePlatformPlugin();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_THAT(result->detail, testing::HasSubstr("not checked"));
}

#ifdef _WIN32

// The SDK's offscreen plugin of this test's own Qt build (the d suffix in Debug), unmodified.
std::filesystem::path sdkOffscreenPlugin() {
    const bool debugQt = GetModuleHandleW(L"Qt6Cored.dll") != nullptr;
    return frost::io::toStdPath(FROST_QT_PLUGIN_DIR).value() / "platforms" /
           (debugQt ? L"qoffscreend.dll" : L"qoffscreen.dll");
}

// A std::filesystem path for `utf8` that Win32 accepts at any length (the \\?\ form).
std::filesystem::path anyLengthPath(const std::string& utf8) {
    return frost::io::toStdPath(frost::io::extendedLengthPath(utf8).value()).value();
}

// Copies `from` to `dir`\`name` (creating `dir`, of any length) and returns the copy's path
// in UTF-8.
std::string copyInto(const std::filesystem::path& from, const std::string& dir, const std::string& name) {
    std::error_code error;
    std::filesystem::create_directories(anyLengthPath(dir), error);
    EXPECT_FALSE(error) << dir << ": " << error.message();
    const std::string to = frost::io::joinPath(dir, name).value();
    std::filesystem::copy_file(from, anyLengthPath(to), std::filesystem::copy_options::overwrite_existing, error);
    EXPECT_FALSE(error) << from.string() << ": " << error.message();
    return to;
}

// Frees every reference this process holds to the module at `utf8` (so the scratch file can be
// deleted): the pin and any later load.
void unload(const std::string& utf8) {
    const std::wstring wide = frost::io::toWin32Path(utf8).value();
    HMODULE module = nullptr;
    while (GetModuleHandleExW(0, wide.c_str(), &module) != FALSE && module != nullptr) {
        // GetModuleHandleExW without UNCHANGED_REFCOUNT took one more reference.
        FreeLibrary(module);
        if (FreeLibrary(module) == FALSE) {
            break;
        }
    }
}

// The long form (GetLongPathNameW) of the existing path `utf8`, so that lengths built on it are
// the ones the loader sees (the temp directory may be named through an 8.3 component); `utf8`
// itself if that fails.
std::string longForm(const std::string& utf8) {
    const std::wstring wide = frost::io::toWide(utf8).value();
    const DWORD units = GetLongPathNameW(wide.c_str(), nullptr, 0);
    if (units == 0) {
        return utf8;
    }
    std::wstring out(units, L'\0');
    const DWORD written = GetLongPathNameW(wide.c_str(), out.data(), units);
    if (written == 0 || written >= units) {
        return utf8;
    }
    out.resize(written);
    return frost::io::fromWide(out).value_or(utf8);
}

// `base` extended by ASCII components to exactly `units` UTF-16 code units (at least 2 more).
std::string padToLength(std::string base, std::size_t units) {
    const auto length = [](const std::string& s) { return frost::io::utf8ToUtf16(s).value().size(); };
    while (length(base) < units) {
        // A separator and up to 100 units, never leaving exactly 1 unit (no room for a component).
        const std::size_t remaining = units - length(base);
        const std::size_t component = remaining - 1 <= 100 ? remaining - 1 : std::min<std::size_t>(100, remaining - 3);
        base = frost::io::joinPath(base, std::string(component, 'p')).value();
    }
    return base;
}

/*
 * Loads Qt6Gui of this test's Qt build from the test's directory and keeps it for the
 * process's lifetime, as frost does. The platform plugin imports it, and with
 * loadPlatformPlugin()'s USER_DIRS | SYSTEM32 flags the loader finds it only when it is already
 * loaded (frost's launcher adds the install directory, which a test process has not done).
 * Unloading the plugin afterwards then never unloads Qt6Gui.
 */
void keepQtGuiLoaded() {
    const bool debugQt = GetModuleHandleW(L"Qt6Cored.dll") != nullptr;
    const HMODULE gui = LoadLibraryExW(debugQt ? L"Qt6Guid.dll" : L"Qt6Gui.dll", nullptr,
                                       LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    ASSERT_NE(gui, nullptr) << "win32 error " << GetLastError();
}

TEST(StartupLoadPlatformPlugin, SkipsMissingCandidates) {
    // The first candidate does not exist; the second does, under other letter case, as Windows
    // file names are case-insensitive (the copy is "Real\QOFFSCREEN(D).DLL", the candidate
    // "real\qoffscreen(d).dll").
    keepQtGuiLoaded();
    const frost::test::ScratchDir scratch("app-startup-skip");
    std::string name = frost::io::fromStdPath(sdkOffscreenPlugin().filename()).value();
    const std::string lower = name;
    std::ranges::transform(name, name.begin(), [](char c) { return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c; });
    const std::string real = copyInto(sdkOffscreenPlugin(), scratch.join("Real"), name);
    const std::string candidate = frost::io::joinPath(scratch.join("real"), lower).value();
    const std::string missing = frost::io::joinPath(scratch.join("missing"), "qoffscreen.dll").value();
    const std::vector<std::u16string> candidates{frost::io::utf8ToUtf16(missing).value(),
                                                 frost::io::utf8ToUtf16(candidate).value()};
    const auto loaded = frost::app::loadPlatformPlugin(candidates, std::vector<std::string>{"offscreen"});
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message();
    EXPECT_EQ(*loaded, candidates[1]);
    unload(real);

    const auto none = frost::app::loadPlatformPlugin(std::vector{candidates[0]}, std::vector<std::string>{"offscreen"});
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().code(), frost::ErrorCode::NotFound);
}

TEST(StartupLoadPlatformPlugin, ReportsWin32ErrorForAnUnloadableFile) {
    // A file that exists but is not a PE image: the loader's own error comes back, and a
    // later candidate is not tried, because Qt would stop at the first one too.
    const frost::test::ScratchDir scratch("app-startup-bad");
    const std::string bad = frost::io::joinPath(scratch.join("bad"), "qoffscreen.dll").value();
    std::error_code error;
    std::filesystem::create_directories(anyLengthPath(scratch.join("bad")), error);
    {
        std::ofstream out(anyLengthPath(bad), std::ios::binary);
        out << std::string(4096, 'x');
    }
    const std::string real = copyInto(sdkOffscreenPlugin(), scratch.join("real"), "qoffscreen.dll");
    const std::vector<std::u16string> candidates{frost::io::utf8ToUtf16(bad).value(),
                                                 frost::io::utf8ToUtf16(real).value()};
    const auto result = frost::app::loadPlatformPlugin(candidates, std::vector<std::string>{"offscreen"});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), frost::ErrorCode::IoError);
    EXPECT_THAT(result.error().message(),
                testing::HasSubstr("Qt's platform plugin \"offscreen\" cannot be loaded from " + bad + ": "));
    EXPECT_THAT(result.error().message(), testing::HasSubstr("(win32 error 193)"));
    EXPECT_EQ(GetModuleHandleW(frost::io::toWin32Path(real).value().c_str()), nullptr);
}

TEST(StartupLoadPlatformPlugin, PinnedPluginSatisfiesQtsPlainPathLoad) {
    /*
     * The mechanism frost's start from a long directory rests on (probe 7, ADR 0012): Qt strips
     * \\?\ and loads its platform plugin through the plain path, which the loader refuses beyond
     * 250 units for a DLL embedding RT_MANIFEST 2, as the SDK's unmodified plugin does. Once
     * loadPlatformPlugin() has pinned it through \\?\, that plain-path load returns the same
     * module. Pinned here on every run, so a Windows loader change that breaks it fails here.
     * 300 units with a non-ASCII first component.
     */
    keepQtGuiLoaded();
    const frost::test::ScratchDir scratch("app-startup-pin");
    const std::string dir = padToLength(frost::io::joinPath(scratch.path(), "\xC3\xA4 \xE6\xB5\x8B x").value(), 300);
    const std::string name = frost::io::fromStdPath(sdkOffscreenPlugin().filename()).value();
    const std::string plugin = copyInto(sdkOffscreenPlugin(), dir, name);
    ASSERT_GT(frost::io::utf8ToUtf16(plugin).value().size(), 260u);
    std::wstring plain = frost::io::toWide(plugin).value();

    // The control: unpinned, the plain-path load fails, so what follows is the pin's doing.
    SetLastError(ERROR_SUCCESS);
    EXPECT_EQ(LoadLibraryExW(plain.c_str(), nullptr, 0), nullptr);
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_FILENAME_EXCED_RANGE));

    const auto pinned = frost::app::loadPlatformPlugin(std::vector{frost::io::utf8ToUtf16(plugin).value()},
                                                       std::vector<std::string>{"offscreen"});
    ASSERT_TRUE(pinned.has_value()) << pinned.error().message();

    // Qt's later call: the plain path, no \\?\ prefix, backslashes and forward slashes.
    HMODULE byPin = GetModuleHandleW(frost::io::toWin32Path(plugin).value().c_str());
    ASSERT_NE(byPin, nullptr);
    HMODULE byPlainPath = LoadLibraryExW(plain.c_str(), nullptr, 0);
    EXPECT_EQ(byPlainPath, byPin) << "win32 error " << GetLastError();
    std::ranges::replace(plain, L'\\', L'/');
    HMODULE bySlashes = LoadLibraryExW(plain.c_str(), nullptr, 0);
    EXPECT_EQ(bySlashes, byPin) << "win32 error " << GetLastError();
    for (HMODULE module : {byPlainPath, bySlashes}) {
        if (module != nullptr) {
            FreeLibrary(module);
        }
    }
    unload(plugin);
}

TEST(StartupLoaderLimit, PlainPathOfAManifestDllLoadsUpTo250Units) {
    /*
     * The Windows loader's limit that kLoaderPlainPathLimitUnits states and that gates
     * preloadOtherPlugins(), measured on every run with the SDK's unmodified offscreen plugin
     * (it embeds RT_MANIFEST 2, as all of Qt's DLLs do): a plain path of exactly 250 UTF-16
     * units loads, 251 fails with ERROR_FILENAME_EXCED_RANGE. A Windows or Qt upgrade that
     * lowers the limit fails here, where Qt would otherwise skip plugins silently. No pin.
     */
    keepQtGuiLoaded();
    const frost::test::ScratchDir scratch("app-startup-limit");
    const std::string name = frost::io::fromStdPath(sdkOffscreenPlugin().filename()).value();
    const std::size_t limit = frost::app::kLoaderPlainPathLimitUnits;
    for (const std::size_t units : {limit, limit + 1}) {
        SCOPED_TRACE(std::format("plugin path of {} UTF-16 units", units));
        const std::string dir =
            padToLength(frost::io::joinPath(longForm(scratch.path()), std::format("\xC3\xA4 {}", units)).value(),
                        units - 1 - name.size());
        const std::string plugin = copyInto(sdkOffscreenPlugin(), dir, name);
        const std::wstring plain = frost::io::toWide(plugin).value();
        ASSERT_EQ(plain.size(), units);
        SetLastError(ERROR_SUCCESS);
        HMODULE module = LoadLibraryExW(plain.c_str(), nullptr, 0);
        const DWORD error = GetLastError();
        if (units <= limit) {
            EXPECT_NE(module, nullptr) << "win32 error " << error;
        } else {
            EXPECT_EQ(module, nullptr);
            EXPECT_EQ(error, static_cast<DWORD>(ERROR_FILENAME_EXCED_RANGE));
        }
        if (module != nullptr) {
            FreeLibrary(module);
        }
        unload(plugin);
    }
}

TEST(StartupPlatformPlugin, PinsFromQtQpaPlatformPluginPathFirst) {
    /*
     * preparePlatformPlugin() with QT_QPA_PLATFORM_PLUGIN_PATH set, as a script would write it
     * (forward slashes, "." and "..", a trailing separator): the plugin in that directory is
     * pinned, through the normalised path (platformPluginOverrideDirectory()); the \\?\ form
     * of the raw value would name no file. The test binary's own directory has no platforms\.
     */
    keepQtGuiLoaded();
    const frost::test::ScratchDir scratch("app-startup-override");
    const std::string name = frost::io::fromStdPath(sdkOffscreenPlugin().filename()).value();
    const std::string custom = scratch.join("custom pl\xC3\xBCgins");
    const std::string plugin = copyInto(sdkOffscreenPlugin(), custom, name);
    std::string spelled = custom + "\\.\\sub\\..\\";
    std::ranges::replace(spelled, '\\', '/');
    const std::wstring value = frost::io::toWide(spelled).value();
    const ScopedEnvironment pluginPath(L"QT_PLUGIN_PATH", nullptr);
    const ScopedEnvironment platformPath(L"QT_QPA_PLATFORM_PLUGIN_PATH", value.c_str());
    const ScopedEnvironment platform(L"QT_QPA_PLATFORM", L"offscreen");
    const auto result = frost::app::preparePlatformPlugin();
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->detail, "loaded Qt's platform plugin " + plugin);
    unload(plugin);
}

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
        check(!hardening.installDirectoryExtended, "hardenProcess() leaves the launcher's step to the launcher", 0);

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

/*
 * The launcher's step after hardening (searchInstallDirectoryExtended()), in a fresh child:
 * a DLL in the \\?\ "install" directory loads later by bare name, in \\?\ form, while one in
 * the current directory still does not. The DLL is a renamed copy of System32's version.dll.
 */
int extendedInstallDirectoryChecks() {
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
        const frost::test::ScratchDir scratch("app-startup-extended");
        wchar_t system[MAX_PATH];
        if (GetSystemDirectoryW(system, MAX_PATH) == 0) {
            std::fprintf(stderr, "GetSystemDirectoryW failed\n");
            return 1;
        }
        const std::filesystem::path install = frost::io::toStdPath(scratch.join("install")).value();
        const std::filesystem::path current = frost::io::toStdPath(scratch.join("current")).value();
        std::error_code error;
        std::filesystem::create_directories(install, error);
        std::filesystem::create_directories(current, error);
        std::filesystem::copy_file(std::filesystem::path(system) / L"version.dll", install / L"frost_late_probe.dll",
                                   error);
        if (!error) {
            std::filesystem::copy_file(std::filesystem::path(system) / L"version.dll", current / L"frost_cwd_probe.dll",
                                       error);
        }
        if (error || SetCurrentDirectoryW(current.c_str()) == FALSE) {
            std::fprintf(stderr, "setup failed: %s\n", error.message().c_str());
            return 1;
        }
        const frost::app::ProcessHardening hardening = frost::app::hardenProcess();
        check(hardening.complete(), "hardenProcess() applied", 0);
        check(frost::app::searchInstallDirectoryExtended(frost::app::extendedPath(install.wstring())),
              "searchInstallDirectoryExtended() applied", GetLastError());
        HMODULE late = LoadLibraryW(L"frost_late_probe.dll");
        check(late != nullptr, "a DLL in the install directory loads by bare name", GetLastError());
        if (late != nullptr) {
            check(frost::app::modulePath(late).starts_with(L"\\\\?\\"), "it loads in its \\\\?\\ form", 0);
            FreeLibrary(late);
        }
        HMODULE planted = LoadLibraryW(L"frost_cwd_probe.dll");
        check(planted == nullptr, "the current directory still supplies no DLL", GetLastError());
        HMODULE system32 = LoadLibraryW(L"version.dll");
        check(system32 != nullptr, "System32 DLLs still load by name", GetLastError());
        for (HMODULE module : {planted, system32}) {
            if (module != nullptr) {
                FreeLibrary(module);
            }
        }
        SetCurrentDirectoryW(original.c_str());
    }
    return failures == 0 ? 0 : 1;
}

TEST(StartupHardeningDeathTest, ExtendedInstallDirectoryServesLaterLoads) {
    EXPECT_EXIT(std::exit(extendedInstallDirectoryChecks()), testing::ExitedWithCode(0), "");
}

/*
 * The dialog guard in a fresh child. The child's thread moves to a private desktop first
 * (SetThreadDesktop fails once a thread has a hook or a window), so even a guard that did
 * nothing could not put a box on the user's screen; a watchdog ends the child after 10 s if a
 * box waits there. Then: a hidden "#32770" window is refused, MessageBoxW returns 0 at once,
 * and other windows are still created.
 */
int dialogGuardChecks() {
    int failures = 0;
    const auto check = [&failures](bool ok, const char* what, DWORD error) {
        if (!ok) {
            ++failures;
            std::fprintf(stderr, "check failed: %s (last error %lu)\n", what, error);
        }
    };
    // Not tests/support's PrivateDesktop: a hook needs DESKTOP_HOOKCONTROL on the thread's
    // desktop, which a desktop for child processes does not grant. Never closed: the thread
    // stays on it until the child exits.
    const std::wstring name = std::format(L"frost-test-guard-{}", GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
                                   DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_HOOKCONTROL |
                                       DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS,
                                   nullptr);
    if (desktop == nullptr || SetThreadDesktop(desktop) == FALSE) {
        std::fprintf(stderr, "no private desktop for this thread (last error %lu); no dialog is tried\n",
                     GetLastError());
        return 1;
    }
    std::thread([] {
        Sleep(10'000);
        std::fprintf(stderr, "watchdog: a dialog waited for 10 s\n");
        std::_Exit(9);
    }).detach();
    if (!frost::app::guardCommandLineDialogs()) {
        std::fprintf(stderr,
                     "check failed: guardCommandLineDialogs() installed its hook (last error %lu); "
                     "no dialog is tried\n",
                     GetLastError());
        return 1;
    }
    HWND dialog = CreateWindowExW(0, L"#32770", L"frost dialog guard probe", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    check(dialog == nullptr, "a #32770 window is refused", GetLastError());
    if (dialog != nullptr) {
        DestroyWindow(dialog);
    }
    check(MessageBoxW(nullptr, L"probe", L"frost dialog guard probe", MB_OK) == 0, "MessageBoxW returns 0 at once",
          GetLastError());
    HWND other = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr),
                                 nullptr);
    check(other != nullptr, "other windows are still created", GetLastError());
    if (other != nullptr) {
        DestroyWindow(other);
    }
    return failures == 0 ? 0 : 1;
}

TEST(StartupDialogGuardDeathTest, RefusesDialogWindowsOnTheMainThread) {
    EXPECT_EXIT(std::exit(dialogGuardChecks()), testing::ExitedWithCode(0),
                "frost: refused to show a dialog in a command-line mode: frost dialog guard probe");
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
 * switches every dialog source on (a superset of the defaults), calls it, and reads every
 * setting back. Nothing
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

    // Every dialog source switched on: a superset of a GUI-subsystem program's defaults (the
    // CRT sets only one of the two abort bits per build flavour, and _CRT_WARN goes to the
    // debugger by default), so suppressCrashDialogs() must clear each one.
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

TEST(StartupFatalHandlerDeathTest, QtFatalExitsWithStatus4) {
    // Where Qt would abort (0xC0000409 on Windows, SIGABRT on Linux) or show the Debug CRT's
    // box, a command-line mode ends with status 4 and says why on stderr.
    EXPECT_EXIT(
        {
            frost::app::installCommandLineFatalHandler(nullptr);
            qFatal("probe %d", 42);
        },
        testing::ExitedWithCode(4), "frost: Qt fatal error: probe 42");
}

TEST(StartupFatalHandlerDeathTest, OtherMessagesReachThePreviousHandler) {
    // A warning still goes to the handler that was installed before (Qt's default one in frost,
    // a recorder here, because Qt's default writes to the debugger without a console) and does
    // not end the process.
    EXPECT_EXIT(
        {
            qInstallMessageHandler([](QtMsgType, const QMessageLogContext&, const QString& message) {
                std::fprintf(stderr, "previous handler: %s\n", message.toUtf8().constData());
                std::fflush(stderr);
            });
            frost::app::installCommandLineFatalHandler(nullptr);
            qWarning("frost-warning-probe");
            std::exit(0);
        },
        testing::ExitedWithCode(0), "previous handler: frost-warning-probe");
}

} // namespace
