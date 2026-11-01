// SPDX-License-Identifier: Apache-2.0
//
// Integration tests that run the real frost executable (PLAN.md section 7,
// M0 exit): --version, --help, error exit codes, the built-in self-test (also that
// arguments after "--" never reach Qt), and launching from a directory whose path has
// non-ASCII characters and spaces, and from one longer than MAX_PATH (260 characters) on
// Windows, also through its 8.3 short name; the 250-unit plugin-path limit from both sides
// (kQtPluginPathLimitUnits); and the start-up check's reports when Qt could not load its
// platform plugin (frost/app/startup.hpp).
// Processes are started through frost::io's production IProcessLauncher.

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/utf.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

using namespace std::chrono_literals;
using frost::test::ScratchDir;

// The built executable's absolute path (CMake passes it with forward slashes).
std::string frostExe() { return frost::io::normalizePath(FROST_EXE_PATH).value(); }

struct FrostRun {
    // The child's exit status; -1 if it could not be started, did not finish within
    // runFrost's 120 s timeout, or its output could not be read.
    int exitCode = -1;
    std::string output; // stdout and stderr, interleaved as the child wrote them.
    // What went wrong (launch error, Timeout or IoError) when exitCode == -1.
    std::string startError;
};

// Runs `exe` with `args` and collects its combined output (stderr merged into stdout).
// `environment` overrides apply on top of the test's own environment. Waits at most
// 120 s; on timeout the child's process tree is killed and exitCode is -1.
FrostRun runFrost(const std::string& exe, std::vector<std::string> args,
                  std::vector<std::pair<std::string, std::optional<std::string>>> environment = {}) {
    frost::io::ProcessSpec spec;
    spec.executable = exe;
    spec.arguments = std::move(args);
    spec.environment = std::move(environment);
    spec.stderrMode = frost::io::Stdio::MergeIntoStdout;
    auto result = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, 120s);
    if (!result) {
        return {-1, {}, result.error().describe()};
    }
    return {result->exitCode, std::move(result->stdoutBytes), {}};
}

// A std::filesystem path for `utf8` that Win32 accepts at any length (the \\?\ form).
fs::path anyLengthPath(const std::string& utf8) {
#ifdef _WIN32
    return frost::io::toStdPath(frost::io::extendedLengthPath(utf8).value()).value();
#else
    return frost::io::toStdPath(utf8).value();
#endif
}

// Which of Qt's platform plugins copyFrostTo() copies on Windows.
enum class Plugins : unsigned char {
    All,        // platforms/ as built: the native and the offscreen plugin.
    None,       // No platforms/ directory.
    NativeOnly, // platforms/ with only qwindows(d).dll.
};

// Copies frost and the runtime files it needs (DLLs and, as `plugins` says, Qt's
// platform plugins on Windows) from the build directory into `dir`, creating it.
// Returns the copied executable's path.
std::string copyFrostTo(const std::string& dir, [[maybe_unused]] Plugins plugins = Plugins::All) {
    if (auto created = frost::io::realFileSystem().createDirectories(dir); !created) {
        ADD_FAILURE() << created.error().describe();
        return {};
    }
    const fs::path exe = frost::io::toStdPath(frostExe()).value();
    const fs::path target = anyLengthPath(dir);
    std::error_code error;
    fs::copy_file(exe, target / exe.filename(), fs::copy_options::overwrite_existing, error);
    EXPECT_FALSE(error) << "copy " << exe.string() << ": " << error.message();
#ifdef _WIN32
    const fs::path buildDir = exe.parent_path();
    for (const auto& entry : fs::directory_iterator(buildDir, error)) {
        if (entry.path().extension() == L".dll") {
            fs::copy_file(entry.path(), target / entry.path().filename(), fs::copy_options::overwrite_existing, error);
            EXPECT_FALSE(error) << "copy " << entry.path().string() << ": " << error.message();
        }
    }
    if (plugins != Plugins::None) {
        fs::create_directories(target / "platforms", error);
        EXPECT_FALSE(error) << "create platforms: " << error.message();
        for (const auto& entry : fs::directory_iterator(buildDir / "platforms", error)) {
            if (plugins == Plugins::All || entry.path().filename().wstring().starts_with(L"qwindows")) {
                fs::copy_file(entry.path(), target / "platforms" / entry.path().filename(),
                              fs::copy_options::overwrite_existing, error);
                EXPECT_FALSE(error) << "copy " << entry.path().string() << ": " << error.message();
            }
        }
    }
#endif
    return frost::io::joinPath(dir, frost::io::fromStdPath(exe.filename()).value()).value();
}

TEST(FrostProcess, VersionPrintsProductCompilerAndQt) {
    const auto result = runFrost(frostExe(), {"--version"});
    ASSERT_EQ(result.exitCode, 0) << result.startError << result.output;
    EXPECT_NE(result.output.find("FrostIDE 0.0.1"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("Clang"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("Qt 6."), std::string::npos) << result.output;
}

TEST(FrostProcess, HelpPrintsUsage) {
    const auto result = runFrost(frostExe(), {"--help"});
    ASSERT_EQ(result.exitCode, 0) << result.startError << result.output;
    EXPECT_NE(result.output.find("Usage: frost"), std::string::npos) << result.output;
}

TEST(FrostProcess, UnknownOptionExitsWithStatus2) {
    const auto result = runFrost(frostExe(), {"--no-such-option"});
    EXPECT_EQ(result.exitCode, 2) << result.startError << result.output;
    EXPECT_NE(result.output.find("unknown option --no-such-option"), std::string::npos) << result.output;
}

TEST(FrostProcess, SelfTestPasses) {
    const auto result = runFrost(frostExe(), {"--selftest"});
    ASSERT_EQ(result.exitCode, 0) << result.startError << result.output;
    EXPECT_NE(result.output.find("selftest: PASSED"), std::string::npos) << result.output;
#ifdef _WIN32
    // The manifest's effects are visible only inside the real executable.
    EXPECT_NE(result.output.find("ok   utf8-code-page"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("ok   long-path-manifest"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("ok   dpi-awareness"), std::string::npos) << result.output;
    // Start-up hardening (frost/app/startup.hpp) and the linker's /DEPENDENTLOADFLAG.
    EXPECT_NE(result.output.find("ok   process-hardening"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("ok   dependent-load-flags: load configuration DependentLoadFlags = 0xa00"),
              std::string::npos)
        << result.output;
#endif
    EXPECT_NE(result.output.find("ok   plugin-paths"), std::string::npos) << result.output;
}

TEST(FrostProcess, SelfTestIgnoresQtOptionsAfterDoubleDash) {
    /*
     * Arguments after "--" are paths for frost, never options for Qt. Passed on to Qt,
     * "-platform minimal" would replace the offscreen platform that the start-up plugin
     * check had just checked, and Qt would abort (it has no minimal plugin next to frost on
     * Windows) or run on another platform. Runs offscreen and opens no window.
     */
    std::vector<std::pair<std::string, std::optional<std::string>>> environment{{"QT_QPA_PLATFORM", std::nullopt}};
#ifdef _WIN32
    // As ownPluginsOnly() below: only <exe dir>/platforms may supply the plugin.
    environment.emplace_back("QT_PLUGIN_PATH", std::nullopt);
    environment.emplace_back("QT_QPA_PLATFORM_PLUGIN_PATH", std::nullopt);
#endif
    const auto result = runFrost(frostExe(), {"--selftest", "--", "-platform", "minimal"}, environment);
    ASSERT_EQ(result.exitCode, 0) << result.startError << result.output;
    EXPECT_NE(result.output.find("selftest: PASSED"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("platform plugin 'offscreen'"), std::string::npos) << result.output;
}

TEST(FrostProcess, RunsFromNonAsciiPathWithSpaces) {
    // A per-process scratch root, so concurrent runs never delete each other's copies.
    const ScratchDir scratch("app-process");
    // Latin accents, CJK and spaces in one directory name.
    const std::string exe = copyFrostTo(scratch.join("frost ünïcødé 测试 dir"));
    const auto result = runFrost(exe, {"--selftest"});
    ASSERT_EQ(result.exitCode, 0) << result.startError << result.output;
    EXPECT_NE(result.output.find("selftest: PASSED"), std::string::npos) << result.output;
}

#ifdef _WIN32

// Environment overrides under which only <exe dir>/platforms may supply the plugin, and
// the self-test picks offscreen itself.
std::vector<std::pair<std::string, std::optional<std::string>>> ownPluginsOnly() {
    return {{"QT_PLUGIN_PATH", std::nullopt},
            {"QT_QPA_PLATFORM_PLUGIN_PATH", std::nullopt},
            {"QT_QPA_PLATFORM", std::nullopt}};
}

// The checks every self-test run that frost's plugin check stopped must pass: it said so,
// failed, and did it promptly rather than leaving Qt to abort (observed 2026-10-31,
// before the check existed: the Debug CRT's dialog kept the child alive until the timeout).
void expectPluginCheckFailure(const FrostRun& result, std::chrono::steady_clock::time_point started,
                              std::string_view message) {
    EXPECT_EQ(result.exitCode, 1) << result.startError << result.output;
    EXPECT_NE(result.output.find(std::string("FAIL plugin-paths: ") + std::string(message)), std::string::npos)
        << result.output;
    EXPECT_NE(result.output.find("selftest: FAILED"), std::string::npos) << result.output;
    EXPECT_LT(std::chrono::steady_clock::now() - started, 30s);
}

TEST(FrostProcess, SelfTestReportsMissingPlatformPlugin) {
    // A copy of frost.exe and its DLLs without platforms/.
    const ScratchDir scratch("app-process-noplugins");
    const std::string exe = copyFrostTo(scratch.join("frost"), Plugins::None);
    const auto started = std::chrono::steady_clock::now();
    const auto result = runFrost(exe, {"--selftest"}, ownPluginsOnly());
    expectPluginCheckFailure(result, started, "Qt's platform plugin \"offscreen\" is missing");
}

TEST(FrostProcess, SelfTestReportsMissingPluginForTheChosenPlatform) {
    // platforms/ holds only the native plugin, but the self-test runs on offscreen: having
    // some platform plugin is not enough.
    const ScratchDir scratch("app-process-nativeonly");
    const std::string exe = copyFrostTo(scratch.join("frost"), Plugins::NativeOnly);
    const auto started = std::chrono::steady_clock::now();
    const auto result = runFrost(exe, {"--selftest"}, ownPluginsOnly());
    expectPluginCheckFailure(result, started, "Qt's platform plugin \"offscreen\" is missing");
    EXPECT_NE(result.output.find("holds no qoffscreen.dll or qoffscreend.dll"), std::string::npos) << result.output;
}

// Reads HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled.
bool systemLongPathsEnabled() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                                        L"LongPathsEnabled", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && value != 0;
}

// A directory below `scratch` whose path is longer than MAX_PATH (about 300 characters).
std::string longDirectoryIn(const ScratchDir& scratch) {
    std::string dir = scratch.path();
    while (dir.size() < 300) {
        dir = frost::io::joinPath(dir, std::string(40, 'x') + " segment").value();
    }
    return dir;
}

// The long (GetLongPathNameW) or 8.3 short (GetShortPathNameW) form of the existing
// absolute path `utf8`, without a \\?\ prefix, or "" on failure.
std::string convertedPath(const std::string& utf8, bool shortForm) {
    const auto wide = frost::io::toWin32Path(utf8);
    if (!wide) {
        return {};
    }
    const auto convert = [&](wchar_t* buffer, DWORD units) {
        return shortForm ? GetShortPathNameW(wide->c_str(), buffer, units)
                         : GetLongPathNameW(wide->c_str(), buffer, units);
    };
    const DWORD units = convert(nullptr, 0);
    if (units == 0) {
        return {};
    }
    std::wstring out(units, L'\0');
    const DWORD written = convert(out.data(), units);
    if (written == 0 || written >= units) {
        return {};
    }
    out.resize(written);
    std::wstring_view view = out;
    std::wstring plain;
    if (view.starts_with(L"\\\\?\\UNC\\")) {
        plain = L"\\\\" + std::wstring(view.substr(8));
    } else if (view.starts_with(L"\\\\?\\")) {
        plain = std::wstring(view.substr(4));
    } else {
        plain = out;
    }
    return frost::io::fromWide(plain).value_or(std::string());
}

TEST(FrostProcess, RunsFromPathLongerThanMaxPath) {
    /*
     * PLAN.md's M0 exit criterion. frost::io creates the tree and starts the process
     * through \\?\ paths, which work whatever LongPathsEnabled says (io_integration_tests
     * launches a helper from such a directory), and the loader does find frost's DLLs
     * there. Qt's plugins, though, do not load from a path longer than 250 units while the
     * system-wide switch is off: LoadLibraryW, which Qt uses for them, fails beyond that
     * length for a DLL that embeds an isolation-aware manifest, as Qt's DLLs do
     * (kQtPluginPathLimitUnits in frost/app/startup.hpp). So this test needs the switch,
     * which the frost manifest's longPathAware setting depends on, and skips without it:
     * the criterion stays visibly unmet on such a machine. Whether the switch lifts the
     * loader's limit has not been measured; this test, run with the switch on, measures it.
     * ReportsLongPathLimitCleanly covers what frost does there instead.
     */
    if (!systemLongPathsEnabled()) {
        GTEST_SKIP() << "LongPathsEnabled=0 on this machine: Qt cannot load its platform plugin from a directory "
                        "beyond MAX_PATH; enabling the switch needs administrator rights";
    }
    const ScratchDir scratch("app-process-long");
    const std::string dir = longDirectoryIn(scratch);
    ASSERT_GT(dir.size(), 260u);
    const std::string exe = copyFrostTo(dir);
    const auto result = runFrost(exe, {"--selftest"});
    ASSERT_EQ(result.exitCode, 0) << result.startError << result.output;
    EXPECT_NE(result.output.find("selftest: PASSED"), std::string::npos) << result.output;
}

TEST(FrostProcess, ReportsLongPathLimitCleanly) {
    // The other side of RunsFromPathLongerThanMaxPath: with the switch off, frost must say
    // promptly why it cannot start instead of letting Qt abort. This is not the M0 exit
    // criterion, which asks for a launch that works.
    if (systemLongPathsEnabled()) {
        GTEST_SKIP() << "LongPathsEnabled=1 on this machine: frost does not refuse long paths then, and "
                        "RunsFromPathLongerThanMaxPath measures whether they launch";
    }
    const ScratchDir scratch("app-process-long-report");
    const std::string exe = copyFrostTo(longDirectoryIn(scratch));
    const auto started = std::chrono::steady_clock::now();
    const auto result = runFrost(exe, {"--selftest"}, ownPluginsOnly());
    expectPluginCheckFailure(result, started, "Qt cannot load its platform plugin");
    EXPECT_NE(result.output.find("LongPathsEnabled"), std::string::npos) << result.output;
}

TEST(FrostProcess, RunsThroughShortNamesOfALongPath) {
    /*
     * Started through the 8.3 short name of a directory beyond MAX_PATH, frost sees its
     * own path in that form, and so does Qt: it loads its platform plugin through the short
     * path with LongPathsEnabled=0 (measured 2026-11-01 with Qt 6.12.0; the switch has never
     * been on here). The start-up check must therefore measure the form GetModuleFileNameW
     * reports; measuring the long form would refuse a launch that works. If Qt ever moves to
     * the long form, this fails while LongPathsEnabled=0 with Qt's own abort instead of a
     * clean report, and the check has to follow. With the switch on it can tell only if the
     * switch does not lift the loader's limit, which has not been measured
     * (kQtPluginPathLimitUnits).
     */
    const ScratchDir scratch("app-process-short");
    const std::string exe = copyFrostTo(longDirectoryIn(scratch));
    const std::string shortExe = convertedPath(exe, true);
    // "\platforms\qoffscreend.dll" is 26 units; the short form must be within the 250-unit
    // plugin-path limit (kQtPluginPathLimitUnits).
    if (shortExe.empty() || frost::io::parentPath(shortExe).size() + 26 > 250) {
        GTEST_SKIP() << "the temp directory's volume does not generate 8.3 short names (got '" << shortExe << "')";
    }
    const auto started = std::chrono::steady_clock::now();
    const auto result = runFrost(shortExe, {"--selftest"}, ownPluginsOnly());
    EXPECT_EQ(result.exitCode, 0) << result.startError << result.output;
    EXPECT_NE(result.output.find("selftest: PASSED"), std::string::npos) << result.output;
    // The check saw (and reported) the short form.
    EXPECT_NE(result.output.find("ok   plugin-paths: Qt can load its platform plugin"), std::string::npos)
        << result.output;
    EXPECT_NE(result.output.find("~1\\platforms\\"), std::string::npos) << result.output;
    EXPECT_LT(std::chrono::steady_clock::now() - started, 30s);
}

// The offscreen plugin's file name in the built frost's platforms/ (qoffscreen.dll, or
// qoffscreend.dll in Debug), or "" if there is none.
std::string offscreenPluginName() {
    std::error_code error;
    const fs::path platforms = frost::io::toStdPath(frostExe()).value().parent_path() / "platforms";
    for (const auto& entry : fs::directory_iterator(platforms, error)) {
        const std::string name = frost::io::fromStdPath(entry.path().filename()).value_or(std::string());
        if (name.starts_with("qoffscreen")) {
            return name;
        }
    }
    return {};
}

// Length of `utf8` in UTF-16 code units, the unit kQtPluginPathLimitUnits counts.
std::size_t utf16Units(std::string_view utf8) { return frost::io::utf8ToUtf16(utf8).value().size(); }

// `base` extended by ASCII path components to exactly `units` UTF-16 code units.
// Precondition: units == utf16Units(base) or units >= utf16Units(base) + 2.
std::string padToLength(std::string base, std::size_t units) {
    while (utf16Units(base) < units) {
        // Components stay well below the 255-unit limit, and never leave a remainder of 1.
        const std::size_t remaining = units - utf16Units(base);
        base = frost::io::joinPath(base, std::string(remaining - 1 <= 200 ? remaining - 1 : 100, 'b')).value();
    }
    return base;
}

TEST(FrostProcess, PlatformPluginPathBoundary) {
    /*
     * kQtPluginPathLimitUnits (250, frost/app/startup.hpp; this binary runs frost and does
     * not link frost::app), with the plugin the self-test uses. While LongPathsEnabled=0
     * (as on the reference machine) this pins the limit from both sides: at 250 units Qt
     * must load the plugin, so a lower limit fails here rather than letting Qt abort; at
     * 251 frost must refuse. With the switch on, frost applies no limit and the test checks
     * only that frost also starts at 251 (the first such run measures whether the switch
     * lifts the loader's limit); it cannot see the limit that holds with the switch off, so
     * re-measure that with the switch off.
     * The scratch path is taken in its long form and lengths are counted in UTF-16 units,
     * so they are what frost and Qt see.
     */
    constexpr std::size_t kLimit = 250;
    const std::string plugin = offscreenPluginName();
    ASSERT_FALSE(plugin.empty()) << "no offscreen plugin next to " << frostExe();
    const ScratchDir scratch("app-process-boundary");
    const std::string base = convertedPath(scratch.path(), false);
    ASSERT_FALSE(base.empty()) << scratch.path();
    const std::size_t suffix = std::string_view("\\platforms\\").size() + plugin.size();

    // The first components are non-ASCII (two UTF-8 bytes, one UTF-16 unit each), so every
    // machine checks that the lengths are counted in UTF-16 units.
    const std::string atLimit = padToLength(frost::io::joinPath(base, "\xC3\xA4").value(), kLimit - suffix);
    ASSERT_EQ(utf16Units(atLimit) + suffix, kLimit) << atLimit;
    auto started = std::chrono::steady_clock::now();
    const auto loads = runFrost(copyFrostTo(atLimit), {"--selftest"}, ownPluginsOnly());
    EXPECT_EQ(loads.exitCode, 0) << loads.startError << loads.output;
    EXPECT_NE(loads.output.find("ok   plugin-paths: Qt can load its platform plugin"), std::string::npos)
        << loads.output;
    EXPECT_NE(loads.output.find("selftest: PASSED"), std::string::npos) << loads.output;
    EXPECT_LT(std::chrono::steady_clock::now() - started, 30s);

    const std::string beyond = padToLength(frost::io::joinPath(base, "\xC3\xB6").value(), kLimit + 1 - suffix);
    ASSERT_EQ(utf16Units(beyond) + suffix, kLimit + 1) << beyond;
    started = std::chrono::steady_clock::now();
    const auto refused = runFrost(copyFrostTo(beyond), {"--selftest"}, ownPluginsOnly());
    if (systemLongPathsEnabled()) {
        EXPECT_EQ(refused.exitCode, 0) << refused.startError << refused.output;
        EXPECT_NE(refused.output.find("selftest: PASSED"), std::string::npos) << refused.output;
        return;
    }
    expectPluginCheckFailure(refused, started, "Qt cannot load its platform plugin");
    EXPECT_NE(refused.output.find(plugin + " (251 characters"), std::string::npos) << refused.output;
}

#endif

} // namespace
