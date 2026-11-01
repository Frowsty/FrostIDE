// SPDX-License-Identifier: Apache-2.0
//
// Integration tests that run the real frost program (PLAN.md section 7, M0 exit): --version,
// --help, error exit codes, the built-in self-test (also that arguments after "--" never reach
// Qt), and launching from a directory whose path has non-ASCII characters and spaces. On
// Windows also the M0 exit criterion's long paths (ADR 0012): a start from every directory
// length from 200 to 400 UTF-16 units, plain and \\?\, also with long paths ineffective for
// frost (a manifest without longPathAware, the state LongPathsEnabled=0 gives), and through 8.3
// short names; a DLL loaded late and a non-platform plugin from a long plain path; the start-up
// check's reports when Qt's platform plugin is missing or unloadable, and that Qt's own "no
// platform plugin" dialog never appears in a command-line mode; the launcher's failure
// reports; and the structure of the two images: frost.exe imports KERNEL32.dll alone, and
// frost_app.dll carries no manifest and no DependentLoadFlags.
// Processes are started through frost::io's production IProcessLauncher, on Windows on a
// private desktop each and with hard-error boxes off (runFrost).

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/utf.hpp"

#include "support/private_desktop.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
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
    // runFrost's timeout, or its output could not be read.
    int exitCode = -1;
    std::string output; // stdout and stderr, interleaved as the child wrote them.
    // What went wrong (launch error, Timeout or IoError) when exitCode == -1.
    std::string startError;
};

using Environment = std::vector<std::pair<std::string, std::optional<std::string>>>;

/*
 * Runs `exe` with `args` and collects its combined output (stderr merged into stdout).
 * `environment` overrides apply on top of the test's own environment; `form` is the form of
 * the executable path handed to CreateProcessW on Windows. Waits at most `timeout`; on
 * timeout the child's process tree is killed and exitCode is -1.
 * On Windows no box from the child reaches the user's screen (tests/support/private_desktop.hpp).
 * The child runs on a fresh private desktop, which holds the boxes it draws itself (a box
 * frost failed to prevent, or a Debug CRT box); such a run ends at the timeout instead. It
 * also inherits an error mode without hard-error boxes, so a loader failure before main
 * ends it at once with that status. If no desktop can be created the child is not started
 * and the test fails.
 */
FrostRun runFrost(const std::string& exe, std::vector<std::string> args, Environment environment = {},
                  frost::io::ImagePathForm form = frost::io::ImagePathForm::Auto,
                  std::chrono::milliseconds timeout = 120s) {
    frost::io::ProcessSpec spec;
    spec.executable = exe;
    spec.arguments = std::move(args);
    spec.environment = std::move(environment);
    spec.stderrMode = frost::io::Stdio::MergeIntoStdout;
    spec.imagePathForm = form;
#ifdef _WIN32
    const frost::test::NoHardErrorBoxes noHardErrorBoxes;
    const frost::test::PrivateDesktop desktop;
    if (!desktop.valid()) {
        return {-1, {}, "no private desktop, so the child was not started (never on the user's desktop)"};
    }
    spec.desktop = desktop.name();
#endif
    auto result = frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, timeout);
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

// Copies frost and the runtime files it needs (on Windows frost_app.dll and the other DLLs
// beside frost.exe and, as `plugins` says, Qt's platform plugins) from the build directory
// into `dir`, creating it. Returns the copied executable's path.
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
    // Start-up hardening, including the launcher's \\?\ install directory (ADR 0012), and the
    // linker's /DEPENDENTLOADFLAG on frost.exe only.
    EXPECT_NE(result.output.find("ok   process-hardening"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("ok   dependent-load-flags: DependentLoadFlags frost.exe 0xa00, frost_app.dll 0x0"),
              std::string::npos)
        << result.output;
    // The launcher loaded frost_app.dll and, through it, Qt through \\?\ paths.
    EXPECT_NE(result.output.find("ok   load-form: frost_app.dll and Qt6Core"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("ok   dialog-guard"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("ok   plugin-paths: loaded Qt's platform plugin "), std::string::npos)
        << result.output;
    EXPECT_NE(result.output.find("ok   late-load: all "), std::string::npos) << result.output;
#endif
    EXPECT_NE(result.output.find("ok   plugin-paths"), std::string::npos) << result.output;
}

TEST(FrostProcess, SelfTestIgnoresQtOptionsAfterDoubleDash) {
    /*
     * Arguments after "--" are paths for frost, never options for Qt. Passed on to Qt,
     * "-platform minimal" would replace the offscreen platform that the start-up plugin
     * check had just pinned, and Qt would abort (it has no minimal plugin next to frost on
     * Windows) or run on another platform. Runs offscreen and opens no window.
     */
    Environment environment{{"QT_QPA_PLATFORM", std::nullopt}};
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

using frost::io::ImagePathForm;

// Environment overrides under which only <exe dir>/platforms may supply the plugin, and
// the self-test picks offscreen itself.
Environment ownPluginsOnly() {
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

// The d suffix of this build's Qt DLLs: the test is built like frost, and only the Debug
// configuration uses Qt's debug build.
#ifdef _DEBUG
constexpr std::string_view kQtSuffix = "d";
#else
constexpr std::string_view kQtSuffix = "";
#endif

// The name of Qt module `module`'s DLL in this build ("Qt6Network" -> Qt6Network(d).dll).
std::string qtDll(std::string_view module) { return std::format("{}{}.dll", module, kQtSuffix); }

// `utf8` (a path CMake passed with forward slashes) joined with `name`, as a std::filesystem path.
fs::path sdkFile(std::string_view dir, std::string_view name) {
    return frost::io::toStdPath(frost::io::joinPath(frost::io::normalizePath(dir).value(), name).value()).value();
}

// Copies `from` into the existing directory `dir` (of any length) as `name`.
void copyInto(const fs::path& from, const std::string& dir, const std::string& name) {
    std::error_code error;
    fs::copy_file(from, anyLengthPath(dir) / frost::io::toStdPath(name).value(), fs::copy_options::overwrite_existing,
                  error);
    EXPECT_FALSE(error) << "copy " << from.string() << ": " << error.message();
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

TEST(FrostProcess, SelfTestReportsUnloadablePlatformPlugin) {
    // The plugin exists but is not a DLL: the pin's own loader error comes back, promptly,
    // instead of Qt's abort.
    const ScratchDir scratch("app-process-badplugin");
    const std::string dir = scratch.join("frost");
    const std::string exe = copyFrostTo(dir);
    {
        std::ofstream out(anyLengthPath(dir) / "platforms" / frost::io::toStdPath(qtDll("qoffscreen")).value(),
                          std::ios::binary | std::ios::trunc);
        out << std::string(4096, 'x');
    }
    const auto started = std::chrono::steady_clock::now();
    const auto result = runFrost(exe, {"--selftest"}, ownPluginsOnly());
    expectPluginCheckFailure(result, started, "Qt's platform plugin \"offscreen\" cannot be loaded from ");
    EXPECT_NE(result.output.find("(win32 error 193)"), std::string::npos) << result.output;
}

TEST(FrostProcess, SelfTestNeverShowsQtsDialog) {
    /*
     * QT_PLUGIN_PATH names a directory without plugins, so frost's check leaves the platform
     * plugin to Qt ("not checked"), and Qt finds none. Qt's release build then shows its "no
     * Qt platform plugin could be initialized" box when the process has no console, as here;
     * the dialog guard refuses it, and Qt's fatal error ends frost with status 4 and the
     * message on stderr. Qt's debug build shows no box, and only the fatal handler acts.
     * Before the guard (2026-10-31), the box waited for a click. The self-test's report is
     * never printed: Qt ends the process inside QApplication's constructor, before the GUI
     * checks.
     */
    const ScratchDir scratch("app-process-qtdialog");
    const std::string exe = copyFrostTo(scratch.join("frost"), Plugins::None);
    const std::string empty = scratch.join("no plugins");
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(empty).has_value());
    const auto started = std::chrono::steady_clock::now();
    const auto result = runFrost(
        exe, {"--selftest"},
        {{"QT_PLUGIN_PATH", empty}, {"QT_QPA_PLATFORM_PLUGIN_PATH", std::nullopt}, {"QT_QPA_PLATFORM", std::nullopt}},
        ImagePathForm::Auto, 30s);
    EXPECT_EQ(result.exitCode, 4) << result.startError << result.output;
    EXPECT_NE(result.output.find("frost: Qt fatal error: "), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("no Qt platform plugin could be initialized"), std::string::npos) << result.output;
    EXPECT_EQ(result.output.find("selftest: "), std::string::npos) << result.output;
#ifndef _DEBUG
    EXPECT_NE(result.output.find("frost: refused to show a dialog in a command-line mode"), std::string::npos)
        << result.output;
#endif
    EXPECT_LT(std::chrono::steady_clock::now() - started, 30s);
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

// Length of `utf8` in UTF-16 code units, the unit Win32 paths are measured in.
std::size_t utf16Units(std::string_view utf8) { return frost::io::utf8ToUtf16(utf8).value().size(); }

// `base` extended by ASCII path components to exactly `units` UTF-16 code units.
// Precondition: units == utf16Units(base) or units >= utf16Units(base) + 2.
std::string padToLength(std::string base, std::size_t units) {
    while (utf16Units(base) < units) {
        // A separator and up to 100 units, never leaving exactly 1 (no room for a component).
        const std::size_t remaining = units - utf16Units(base);
        const std::size_t component = remaining - 1 <= 100 ? remaining - 1 : std::min<std::size_t>(100, remaining - 3);
        base = frost::io::joinPath(base, std::string(component, 'b')).value();
    }
    return base;
}

// The scratch directory's path in its long form, so that lengths built on it are what frost
// and the loader see (the temp directory may be named through an 8.3 component).
std::string longFormOf(const ScratchDir& scratch) {
    std::string base = convertedPath(scratch.path(), false);
    EXPECT_FALSE(base.empty()) << scratch.path();
    return base;
}

// Whether this test process's long paths are effective (LongPathsEnabled=1, and the test's
// manifest is longPathAware). Without them CreateProcessW refuses a plain executable path
// from MAX_PATH on, which the matrix then expects instead of a start.
bool parentLongPathsEffective() {
    using Query = BOOLEAN(NTAPI*)();
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto query =
        ntdll == nullptr
            ? nullptr
            : reinterpret_cast<Query>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlAreLongPathsEnabled")));
    return query != nullptr && query() != FALSE;
}

/*
 * Recreates the directory tree `from` (a copyFrostTo() result) at `to`, of any length, with
 * hard links instead of copies, so every cell of a length matrix gets a whole installation
 * without copying Qt's DLLs again. `from` and `to` must be on one volume. Returns the
 * executable's path in `to`.
 */
std::string linkTree(const std::string& from, const std::string& to) {
    std::error_code error;
    const fs::path source = anyLengthPath(from);
    const fs::path target = anyLengthPath(to);
    fs::create_directories(target, error);
    EXPECT_FALSE(error) << to << ": " << error.message();
    std::vector<fs::path> pending{fs::path()};
    while (!pending.empty()) {
        const fs::path relative = std::move(pending.back());
        pending.pop_back();
        for (const auto& entry : fs::directory_iterator(source / relative, error)) {
            const fs::path name = relative / entry.path().filename();
            if (entry.is_directory()) {
                fs::create_directories(target / name, error);
                pending.push_back(name);
            } else {
                fs::create_hard_link(entry.path(), target / name, error);
            }
            EXPECT_FALSE(error) << "link " << name.string() << ": " << error.message();
        }
        EXPECT_FALSE(error) << "list " << relative.string() << ": " << error.message();
    }
    return frost::io::joinPath(to, "frost.exe").value();
}

// The directory of exactly `units` UTF-16 units below `base` for a matrix cell: a first
// component with non-ASCII characters and spaces ("ä 测 x", one unit per character), then ASCII.
std::string cellDirectory(const std::string& base, std::size_t units) {
    return padToLength(frost::io::joinPath(base, "\xC3\xA4 \xE6\xB5\x8B x").value(), units);
}

// "plain" or "\\?\", for messages.
std::string_view formName(ImagePathForm form) { return form == ImagePathForm::Extended ? "\\\\?\\" : "plain"; }

// The directory lengths of StartsFromEveryDirectoryLength: around every boundary measured
// (the loader's 250-unit limit for DLLs that embed RT_MANIFEST 2, and MAX_PATH), and through
// the 218-249 range in which the single-image frost died before main in at least one
// configuration (ADR 0012), then up to 400.
constexpr std::size_t kMatrixLengths[] = {200, 217, 218, 219, 224, 225, 226, 230, 234, 235, 236,
                                          240, 245, 249, 250, 251, 255, 259, 260, 261, 300, 400};

TEST(FrostProcess, StartsFromEveryDirectoryLength) {
    /*
     * The M0 exit criterion at every length that has mattered, plain and \\?\: the whole
     * self-test must pass within 30 s each (a box frost failed to refuse would hold it until
     * the timeout) and see the form it was started through. Plain from MAX_PATH on needs this
     * test process's long paths (LongPathsEnabled=1); without them CreateProcessW's refusal is
     * expected there, which is what a parent on such a machine meets too.
     */
    const ScratchDir scratch("app-process-matrix");
    const std::string base = longFormOf(scratch);
    ASSERT_FALSE(base.empty());
    const std::string master = frost::io::joinPath(scratch.path(), "m").value();
    ASSERT_FALSE(copyFrostTo(master).empty());
    const bool parentLongPaths = parentLongPathsEffective();
    std::string failedCells;
    for (const std::size_t units : kMatrixLengths) {
        const std::string dir = cellDirectory(base, units);
        ASSERT_EQ(utf16Units(dir), units) << dir;
        const std::string exe = linkTree(master, dir);
        for (const ImagePathForm form : {ImagePathForm::Plain, ImagePathForm::Extended}) {
            SCOPED_TRACE(std::format("directory {} UTF-16 units, {} start", units, formName(form)));
            const auto result = runFrost(exe, {"--selftest"}, ownPluginsOnly(), form, 30s);
            const std::size_t exeUnits = units + std::string_view("\\frost.exe").size();
            if (form == ImagePathForm::Plain && exeUnits >= MAX_PATH && !parentLongPaths) {
                EXPECT_EQ(result.exitCode, -1) << result.output;
                EXPECT_FALSE(result.startError.empty())
                    << "CreateProcessW accepted a plain path of " << exeUnits << " units without effective long paths";
                continue;
            }
            const bool passed = result.exitCode == 0 && result.output.contains("selftest: PASSED");
            EXPECT_TRUE(passed) << result.startError << result.output;
            // The child saw the form it was started through.
            const std::string formSeen = form == ImagePathForm::Extended
                                             ? std::string("image-path: \\\\?\\ form, ")
                                             : std::format("image-path: plain form, {} UTF-16 units", exeUnits);
            EXPECT_TRUE(result.output.contains(formSeen)) << result.output;
            if (!passed) {
                failedCells += std::format(" {}/{}", units, formName(form));
            }
        }
    }
    EXPECT_EQ(failedCells, "") << "cells that did not pass:" << failedCells;
}

// Removes the longPathAware element from RT_MANIFEST 1 of the executable at `utf8` (a short
// path, edited in place), so the process it starts has long paths ineffective whatever
// LongPathsEnabled says: the state of a machine with the switch off. Records a test failure
// and returns false on error.
bool dropLongPathAwareness(const std::string& utf8) {
    const std::wstring exe = frost::io::toWide(utf8).value();
    std::string manifest;
    WORD language = 0;
    {
        HMODULE image = LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (image == nullptr) {
            ADD_FAILURE() << "cannot map " << utf8 << " (win32 error " << GetLastError() << ")";
            return false;
        }
        const HRSRC resource = FindResourceW(image, MAKEINTRESOURCEW(1), RT_MANIFEST);
        const HGLOBAL loaded = resource == nullptr ? nullptr : LoadResource(image, resource);
        if (loaded != nullptr) {
            manifest.assign(static_cast<const char*>(LockResource(loaded)), SizeofResource(image, resource));
        }
        EnumResourceLanguagesW(
            image, RT_MANIFEST, MAKEINTRESOURCEW(1),
            [](HMODULE, LPCWSTR, LPCWSTR, WORD found, LONG_PTR out) -> BOOL {
                *reinterpret_cast<WORD*>(out) = found;
                return FALSE; // The first language is the one the loader reads.
            },
            reinterpret_cast<LONG_PTR>(&language));
        FreeLibrary(image);
    }
    const std::size_t begin = manifest.find("<longPathAware");
    const std::string_view close = "</longPathAware>";
    const std::size_t end = manifest.find(close);
    if (begin == std::string::npos || end == std::string::npos || end < begin) {
        ADD_FAILURE() << utf8 << " has no longPathAware element to remove:\n" << manifest;
        return false;
    }
    manifest.erase(begin, end + close.size() - begin);
    HANDLE update = BeginUpdateResourceW(exe.c_str(), FALSE);
    if (update == nullptr) {
        ADD_FAILURE() << "BeginUpdateResourceW failed (win32 error " << GetLastError() << ")";
        return false;
    }
    const bool updated = UpdateResourceW(update, RT_MANIFEST, MAKEINTRESOURCEW(1), language, manifest.data(),
                                         static_cast<DWORD>(manifest.size())) != FALSE;
    if (EndUpdateResourceW(update, updated ? FALSE : TRUE) == FALSE || !updated) {
        ADD_FAILURE() << "the manifest of " << utf8 << " could not be rewritten (win32 error " << GetLastError() << ")";
        return false;
    }
    return true;
}

TEST(FrostProcess, StartsWithLongPathsIneffective) {
    /*
     * The design's claim for machines with LongPathsEnabled=0, simulated here, where the
     * switch is on: frost.exe's manifest copy without longPathAware makes long paths
     * ineffective for frost alone (only that copy is edited, at a short path; no deployed DLL
     * is touched). frost must still start and pass every check but long-path-manifest, which
     * reports the edited manifest. Plain up to 249 units (a parent without long paths could
     * not start a longer plain path), \\?\ up to 400.
     */
    const ScratchDir scratch("app-process-nolp");
    const std::string base = longFormOf(scratch);
    ASSERT_FALSE(base.empty());
    const std::string master = frost::io::joinPath(scratch.path(), "m").value();
    const std::string masterExe = copyFrostTo(master);
    ASSERT_TRUE(dropLongPathAwareness(masterExe));
    constexpr std::size_t kLengths[] = {200, 218, 225, 236, 249, 260, 300, 400};
    for (const std::size_t units : kLengths) {
        const std::string exe = linkTree(master, cellDirectory(base, units));
        for (const ImagePathForm form : {ImagePathForm::Plain, ImagePathForm::Extended}) {
            if (form == ImagePathForm::Plain && units >= 250) {
                continue;
            }
            SCOPED_TRACE(
                std::format("long paths ineffective, directory {} UTF-16 units, {} start", units, formName(form)));
            const auto result = runFrost(exe, {"--selftest"}, ownPluginsOnly(), form, 30s);
            EXPECT_EQ(result.exitCode, 1) << result.startError << result.output;
            EXPECT_TRUE(result.output.contains("selftest: FAIL long-path-manifest: ")) << result.output;
            EXPECT_TRUE(result.output.contains("selftest: FAILED (1 required checks)")) << result.output;
            EXPECT_TRUE(result.output.contains("selftest: warn long-paths-effective: not effective for this process"))
                << result.output;
            EXPECT_TRUE(result.output.contains("ok   plugin-paths: loaded Qt's platform plugin ")) << result.output;
            EXPECT_TRUE(result.output.contains("ok   main-window: main window exposed and closed")) << result.output;
        }
    }
}

// A directory below `scratch` whose path is longer than MAX_PATH (about 300 units), with
// non-ASCII characters and spaces in its first component.
std::string longDirectoryIn(const ScratchDir& scratch) {
    std::string dir = frost::io::joinPath(scratch.path(), "frost ünïcødé 测试").value();
    while (dir.size() < 300) {
        dir = frost::io::joinPath(dir, std::string(40, 'x') + " segment").value();
    }
    return dir;
}

TEST(FrostProcess, RunsFromPathLongerThanMaxPath) {
    /*
     * PLAN.md's M0 exit criterion, as a user meets it: a directory beyond MAX_PATH with
     * non-ASCII characters and spaces, started the way frost::io picks (\\?\ from MAX_PATH on)
     * and plain. No skip on LongPathsEnabled: frost loads its DLLs and plugins through \\?\
     * paths, so the switch does not matter (StartsWithLongPathsIneffective); only a plain start
     * needs the parent's long paths, and without them CreateProcessW's refusal is expected.
     */
    const ScratchDir scratch("app-process-long");
    const std::string dir = longDirectoryIn(scratch);
    ASSERT_GT(utf16Units(dir), 260u);
    const std::string exe = copyFrostTo(dir);
    for (const ImagePathForm form : {ImagePathForm::Auto, ImagePathForm::Plain}) {
        SCOPED_TRACE(form == ImagePathForm::Auto ? "frost::io's choice" : "plain");
        const auto result = runFrost(exe, {"--selftest"}, {}, form);
        if (form == ImagePathForm::Plain && !parentLongPathsEffective()) {
            EXPECT_EQ(result.exitCode, -1) << result.output;
            continue;
        }
        EXPECT_EQ(result.exitCode, 0) << result.startError << result.output;
        EXPECT_TRUE(result.output.contains("selftest: PASSED")) << result.output;
    }
}

TEST(FrostProcess, RunsThroughShortNamesOfALongPath) {
    /*
     * Started through the 8.3 short name of a directory beyond MAX_PATH, frost sees its own
     * path in that form (GetModuleFileNameW reports what the parent passed) and loads its
     * DLLs and the platform plugin through it, in \\?\ form. Skips only where the volume
     * generates no short names.
     */
    const ScratchDir scratch("app-process-short");
    const std::string exe = copyFrostTo(longDirectoryIn(scratch));
    const std::string shortExe = convertedPath(exe, true);
    if (shortExe.empty() || !shortExe.contains('~')) {
        GTEST_SKIP() << "the temp directory's volume does not generate 8.3 short names (got '" << shortExe << "')";
    }
    const auto started = std::chrono::steady_clock::now();
    const auto result = runFrost(shortExe, {"--selftest"}, ownPluginsOnly());
    EXPECT_EQ(result.exitCode, 0) << result.startError << result.output;
    EXPECT_NE(result.output.find("selftest: PASSED"), std::string::npos) << result.output;
    // The pin used the short form.
    EXPECT_NE(result.output.find("ok   plugin-paths: loaded Qt's platform plugin "), std::string::npos)
        << result.output;
    EXPECT_NE(result.output.find("~1\\platforms\\"), std::string::npos) << result.output;
    EXPECT_LT(std::chrono::steady_clock::now() - started, 30s);
}

TEST(FrostProcess, LateLoadOfADllBesideTheExeWorksFromALongPlainPath) {
    /*
     * A DLL beside frost.exe that nothing loaded at start-up, as Qt loads its optional
     * modules, loaded later by bare name. From a long plain start, the default search would
     * combine the plain install directory with the name and fail (win32 error 206); the
     * launcher made the \\?\ install directory the process default, so it loads in \\?\ form.
     * Qt6Network embeds RT_MANIFEST 2 like Qt's other DLLs. 300 units, plain; a \\?\ start if
     * this process's long paths are ineffective, because a plain one would then be refused.
     */
    const ScratchDir scratch("app-process-lateload");
    const std::string dir = cellDirectory(longFormOf(scratch), 300);
    const std::string exe = copyFrostTo(dir);
    copyInto(sdkFile(FROST_QT_BIN_DIR, qtDll("Qt6Network")), dir, qtDll("Qt6Network"));
    const auto form = parentLongPathsEffective() ? ImagePathForm::Plain : ImagePathForm::Extended;
    const auto result = runFrost(exe, {"--selftest"}, ownPluginsOnly(), form, 30s);
    EXPECT_EQ(result.exitCode, 0) << result.startError << result.output;
    EXPECT_TRUE(
        result.output.contains(std::format("ok   late-load: loaded by name: {} (\\\\?\\ form)", qtDll("Qt6Network"))))
        << result.output;
}

TEST(FrostProcess, PreloadsOtherPluginsFromALongPath) {
    /*
     * A non-platform plugin with a dependency (imageformats\qsvg, which imports Qt6Svg) in a
     * directory whose plugin paths pass the loader's 250-unit limit: frost loads it through
     * \\?\ before Qt would load it by its plain path, and the dependency comes from the \\?\
     * install directory.
     */
    const ScratchDir scratch("app-process-plugins");
    const std::string dir = cellDirectory(longFormOf(scratch), 300);
    const std::string exe = copyFrostTo(dir);
    ASSERT_TRUE(fs::create_directories(anyLengthPath(dir) / "imageformats"));
    copyInto(sdkFile(FROST_QT_PLUGIN_DIR, "imageformats\\" + qtDll("qsvg")), dir, "imageformats\\" + qtDll("qsvg"));
    copyInto(sdkFile(FROST_QT_BIN_DIR, qtDll("Qt6Svg")), dir, qtDll("Qt6Svg"));
    const auto form = parentLongPathsEffective() ? ImagePathForm::Plain : ImagePathForm::Extended;
    const auto result = runFrost(exe, {"--selftest"}, ownPluginsOnly(), form, 30s);
    EXPECT_EQ(result.exitCode, 0) << result.startError << result.output;
    EXPECT_TRUE(result.output.contains("ok   other-plugins: loaded 1 of 1 plugin(s) through \\\\?\\")) << result.output;
    EXPECT_TRUE(result.output.contains("selftest: PASSED")) << result.output;
}

// Runs a copy of frost from which `missing` (a file beside frost.exe) was removed, in a
// command-line mode, and checks the launcher's report: status 4 at once, the reason on
// stderr, and no box (the run would otherwise wait on the private desktop until the timeout).
void expectLauncherFailure(const std::string& missing) {
    const ScratchDir scratch("app-process-launcher");
    const std::string dir = scratch.join("fr\xC3\xB8st launcher");
    const std::string exe = copyFrostTo(dir);
    std::error_code error;
    ASSERT_TRUE(fs::remove(anyLengthPath(dir) / frost::io::toStdPath(missing).value(), error)) << error.message();
    const auto started = std::chrono::steady_clock::now();
    const auto result = runFrost(exe, {"--version"}, {}, ImagePathForm::Auto, 30s);
    EXPECT_EQ(result.exitCode, 4) << result.startError << result.output;
    const std::string expected = "frost: cannot load " + frost::io::joinPath(dir, "frost_app.dll").value() + ": ";
    EXPECT_TRUE(result.output.contains(expected)) << "expected " << expected << "\n" << result.output;
    EXPECT_TRUE(result.output.contains("(win32 error 126)")) << result.output;
    EXPECT_LT(std::chrono::steady_clock::now() - started, 30s);
}

TEST(FrostProcess, LauncherReportsMissingAppDll) { expectLauncherFailure("frost_app.dll"); }

TEST(FrostProcess, LauncherReportsMissingQtDll) {
    // frost_app.dll's import is missing: the launcher reports its own load failure, where the
    // single-image frost died in the loader with 0xC0000135 before any code ran.
    expectLauncherFailure(qtDll("Qt6Widgets"));
}

/*
 * A PE image of the build, mapped for reading (LOAD_LIBRARY_AS_IMAGE_RESOURCE: sections at
 * their RVAs, no imports resolved, no code run) and unmapped on destruction.
 */
class MappedImage {
public:
    explicit MappedImage(const std::string& utf8)
        : m_module(LoadLibraryExW(frost::io::toWin32Path(utf8).value().c_str(), nullptr,
                                  LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
        if (m_module == nullptr) {
            ADD_FAILURE() << "cannot map " << utf8 << " (win32 error " << GetLastError() << ")";
        }
    }
    ~MappedImage() {
        if (m_module != nullptr) {
            FreeLibrary(m_module);
        }
    }
    MappedImage(const MappedImage&) = delete ("unmaps exactly once");
    MappedImage& operator=(const MappedImage&) = delete ("unmaps exactly once");

    [[nodiscard]] bool valid() const noexcept { return m_module != nullptr; }
    // The handle for the resource APIs, which accept a data-file handle as it is.
    [[nodiscard]] HMODULE module() const noexcept { return m_module; }

    // The image's NT headers; the handle's low bits mark the data-file mapping.
    [[nodiscard]] const IMAGE_NT_HEADERS& headers() const {
        const auto* dos = at<IMAGE_DOS_HEADER>(0);
        return *at<IMAGE_NT_HEADERS>(static_cast<DWORD>(dos->e_lfanew));
    }
    [[nodiscard]] IMAGE_DATA_DIRECTORY directory(unsigned index) const {
        const IMAGE_OPTIONAL_HEADER& optional = headers().OptionalHeader;
        return index < optional.NumberOfRvaAndSizes ? optional.DataDirectory[index] : IMAGE_DATA_DIRECTORY{};
    }
    template<typename T>
    [[nodiscard]] const T* at(DWORD rva) const {
        const auto base = reinterpret_cast<std::uintptr_t>(m_module) & ~std::uintptr_t{3};
        return reinterpret_cast<const T*>(base + rva);
    }

private:
    HMODULE m_module;
};

// The built frost_app.dll beside frost.exe.
std::string appDll() { return frost::io::joinPath(frost::io::parentPath(frostExe()), "frost_app.dll").value(); }

// The DLL names of the image's import directory, lower-cased.
std::vector<std::string> importedDlls(const MappedImage& image) {
    std::vector<std::string> names;
    const IMAGE_DATA_DIRECTORY entry = image.directory(IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (entry.VirtualAddress == 0) {
        return names;
    }
    for (const auto* descriptor = image.at<IMAGE_IMPORT_DESCRIPTOR>(entry.VirtualAddress); descriptor->Name != 0;
         ++descriptor) {
        std::string name = image.at<char>(descriptor->Name);
        std::ranges::transform(name, name.begin(),
                               [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; });
        names.push_back(std::move(name));
    }
    return names;
}

// The DependentLoadFlags of the image's load configuration, 0 if it has none.
DWORD dependentLoadFlags(const MappedImage& image) {
    const IMAGE_DATA_DIRECTORY entry = image.directory(IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG);
    if (entry.VirtualAddress == 0) {
        return 0;
    }
    const auto* config = image.at<IMAGE_LOAD_CONFIG_DIRECTORY>(entry.VirtualAddress);
    if (config->Size < offsetof(IMAGE_LOAD_CONFIG_DIRECTORY, DependentLoadFlags) + sizeof(WORD)) {
        return 0;
    }
    return config->DependentLoadFlags;
}

// The integer names of the image's RT_MANIFEST resources (named ones as 0).
std::vector<unsigned> manifestIds(const MappedImage& image) {
    std::vector<unsigned> ids;
    EnumResourceNamesW(
        image.module(), RT_MANIFEST,
        [](HMODULE, LPCWSTR, LPWSTR name, LONG_PTR out) -> BOOL {
            reinterpret_cast<std::vector<unsigned>*>(out)->push_back(
                IS_INTRESOURCE(name) ? static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(name)) : 0u);
            return TRUE;
        },
        reinterpret_cast<LONG_PTR>(&ids));
    return ids;
}

TEST(FrostProcess, LauncherImportsOnlyKernel32) {
    /*
     * frost.exe's static imports are exactly KERNEL32.dll, in every configuration and in the
     * ASan tree: a DLL that embeds RT_MANIFEST 2 among them (the UCRT's api-set forwarders
     * resolve to System32, but Qt, the debug CRT or the ASan runtime would not) brings back the
     * loader's kill before main on long plain starts (ADR 0012). No delay-loaded imports
     * either, and the GUI subsystem, so a start from Explorer opens no console.
     */
    const MappedImage exe(frostExe());
    ASSERT_TRUE(exe.valid());
    EXPECT_EQ(importedDlls(exe), std::vector<std::string>{"kernel32.dll"});
    const IMAGE_DATA_DIRECTORY delay = exe.directory(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    EXPECT_TRUE(delay.VirtualAddress == 0 || exe.at<IMAGE_DELAYLOAD_DESCRIPTOR>(delay.VirtualAddress)->DllNameRVA == 0);
    EXPECT_EQ(exe.headers().OptionalHeader.Subsystem, IMAGE_SUBSYSTEM_WINDOWS_GUI);
    EXPECT_EQ(dependentLoadFlags(exe), 0xA00u);
    // The process manifest is resource 1 of frost.exe alone.
    EXPECT_EQ(manifestIds(exe), std::vector<unsigned>{1u});
}

TEST(FrostProcess, AppDllHasNoDependentLoadFlags) {
    // A /DEPENDENTLOADFLAG on frost_app.dll would replace the launcher's LoadLibraryExW flags
    // for its imports; 0xA00 there broke plain starts from 236 units in the spike.
    const MappedImage app(appDll());
    ASSERT_TRUE(app.valid());
    const DWORD flags = dependentLoadFlags(app);
    EXPECT_TRUE(flags == 0 || flags == 0x900) << std::format("{:#x}", flags);
}

TEST(FrostProcess, AppDllEmbedsNoManifest) {
    // An RT_MANIFEST 2 (the linker's default for a DLL) would subject frost_app.dll to the
    // loader's 250-unit plain-path limit, and an isolation context could change how its
    // imports resolve.
    const MappedImage app(appDll());
    ASSERT_TRUE(app.valid());
    EXPECT_EQ(manifestIds(app), std::vector<unsigned>{});
}

#endif

} // namespace
