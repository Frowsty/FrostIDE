// SPDX-License-Identifier: Apache-2.0
//
// Integration tests that run the real frost program (PLAN.md section 7, M0 exit): --version,
// --help, error exit codes, the built-in self-test (also that arguments after "--" never reach
// Qt), and launching from a directory whose path has non-ASCII characters and spaces. On
// Windows also the M0 exit criterion's long paths (ADR 0012): a start from 22 directory
// lengths from 200 to 400 UTF-16 units around every boundary measured, plain and \\?\, also
// with long paths ineffective for frost (a manifest without longPathAware, the state
// LongPathsEnabled=0 gives), and through 8.3 short names; the self-test with Qt's native
// platform plugin (a GPU driver loads) from image paths of 250 to 400 units, through the
// launcher's restart via a short junction from 260 units on, and the GUI itself from 300
// units (its window, and its failure box naming the real install directory); that other modes
// start in place; the restart's exit status and standard streams, its loop guard, the check
// of its marker and its refusals, with the link below a non-ASCII LOCALAPPDATA; a DLL loaded
// late and non-platform plugins from a long plain path (loaded, skipped or failing); the
// start-up check's reports when Qt's platform plugin is missing or
// unloadable, and that Qt's own "no platform plugin" dialog never appears in a command-line
// mode; the launcher's failure reports and the error mode it loads in; and the structure of
// the two images: frost.exe imports KERNEL32.dll alone, and frost_app.dll carries no manifest
// and no DependentLoadFlags and imports only allowed DLLs. Crash and freeze reports (ADR 0022):
// --selftest=crash and --selftest=freeze write a minidump and a freeze report (left in
// selftest-artifacts/ for verify -Full's symbolize step, with crash_child's contract-violation
// dump), the crash self-test works from every directory length, and the GUI starts its crash
// handler only after the first frame, through the short link after a restart, and never
// leaves it running after frost ends.
// Processes are started through frost::io's production IProcessLauncher, on Windows on a
// private desktop each and with hard-error boxes off (runFrost).

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/io/utf.hpp"

#include "diag/diag_test_support.hpp"
#include "support/minidump_reader.hpp"
#include "support/private_desktop.hpp"
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <tlhelp32.h>
#else
#include <csignal>

#include <signal.h>
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
    // POSIX: the signal that ended the child, or 0.
    int signal = 0;
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
    return {result->exitCode, std::move(result->stdoutBytes), {}, result->signal};
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
    // The crash handler frost starts lives beside the program (ADR 0022).
    const fs::path handler = exe.parent_path() / (std::string("crashpad_handler") + exe.extension().string());
    fs::copy_file(handler, target / handler.filename(), fs::copy_options::overwrite_existing, error);
    EXPECT_FALSE(error) << "copy " << handler.string() << ": " << error.message();
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
     * install directory. Beside it, the same plugin of Qt's other build (debug or release,
     * whose Qt6Svg is not deployed) and a DLL that is no Qt plugin (System32's version.dll):
     * frost loads neither, because neither imports this build's Qt6Core.
     */
    const ScratchDir scratch("app-process-plugins");
    const std::string dir = cellDirectory(longFormOf(scratch), 300);
    const std::string exe = copyFrostTo(dir);
    ASSERT_TRUE(fs::create_directories(anyLengthPath(dir) / "imageformats"));
    copyInto(sdkFile(FROST_QT_PLUGIN_DIR, "imageformats\\" + qtDll("qsvg")), dir, "imageformats\\" + qtDll("qsvg"));
    copyInto(sdkFile(FROST_QT_BIN_DIR, qtDll("Qt6Svg")), dir, qtDll("Qt6Svg"));
    const std::string otherBuild = kQtSuffix.empty() ? "qsvgd.dll" : "qsvg.dll";
    copyInto(sdkFile(FROST_QT_PLUGIN_DIR, "imageformats\\" + otherBuild), dir, "imageformats\\" + otherBuild);
    wchar_t system[MAX_PATH];
    ASSERT_NE(GetSystemDirectoryW(system, MAX_PATH), 0u);
    copyInto(fs::path(system) / L"version.dll", dir, "imageformats\\frost_not_a_plugin.dll");
    const auto form = parentLongPathsEffective() ? ImagePathForm::Plain : ImagePathForm::Extended;
    const auto result = runFrost(exe, {"--selftest"}, ownPluginsOnly(), form, 30s);
    EXPECT_EQ(result.exitCode, 0) << result.startError << result.output;
    EXPECT_TRUE(result.output.contains("ok   other-plugins: loaded 1 of 1 plugin(s) through \\\\?\\")) << result.output;
    // Qt's other build of the same plugin (it would bring in the other Qt6Core) and a DLL that
    // is no Qt plugin are named and left alone.
    EXPECT_TRUE(result.output.contains(std::format("; skipped, as they do not import {}: ", qtDll("Qt6Core"))))
        << result.output;
    EXPECT_TRUE(result.output.contains("imageformats\\" + otherBuild)) << result.output;
    EXPECT_TRUE(result.output.contains("imageformats\\frost_not_a_plugin.dll")) << result.output;
    EXPECT_FALSE(result.output.contains("Qt will skip")) << result.output;
    EXPECT_TRUE(result.output.contains("selftest: PASSED")) << result.output;
}

TEST(FrostProcess, PreloadFailureOnlyWarns) {
    /*
     * A non-platform plugin that cannot be loaded (named like one, but not a PE image) in a
     * directory whose plugin paths pass the loader's limit: the self-test names it in its
     * other-plugins warning and still passes, because Qt would skip that plugin too and frost
     * must start without it.
     */
    const ScratchDir scratch("app-process-badpreload");
    const std::string dir = cellDirectory(longFormOf(scratch), 300);
    const std::string exe = copyFrostTo(dir);
    ASSERT_TRUE(fs::create_directories(anyLengthPath(dir) / "imageformats"));
    const std::string broken = "imageformats\\" + qtDll("qbroken");
    {
        std::ofstream out(anyLengthPath(dir) / frost::io::toStdPath(broken).value(),
                          std::ios::binary | std::ios::trunc);
        out << std::string(4096, 'x');
    }
    const auto form = parentLongPathsEffective() ? ImagePathForm::Plain : ImagePathForm::Extended;
    const auto result = runFrost(exe, {"--selftest"}, ownPluginsOnly(), form, 30s);
    EXPECT_EQ(result.exitCode, 0) << result.startError << result.output;
    EXPECT_TRUE(result.output.contains("selftest: warn other-plugins: loaded 0 of 1 plugin(s) through \\\\?\\"))
        << result.output;
    EXPECT_TRUE(result.output.contains("; Qt will skip " + broken + " (")) << result.output;
    EXPECT_TRUE(result.output.contains("win32 error 193)")) << result.output;
    EXPECT_TRUE(result.output.contains("selftest: PASSED")) << result.output;
}

// The directory for a copy of frost whose image path, <dir>\frost.exe, is exactly `units`
// UTF-16 units long when started in `form` (the \\?\ prefix counts, for the GPU driver too).
std::string imagePathCell(const std::string& base, std::size_t units, ImagePathForm form) {
    const std::size_t prefix = form == ImagePathForm::Extended ? 4 : 0;
    return cellDirectory(base, units - prefix - std::string_view("\\frost.exe").size());
}

// The image path a process started from `exe` in `form` reports (GetModuleFileNameW gives
// the path its parent passed, prefix included).
std::string startedAs(const std::string& exe, ImagePathForm form) {
    return form == ImagePathForm::Extended ? "\\\\?\\" + exe : exe;
}

// The \\?\ form of the absolute path `utf8`, in UTF-16, for Win32 calls at any length.
std::wstring extendedWide(const std::string& utf8) {
    return frost::io::toWide(frost::io::extendedLengthPath(utf8).value()).value();
}

// An entry of the launcher's short-link folder.
struct ShortLinkEntry {
    std::string path;      // <LOCALAPPDATA>\FrostIDE\i\<name>.
    bool junction = false; // Whether it is a mount point (FindFirstFileExW's reparse tag).
};

// The entries of <localAppData>\FrostIDE\i, where the launcher keeps its short links; empty
// when that folder does not exist. Links are listed, never followed.
std::vector<ShortLinkEntry> shortLinksIn(const std::string& localAppData) {
    const std::string folder = frost::io::joinPath(localAppData, "FrostIDE\\i").value();
    std::vector<ShortLinkEntry> found;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileExW((extendedWide(folder) + L"\\*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) {
        return found;
    }
    do {
        const std::wstring name = data.cFileName;
        if (name != L"." && name != L"..") {
            found.push_back({frost::io::joinPath(folder, frost::io::fromWide(name).value()).value(),
                             (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
                                 data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT});
        }
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
    return found;
}

// Whether <localAppData>\FrostIDE exists: a start that needs no restart must not create it.
bool shortLinkFolderExists(const std::string& localAppData) {
    return GetFileAttributesW(extendedWide(frost::io::joinPath(localAppData, "FrostIDE").value()).c_str()) !=
           INVALID_FILE_ATTRIBUTES;
}

// A fresh directory below `scratch` to stand in for %LOCALAPPDATA%: `name` and " ä 测", so the
// link always lives below a profile path with spaces and non-ASCII characters (PLAN.md's M0
// fixture).
std::string localAppDataIn(const ScratchDir& scratch, std::string_view name) {
    const std::string dir = scratch.join(std::string(name) + " \xC3\xA4 \xE6\xB5\x8B");
    EXPECT_TRUE(frost::io::realFileSystem().createDirectories(dir).has_value()) << dir;
    return dir;
}

// The form a plain start of a long path needs: plain where this process's long paths are
// effective, else \\?\ (CreateProcessW would refuse the plain path).
ImagePathForm longStartForm() { return parentLongPathsEffective() ? ImagePathForm::Plain : ImagePathForm::Extended; }

// "plain form" or "\\?\ form", as the self-test's image-path check names `form`.
std::string_view reportedForm(ImagePathForm form) {
    return form == ImagePathForm::Extended ? "\\\\?\\ form" : "plain form";
}

TEST(FrostProcess, SelfTestOnScreenFromEveryImagePathLength) {
    /*
     * The self-test with Qt's native platform plugin (windows), as the GUI starts: Qt loads
     * qwindows.dll, which creates a Direct3D device, so the GPU's user-mode driver loads, and a
     * real window opens on this test's private desktop, never on the user's screen. NVIDIA's
     * driver ends any process whose image path is 260 UTF-16 units (MAX_PATH) or longer as it
     * loads (0xC0000409, FAST_FAIL_INVALID_ARG in nvldumdx.dll's DllMain; a python.exe control
     * dies the same way; ADR 0012). From 260 units on the launcher therefore restarts frost
     * through its short link under %LOCALAPPDATA% (here a scratch directory per cell), and the
     * self-test there must pass and name the path it was restarted from; below 260 frost starts
     * in place and creates no link. Image paths of 250, 259, 260, 261, 300 and 400 units, plain
     * and \\?\ (the prefix counts), and the build directory. Plain from MAX_PATH on needs this
     * process's long paths; without them CreateProcessW's refusal is expected there.
     */
    const ScratchDir scratch("app-process-onscreen");
    const std::string base = longFormOf(scratch);
    ASSERT_FALSE(base.empty());
    const std::string master = frost::io::joinPath(scratch.path(), "m").value();
    ASSERT_FALSE(copyFrostTo(master).empty());
    struct Cell {
        std::string exe;
        ImagePathForm form;
        std::size_t units; // The image path's length as started, or 0 for the build directory.
    };
    std::vector<Cell> cells{{frostExe(), ImagePathForm::Auto, 0}};
    for (const std::size_t units :
         {std::size_t{250}, std::size_t{259}, std::size_t{260}, std::size_t{261}, std::size_t{300}, std::size_t{400}}) {
        for (const ImagePathForm form : {ImagePathForm::Plain, ImagePathForm::Extended}) {
            cells.push_back({linkTree(master, imagePathCell(base, units, form)), form, units});
        }
    }
    for (std::size_t index = 0; index < cells.size(); ++index) {
        const Cell& cell = cells[index];
        SCOPED_TRACE(cell.units == 0 ? std::string("build directory")
                                     : std::format("{}, {} units", formName(cell.form), cell.units));
        const std::string localAppData = localAppDataIn(scratch, std::format("lad {}", index));
        Environment environment = ownPluginsOnly();
        environment.emplace_back("LOCALAPPDATA", localAppData);
        const auto result = runFrost(cell.exe, {"--selftest=onscreen"}, environment, cell.form, 60s);
        if (cell.form == ImagePathForm::Plain && cell.units >= MAX_PATH && !parentLongPathsEffective()) {
            EXPECT_EQ(result.exitCode, -1) << result.output;
            continue;
        }
        EXPECT_EQ(result.exitCode, 0) << result.startError << result.output;
        EXPECT_TRUE(result.output.contains("platform plugin 'windows'")) << result.output;
        EXPECT_TRUE(result.output.contains("ok   main-window: main window exposed and closed")) << result.output;
        EXPECT_TRUE(result.output.contains("selftest: PASSED")) << result.output;
        if (cell.units < MAX_PATH) {
            EXPECT_FALSE(shortLinkFolderExists(localAppData));
            EXPECT_FALSE(result.output.contains("restarted through")) << result.output;
            if (cell.units != 0) {
                EXPECT_TRUE(result.output.contains(
                    std::format("image-path: {}, {} UTF-16 units", reportedForm(cell.form), cell.units)))
                    << result.output;
            }
            continue;
        }
        const std::vector<ShortLinkEntry> links = shortLinksIn(localAppData);
        ASSERT_EQ(links.size(), 1u) << result.output;
        EXPECT_TRUE(links.front().junction);
        // The restarted frost runs from the link, in plain form, and names the first path.
        const std::string expected = std::format(
            "image-path: plain form, {} UTF-16 units; restarted through this short link from {} ({}, {} UTF-16 units)",
            utf16Units(links.front().path) + std::string_view("\\frost.exe").size(), startedAs(cell.exe, cell.form),
            reportedForm(cell.form), cell.units);
        EXPECT_TRUE(result.output.contains(expected)) << "expected " << expected << "\n" << result.output;
        EXPECT_TRUE(result.output.contains(links.front().path + "\\platforms\\")) << result.output;
    }
}

TEST(FrostProcess, OtherModesStartInPlaceFromALongPath) {
    /*
     * Only a start that may load a GPU driver restarts (launcherMayReachGpuDriver();
     * StartupGpuDriverRule has the whole rule): --version, --help, a usage error and the
     * offscreen self-test start in place from a 300-unit image path, see that path themselves
     * and create no short link.
     */
    const ScratchDir scratch("app-process-inplace");
    const ImagePathForm form = longStartForm();
    const std::string exe = copyFrostTo(imagePathCell(longFormOf(scratch), 300, form));
    const std::string localAppData = localAppDataIn(scratch, "lad");
    Environment environment = ownPluginsOnly();
    environment.emplace_back("LOCALAPPDATA", localAppData);
    struct Case {
        std::vector<std::string> args;
        int status;
        std::string expected;
    };
    const std::vector<Case> cases{
        {{"--version"}, 0, "FrostIDE 0.0.1"},
        {{"--help"}, 0, "Usage: frost"},
        {{"--no-such-option"}, 2, "unknown option --no-such-option"},
        {{"--selftest"}, 0, std::format("image-path: {}, 300 UTF-16 units", reportedForm(form))}};
    for (const Case& c : cases) {
        SCOPED_TRACE(c.args.front());
        const auto result = runFrost(exe, c.args, environment, form, 60s);
        EXPECT_EQ(result.exitCode, c.status) << result.startError << result.output;
        EXPECT_TRUE(result.output.contains(c.expected)) << result.output;
        EXPECT_FALSE(result.output.contains("restarted through")) << result.output;
        EXPECT_FALSE(shortLinkFolderExists(localAppData));
    }
}

TEST(FrostProcess, RestartReportsTheRestartedProgramsFailure) {
    /*
     * A restarted frost that fails reaches the parent as a direct start would: from a 300-unit
     * image path without platforms\, the restarted self-test's plugin check fails promptly,
     * and its status (1) and report come back through the waiting launcher.
     */
    const ScratchDir scratch("app-process-restartfail");
    const ImagePathForm form = longStartForm();
    const std::string exe = copyFrostTo(imagePathCell(longFormOf(scratch), 300, form), Plugins::None);
    Environment environment = ownPluginsOnly();
    environment.emplace_back("LOCALAPPDATA", localAppDataIn(scratch, "lad"));
    const auto started = std::chrono::steady_clock::now();
    const auto result = runFrost(exe, {"--selftest=onscreen"}, environment, form, 60s);
    expectPluginCheckFailure(result, started, "Qt's platform plugin \"windows\" is missing");
    EXPECT_TRUE(result.output.contains("; restarted through this short link from " + startedAs(exe, form)))
        << result.output;
}

// Runs a copy of frost from which `missing` (a file beside frost.exe) was removed, in a
// command-line mode, and checks the launcher's report: status 4 at once, the reason on
// stderr, and no box (the run would otherwise wait on the private desktop until the timeout).
// `expectedPath` is how the report names what failed.
void expectLauncherFailure(const std::string& missing, std::string_view expectedPath) {
    const ScratchDir scratch("app-process-launcher");
    const std::string dir = scratch.join("fr\xC3\xB8st launcher");
    const std::string exe = copyFrostTo(dir);
    std::error_code error;
    ASSERT_TRUE(fs::remove(anyLengthPath(dir) / frost::io::toStdPath(missing).value(), error)) << error.message();
    const auto started = std::chrono::steady_clock::now();
    const auto result = runFrost(exe, {"--version"}, {}, ImagePathForm::Auto, 30s);
    EXPECT_EQ(result.exitCode, 4) << result.startError << result.output;
    const std::string expected =
        "frost: cannot load " + frost::io::joinPath(dir, "frost_app.dll").value() + std::string(expectedPath) + ": ";
    EXPECT_TRUE(result.output.contains(expected)) << "expected " << expected << "\n" << result.output;
    EXPECT_TRUE(result.output.contains("(win32 error 126)")) << result.output;
    EXPECT_LT(std::chrono::steady_clock::now() - started, 30s);
}

TEST(FrostProcess, LauncherReportsMissingAppDll) { expectLauncherFailure("frost_app.dll", ""); }

TEST(FrostProcess, LauncherReportsMissingQtDll) {
    // frost_app.dll's import is missing: the launcher reports its own load failure, where the
    // single-image frost died in the loader with 0xC0000135 before any code ran. The loader
    // does not say which import failed, so the report says that frost_app.dll is there.
    expectLauncherFailure(qtDll("Qt6Widgets"), " or a DLL it needs");
}

/*
 * While an instance lives, this process's error mode is exactly `mode`, so a child started
 * meanwhile inherits that mode and nothing else; the destructor restores the previous mode.
 * Same threading rule as frost::test::NoHardErrorBoxes.
 */
class InheritedErrorMode {
public:
    explicit InheritedErrorMode(UINT mode) noexcept : m_previous(SetErrorMode(mode)) {}
    ~InheritedErrorMode() { SetErrorMode(m_previous); }
    InheritedErrorMode(const InheritedErrorMode&) = delete ("restores the error mode once");
    InheritedErrorMode& operator=(const InheritedErrorMode&) = delete ("restores the error mode once");

private:
    UINT m_previous;
};

// Copies the built frost.exe and launcher_probe_app.cpp's DLL, as frost_app.dll, into `dir`
// (created), and in ASan trees the ASan runtime the launcher loads from its directory first.
// Returns the copied frost.exe's path.
std::string copyProbeTo(const std::string& dir) {
    EXPECT_TRUE(frost::io::realFileSystem().createDirectories(dir).has_value()) << dir;
    const fs::path built = frost::io::toStdPath(frostExe()).value();
    copyInto(built, dir, "frost.exe");
    copyInto(frost::io::toStdPath(frost::io::normalizePath(FROST_LAUNCHER_PROBE_APP).value()).value(), dir,
             "frost_app.dll");
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(built.parent_path(), error)) {
        if (entry.path().filename().wstring().starts_with(L"clang_rt.asan")) {
            copyInto(entry.path(), dir, frost::io::fromStdPath(entry.path().filename()).value());
        }
    }
    return frost::io::joinPath(dir, "frost.exe").value();
}

// The hex number after `label` in `text` ("... at load 0x8003, ..."), or nullopt.
std::optional<unsigned> hexAfter(std::string_view text, std::string_view label) {
    const std::size_t at = text.find(label);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    const char* begin = text.data() + at + label.size();
    unsigned value = 0;
    const auto [end, error] = std::from_chars(begin, text.data() + text.size(), value, 16);
    return error == std::errc() && end != begin ? std::optional<unsigned>(value) : std::nullopt;
}

TEST(FrostProcess, LauncherSetsTheErrorModeForItsLoads) {
    /*
     * Without SEM_FAILCRITICALERRORS, the loader answers a mismatched import (127) or a broken
     * image (193) with csrss's modal "Entry Point Not Found" or "Bad Image" box, which would hold
     * `frost --version` in a terminal until someone clicks (review of 4c5533d). Here the parent
     * hands down SEM_NOGPFAULTERRORBOX alone, and launcher_probe_app.cpp, copied as
     * frost_app.dll, records the mode during its own load: the critical-error flags must be on
     * then, in every mode. Command-line modes keep them; a GUI start (no option before "--")
     * gives frost_app.dll the parent's mode back. The probe loads cleanly, so no hard error can
     * arise even if the launcher regresses; the run then just fails here.
     */
    const ScratchDir scratch("app-process-errormode");
    const std::string exe = copyProbeTo(scratch.join("fr\xC3\xB8st probe"));
    constexpr unsigned kCritical = SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX;
    constexpr unsigned kNoBoxes = kCritical | SEM_NOGPFAULTERRORBOX;
    struct Case {
        std::vector<std::string> args;
        bool commandLine;
    };
    const std::vector<Case> cases{{{"--version"}, true},
                                  {{}, false},
                                  {{"--", "-file.txt", "--version"}, false},
                                  {{"file.txt", "--selftest"}, true}};
    for (const Case& c : cases) {
        SCOPED_TRACE(std::format("arguments: {}", c.args.empty() ? std::string("none") : c.args.front() + " ..."));
        frost::io::ProcessSpec spec;
        spec.executable = exe;
        spec.arguments = c.args;
        spec.stderrMode = frost::io::Stdio::MergeIntoStdout;
        const frost::test::PrivateDesktop desktop;
        ASSERT_TRUE(desktop.valid());
        spec.desktop = desktop.name();
        const auto result = [&] {
            const InheritedErrorMode inherited(SEM_NOGPFAULTERRORBOX);
            return frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, {}, 30s);
        }();
        ASSERT_TRUE(result.has_value()) << result.error().describe();
        const std::string& output = result->stdoutBytes;
        EXPECT_EQ(result->exitCode, 0) << output;
        const auto atLoad = hexAfter(output, "error mode at load 0x");
        const auto atEntry = hexAfter(output, "at entry 0x");
        const auto bits = hexAfter(output, "hardening bits 0x");
        ASSERT_TRUE(atLoad && atEntry && bits) << output;
        EXPECT_EQ(*atLoad & kCritical, kCritical) << output;
        if (c.commandLine) {
            EXPECT_EQ(*atEntry & kNoBoxes, kNoBoxes) << output;
        } else {
            EXPECT_EQ(*atEntry & kNoBoxes, static_cast<unsigned>(SEM_NOGPFAULTERRORBOX)) << output;
        }
        // Every hardening step, the \\?\ install directory included.
        EXPECT_EQ(*bits, 0x1Fu) << output;
    }
}

// What a run of the launcher with launcher_probe_app.cpp as frost_app.dll gave back.
struct ProbeRun {
    int exitCode = -1;       // -1 when it could not be run (`error` says why).
    std::string header;      // The probe's first stdout line, without its line break.
    std::string echoed;      // The rest of stdout: what the probe copied from stdin.
    std::string stderrBytes; // All of stderr.
    std::string error;
};

/*
 * Runs `exe` (a copyProbeTo() result) with `args` on a private desktop, in `form`, with
 * stdin, stdout and stderr as separate pipes: `stdinData` is written to stdin, and
 * `environment` applies on top of this process's. The child inherits the error mode
 * `errorMode` (InheritedErrorMode), so a test can tell the parent's mode from the launcher's.
 */
ProbeRun runProbe(const std::string& exe, std::vector<std::string> args, Environment environment, ImagePathForm form,
                  std::string_view stdinData = {},
                  UINT errorMode = SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX) {
    frost::io::ProcessSpec spec;
    spec.executable = exe;
    spec.arguments = std::move(args);
    spec.environment = std::move(environment);
    spec.imagePathForm = form;
    spec.stdinMode = frost::io::Stdio::Pipe;
    const frost::test::PrivateDesktop desktop;
    ProbeRun run;
    if (!desktop.valid()) {
        run.error = "no private desktop";
        return run;
    }
    spec.desktop = desktop.name();
    const auto result = [&] {
        const InheritedErrorMode inherited(errorMode);
        return frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, stdinData, 30s);
    }();
    if (!result) {
        run.error = result.error().describe();
        return run;
    }
    const std::size_t line = result->stdoutBytes.find('\n');
    run.exitCode = result->exitCode;
    run.stderrBytes = result->stderrBytes;
    run.header = result->stdoutBytes.substr(0, line);
    run.echoed = line == std::string::npos ? std::string() : result->stdoutBytes.substr(line + 1);
    return run;
}

TEST(FrostProcess, RestartPassesStatusAndStandardStreamsThrough) {
    /*
     * The launcher that restarts waits for the restarted one and exits with its status, and the
     * restarted one gets the parent's standard handles and error mode. launcher_probe_app.cpp,
     * copied as frost_app.dll beside a frost.exe whose image path is 300 units long, reports
     * where it runs, copies stdin to stdout, writes to stderr and exits with the status the
     * test picks, among them 0xC0000409, the status of a fast-fail crash. Cases: the GUI (no
     * arguments) and --selftest=onscreen, plain and \\?\ where this process's long paths allow
     * a plain start; each stream is checked byte for byte, as is the restart marker's removal.
     * The same probe at 259 units starts in place.
     */
    const ScratchDir scratch("app-process-restartio");
    const std::string base = longFormOf(scratch);
    ASSERT_FALSE(base.empty());
    constexpr unsigned kCritical = SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX;
    constexpr unsigned kNoBoxes = kCritical | SEM_NOGPFAULTERRORBOX;
    struct Case {
        std::size_t units;
        ImagePathForm form;
        std::vector<std::string> args;
        std::string status; // FROST_PROBE_EXIT_CODE.
        int expected;       // The exit code as frost::io reports it (DWORD bits as int).
    };
    std::vector<Case> cases{
        {300, ImagePathForm::Extended, {}, "42", 42},
        {300, ImagePathForm::Extended, {"--selftest=onscreen"}, "0xC0000409", static_cast<int>(0xC0000409u)},
        {259, ImagePathForm::Plain, {}, "7", 7}};
    if (parentLongPathsEffective()) {
        cases.push_back({300, ImagePathForm::Plain, {"--selftest=onscreen", "--", "x"}, "0", 0});
        cases.push_back({400, ImagePathForm::Plain, {"file.txt"}, "0x100", 256});
    }
    // Binary data, larger than a pipe's buffer, with a NUL and a non-ASCII character.
    std::string input = "first line\r\nzweite Zeile \xC3\xBC\n";
    input += std::string("\0\x01\xFF", 3);
    input += std::string(70000, 'q');
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const Case& c = cases[index];
        SCOPED_TRACE(std::format("{}, {} units, {}", formName(c.form), c.units,
                                 c.args.empty() ? std::string("GUI") : c.args.front()));
        const std::string exe = copyProbeTo(imagePathCell(base, c.units, c.form));
        const std::string localAppData = localAppDataIn(scratch, std::format("lad {}", index));
        const ProbeRun run = runProbe(exe, c.args,
                                      {{"LOCALAPPDATA", localAppData},
                                       {"FROST_PROBE_ECHO", "1"},
                                       {"FROST_PROBE_EXIT_CODE", c.status},
                                       {"FROST_RESTARTED_FROM", std::nullopt}},
                                      c.form, input, SEM_NOGPFAULTERRORBOX);
        ASSERT_TRUE(run.error.empty()) << run.error;
        EXPECT_EQ(run.exitCode, c.expected) << run.header << "\n" << run.stderrBytes;
        EXPECT_EQ(run.stderrBytes, "launcher probe: stderr\n");
        EXPECT_TRUE(run.echoed == input) << "stdout after the header: " << run.echoed.size() << " bytes, expected "
                                         << input.size();
        EXPECT_TRUE(run.header.ends_with(", marker unset")) << run.header;
        EXPECT_EQ(hexAfter(run.header, "hardening bits 0x"), 0x1Fu) << run.header;
        const auto atEntry = hexAfter(run.header, "at entry 0x");
        ASSERT_TRUE(atEntry.has_value()) << run.header;
        // The parent's mode reaches the GUI through the restart; command-line modes add the rest.
        EXPECT_EQ(*atEntry & kNoBoxes, c.args.empty() || c.args.front() == "file.txt"
                                           ? static_cast<unsigned>(SEM_NOGPFAULTERRORBOX)
                                           : kNoBoxes)
            << run.header;
        if (c.units < MAX_PATH) {
            EXPECT_TRUE(run.header.contains(", restarted from nothing, image " + startedAs(exe, c.form) + ","))
                << run.header;
            EXPECT_FALSE(shortLinkFolderExists(localAppData));
            continue;
        }
        const std::vector<ShortLinkEntry> links = shortLinksIn(localAppData);
        ASSERT_EQ(links.size(), 1u) << run.header;
        EXPECT_TRUE(links.front().junction);
        EXPECT_TRUE(run.header.contains(", restarted from " + startedAs(exe, c.form) + ", image " + links.front().path +
                                        "\\frost.exe,"))
            << run.header;
    }
}

TEST(FrostProcess, RestartedLauncherNeverRestartsAgain) {
    /*
     * The restart marker (FROST_RESTARTED_FROM) tells a launcher that another one started it
     * through the short link, and it counts only when it names the launcher's own image file,
     * as it does after a real restart (sameImageFile(); review of 2f40f9f). Such a launcher
     * never restarts: if its own image path is still too long for the GPU driver, it refuses
     * with status 4 and a message instead of starting a third process. Here the parent sets the
     * marker to frost's own 300-unit path in its plain spelling, while frost starts through
     * \\?\. A marker that names another file is dropped: from 300 units frost restarts as it
     * would without it, and frost_app.dll gets the real first path; from 200 units it gets
     * nothing. From 200 units a marker naming frost's own file is handed on. In every case the
     * launcher takes the marker out of the environment.
     */
    const ScratchDir scratch("app-process-loopguard");
    const std::string base = longFormOf(scratch);
    ASSERT_FALSE(base.empty());
    const std::string exe = copyProbeTo(imagePathCell(base, 300, ImagePathForm::Extended));
    const std::string stale = "C:\\first launcher\\frost.exe";
    {
        SCOPED_TRACE("a marker naming this file, 300 units");
        const std::string localAppData = localAppDataIn(scratch, "lad guard");
        const auto started = std::chrono::steady_clock::now();
        const ProbeRun run =
            runProbe(exe, {"--selftest=onscreen"}, {{"LOCALAPPDATA", localAppData}, {"FROST_RESTARTED_FROM", exe}},
                     ImagePathForm::Extended);
        ASSERT_TRUE(run.error.empty()) << run.error;
        EXPECT_EQ(run.exitCode, 4) << run.header << run.stderrBytes;
        const std::string expected = "frost: frost was restarted through the short link " +
                                     startedAs(exe, ImagePathForm::Extended) + " from " + exe +
                                     ", but that path is still 300 UTF-16 units long, and graphics drivers end a "
                                     "program started from 260 units (MAX_PATH) or more. Install FrostIDE in a "
                                     "directory with a shorter path.\n";
        EXPECT_EQ(run.stderrBytes, expected);
        EXPECT_EQ(run.header, "") << "frost_app.dll was not loaded";
        EXPECT_FALSE(shortLinkFolderExists(localAppData));
        EXPECT_LT(std::chrono::steady_clock::now() - started, 30s);
    }
    {
        SCOPED_TRACE("a marker naming another file, 300 units");
        const std::string localAppData = localAppDataIn(scratch, "lad stale");
        const ProbeRun run =
            runProbe(exe, {"--selftest=onscreen"}, {{"LOCALAPPDATA", localAppData}, {"FROST_RESTARTED_FROM", stale}},
                     ImagePathForm::Extended);
        ASSERT_TRUE(run.error.empty()) << run.error;
        EXPECT_EQ(run.exitCode, 0) << run.header << run.stderrBytes;
        const std::vector<ShortLinkEntry> links = shortLinksIn(localAppData);
        ASSERT_EQ(links.size(), 1u) << run.header;
        EXPECT_TRUE(run.header.ends_with(", restarted from " + startedAs(exe, ImagePathForm::Extended) + ", image " +
                                         links.front().path + "\\frost.exe, marker unset"))
            << run.header;
    }

    const std::string shortExe = copyProbeTo(imagePathCell(base, 200, ImagePathForm::Plain));
    const std::string localAppData = localAppDataIn(scratch, "lad short");
    // Each marker and what the probe then reports as the path frost was restarted from.
    const std::vector<std::pair<std::string, std::string>> markers{{stale, "nothing"},
                                                                   {"\\\\?\\" + shortExe, "\\\\?\\" + shortExe}};
    for (const auto& [marker, reported] : markers) {
        SCOPED_TRACE("200 units, marker " + marker);
        const ProbeRun run =
            runProbe(shortExe, {"--selftest=onscreen"},
                     {{"LOCALAPPDATA", localAppData}, {"FROST_RESTARTED_FROM", marker}}, ImagePathForm::Plain);
        ASSERT_TRUE(run.error.empty()) << run.error;
        EXPECT_EQ(run.exitCode, 0) << run.header << run.stderrBytes;
        EXPECT_TRUE(run.header.ends_with(", restarted from " + reported + ", image " + shortExe + ", marker unset"))
            << run.header;
        EXPECT_FALSE(shortLinkFolderExists(localAppData));
    }
}

TEST(FrostProcess, RestartRefusesWithoutAUsableShortLink) {
    /*
     * Where no short link can help, the launcher refuses with status 4 and a message that says
     * why and what to do, and starts nothing: LOCALAPPDATA unset; LOCALAPPDATA so long that the
     * link's image path would reach 260 units as well; and a file a user left at the link's
     * path, which stays as it was. In a command-line mode, so the message goes to stderr and no
     * box is shown.
     */
    const ScratchDir scratch("app-process-refuse");
    const std::string base = longFormOf(scratch);
    ASSERT_FALSE(base.empty());
    const ImagePathForm form = longStartForm();
    const std::string exe = copyProbeTo(imagePathCell(base, 300, form));
    const std::string why = std::format("frost: frost.exe's path is {} UTF-16 units long, and graphics drivers end a "
                                        "program started from 260 units (MAX_PATH) or more, so frost restarts "
                                        "through a short link, but ",
                                        300);
    const auto refused = [&](const ProbeRun& run, const std::string& reason) {
        ASSERT_TRUE(run.error.empty()) << run.error;
        EXPECT_EQ(run.exitCode, 4) << run.header << run.stderrBytes;
        EXPECT_EQ(run.stderrBytes, why + reason + "\n");
        EXPECT_EQ(run.header, "") << "nothing was started";
    };
    {
        SCOPED_TRACE("LOCALAPPDATA unset");
        refused(runProbe(exe, {"--selftest=onscreen"}, {{"LOCALAPPDATA", std::nullopt}}, form),
                "LOCALAPPDATA, the folder for that link, is not set. Install FrostIDE in a directory with a "
                "shorter path.");
    }
    {
        SCOPED_TRACE("LOCALAPPDATA too long");
        // <root>\FrostIDE\i\<16 hex digits>\frost.exe is the root's length plus 38 units.
        const std::string root = padToLength(base, 222);
        ASSERT_TRUE(frost::io::realFileSystem().createDirectories(root).has_value());
        const ProbeRun run = runProbe(exe, {"--selftest=onscreen"}, {{"LOCALAPPDATA", root}}, form);
        ASSERT_TRUE(run.error.empty()) << run.error;
        EXPECT_EQ(run.exitCode, 4) << run.header << run.stderrBytes;
        EXPECT_TRUE(run.stderrBytes.starts_with(why + "its short link " + root + "\\FrostIDE\\i\\")) << run.stderrBytes;
        EXPECT_TRUE(run.stderrBytes.ends_with("\\frost.exe would still be 260 UTF-16 units long. Install FrostIDE in "
                                              "a directory with a shorter path.\n"))
            << run.stderrBytes;
        EXPECT_EQ(run.header, "") << "nothing was started";
        EXPECT_FALSE(shortLinkFolderExists(root));
    }
    {
        SCOPED_TRACE("a file at the link's path");
        const std::string localAppData = localAppDataIn(scratch, "lad");
        const ProbeRun first = runProbe(exe, {"--selftest=onscreen"}, {{"LOCALAPPDATA", localAppData}}, form);
        ASSERT_EQ(first.exitCode, 0) << first.header << first.stderrBytes;
        const std::vector<ShortLinkEntry> links = shortLinksIn(localAppData);
        ASSERT_EQ(links.size(), 1u);
        const std::wstring link = extendedWide(links.front().path);
        ASSERT_NE(RemoveDirectoryW(link.c_str()), FALSE) << GetLastError();
        {
            std::ofstream out(fs::path(link), std::ios::binary | std::ios::trunc);
            out << "user data";
        }
        refused(runProbe(exe, {"--selftest=onscreen"}, {{"LOCALAPPDATA", localAppData}}, form),
                "its short link's path " + links.front().path +
                    " holds a file; remove it and start frost again, or install FrostIDE in a directory with a "
                    "shorter path.");
        std::ifstream in(fs::path(link), std::ios::binary);
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()), "user data");
    }
}

// The top-level windows on `desktop` titled exactly `title` and shown, each with the process
// that owns it.
std::vector<std::pair<HWND, DWORD>> shownWindowsTitled(HDESK desktop, std::wstring_view title) {
    struct Search {
        std::wstring_view title;
        std::vector<std::pair<HWND, DWORD>> found;
    } search{title, {}};
    const WNDENUMPROC collect = [](HWND window, LPARAM context) -> BOOL {
        auto& into = *reinterpret_cast<Search*>(context);
        wchar_t text[64] = {};
        const int units = GetWindowTextW(window, text, 64);
        if (IsWindowVisible(window) != FALSE &&
            std::wstring_view(text, static_cast<std::size_t>(units > 0 ? units : 0)) == into.title) {
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            into.found.emplace_back(window, owner);
        }
        return TRUE;
    };
    // FALSE also means "no windows at all", so the result alone is the answer.
    static_cast<void>(EnumDesktopWindows(desktop, collect, reinterpret_cast<LPARAM>(&search)));
    return search.found;
}

// The parent process id of process `pid` (a Toolhelp snapshot), or 0.
DWORD parentProcessOf(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    DWORD parent = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more != FALSE; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == pid) {
            parent = entry.th32ParentProcessID;
            break;
        }
    }
    CloseHandle(snapshot);
    return parent;
}

/*
 * Waits up to `limit` for a shown window titled `title` on `desktop` that a process other
 * than `launcher` owns (the restarted frost's), polling every 20 ms; stops early when
 * `process` (the launcher) ends. Returns the window and its owner, or {nullptr, 0}.
 */
std::pair<HWND, DWORD> waitForRestartedWindow(const frost::test::PrivateDesktop& desktop, std::wstring_view title,
                                              frost::io::IProcess& process, std::chrono::milliseconds limit) {
    const auto launcher = static_cast<DWORD>(process.pid());
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto& [window, owner] : shownWindowsTitled(desktop.handle(), title)) {
            if (owner != launcher) {
                return {window, owner};
            }
        }
        if (process.wait(0ms).has_value()) {
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    return {nullptr, 0};
}

/*
 * Starts `exe` without arguments (the GUI) in `form` on `desktop`, stdout piped with stderr
 * merged into it, under an error mode without hard-error boxes; null after a failure the
 * test records.
 */
std::unique_ptr<frost::io::IProcess> startGui(const std::string& exe, Environment environment, ImagePathForm form,
                                              const frost::test::PrivateDesktop& desktop) {
    frost::io::ProcessSpec spec;
    spec.executable = exe;
    spec.environment = std::move(environment);
    spec.imagePathForm = form;
    spec.stderrMode = frost::io::Stdio::MergeIntoStdout;
    spec.desktop = desktop.name();
    const frost::test::NoHardErrorBoxes noHardErrorBoxes;
    auto process = frost::io::realProcessLauncher().launch(spec);
    if (!process) {
        ADD_FAILURE() << process.error().describe();
        return nullptr;
    }
    return std::move(*process);
}

TEST(FrostProcess, GuiStartFromALongPathShowsItsWindow) {
    /*
     * The GUI from a 300-unit image path, as a user starts it (no arguments): the launcher
     * restarts frost through its short link and waits; the main window ("FrostIDE") appears on
     * this test's private desktop, owned by the restarted process, whose parent is the
     * launcher. Closing the window ends both, with status 0.
     */
    const ScratchDir scratch("app-process-gui");
    const ImagePathForm form = longStartForm();
    const std::string exe = copyFrostTo(imagePathCell(longFormOf(scratch), 300, form));
    const std::string localAppData = localAppDataIn(scratch, "lad");
    Environment environment = ownPluginsOnly();
    environment.emplace_back("LOCALAPPDATA", localAppData);
    const frost::test::PrivateDesktop desktop;
    ASSERT_TRUE(desktop.valid());
    const auto process = startGui(exe, environment, form, desktop);
    ASSERT_NE(process, nullptr);
    const auto [window, owner] = waitForRestartedWindow(desktop, L"FrostIDE", *process, 60s);
    if (window == nullptr) {
        const auto rest = process->communicate({}, 10s);
        FAIL() << "no FrostIDE window from a restarted frost; launcher output: "
               << (rest ? rest->stdoutBytes : rest.error().describe());
    }
    EXPECT_EQ(parentProcessOf(owner), static_cast<DWORD>(process->pid()));
    EXPECT_FALSE(process->wait(0ms).has_value()) << "the launcher waits for the restarted frost";
    const std::vector<ShortLinkEntry> links = shortLinksIn(localAppData);
    ASSERT_EQ(links.size(), 1u);
    EXPECT_TRUE(links.front().junction);
    ASSERT_NE(PostMessageW(window, WM_CLOSE, 0, 0), FALSE) << GetLastError();
    const auto result = process->communicate({}, 30s);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 0) << result->stdoutBytes;
}

TEST(FrostProcess, GuiFailureAfterARestartNamesTheInstallDirectory) {
    /*
     * After a restart every path frost names itself is the link's, so a GUI failure message
     * also says where FrostIDE is installed. Here platforms\ is missing: the restarted frost's
     * box ("FrostIDE cannot start", on the private desktop) and its stderr name the real
     * install directory and the link. The test closes the box; status 4 comes back through
     * the launcher.
     */
    const ScratchDir scratch("app-process-guifail");
    const ImagePathForm form = longStartForm();
    const std::string dir = imagePathCell(longFormOf(scratch), 300, form);
    const std::string exe = copyFrostTo(dir, Plugins::None);
    const std::string localAppData = localAppDataIn(scratch, "lad");
    Environment environment = ownPluginsOnly();
    environment.emplace_back("LOCALAPPDATA", localAppData);
    const frost::test::PrivateDesktop desktop;
    ASSERT_TRUE(desktop.valid());
    const auto process = startGui(exe, environment, form, desktop);
    ASSERT_NE(process, nullptr);
    const auto [box, owner] = waitForRestartedWindow(desktop, L"FrostIDE cannot start", *process, 60s);
    if (box == nullptr) {
        const auto rest = process->communicate({}, 10s);
        FAIL() << "no failure box from a restarted frost; launcher output: "
               << (rest ? rest->stdoutBytes : rest.error().describe());
    }
    EXPECT_EQ(parentProcessOf(owner), static_cast<DWORD>(process->pid()));
    // MessageBoxW's text is its static control 0xFFFF (WM_GETTEXT works across processes).
    std::wstring text(4096, L'\0');
    text.resize(static_cast<std::size_t>(GetDlgItemTextW(box, 0xFFFF, text.data(), static_cast<int>(text.size()))));
    ASSERT_NE(PostMessageW(box, WM_CLOSE, 0, 0), FALSE) << GetLastError();
    const auto result = process->communicate({}, 30s);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 4) << result->stdoutBytes;
    const std::vector<ShortLinkEntry> links = shortLinksIn(localAppData);
    ASSERT_EQ(links.size(), 1u);
    const std::string note = "\n\nFrostIDE is installed in " + dir + ". It runs through the short link " +
                             links.front().path +
                             ", because graphics drivers end a program started from a path of 260 UTF-16 units or "
                             "more.";
    const std::string shown = frost::io::fromWide(text).value_or("?");
    EXPECT_TRUE(shown.starts_with("Qt's platform plugin \"windows\" is missing")) << shown;
    EXPECT_TRUE(shown.ends_with(note)) << "expected " << note << "\nin the box: " << shown;
    // stderr is in text mode: its line breaks are CR LF.
    std::string output = result->stdoutBytes;
    for (std::size_t at = output.find("\r\n"); at != std::string::npos; at = output.find("\r\n", at)) {
        output.erase(at, 1);
    }
    EXPECT_TRUE(output.contains("frost: Qt's platform plugin \"windows\" is missing")) << output;
    EXPECT_TRUE(output.contains(note)) << "expected " << note << "\n" << output;
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

// The DLL names of the image's delay-import directory (every descriptor), lower-cased.
std::vector<std::string> delayImportedDlls(const MappedImage& image) {
    std::vector<std::string> names;
    const IMAGE_DATA_DIRECTORY entry = image.directory(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    if (entry.VirtualAddress == 0) {
        return names;
    }
    for (const auto* descriptor = image.at<IMAGE_DELAYLOAD_DESCRIPTOR>(entry.VirtualAddress);
         descriptor->DllNameRVA != 0; ++descriptor) {
        std::string name = image.at<char>(descriptor->DllNameRVA);
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
    EXPECT_EQ(dependentLoadFlags(app), 0u);
}

TEST(FrostProcess, AppDllEmbedsNoManifest) {
    // An RT_MANIFEST 2 (the linker's default for a DLL) would subject frost_app.dll to the
    // loader's 250-unit plain-path limit, and an isolation context could change how its
    // imports resolve.
    const MappedImage app(appDll());
    ASSERT_TRUE(app.valid());
    EXPECT_EQ(manifestIds(app), std::vector<unsigned>{});
}

// Whether `name` (lower case) is a DLL frost_app.dll may import (see the test below).
bool allowedAppImport(std::string_view name) {
    static constexpr std::string_view kSystem[] = {"kernel32.dll", "user32.dll", "advapi32.dll", "shell32.dll",
                                                   "bcrypt.dll"};
    if (std::ranges::find(kSystem, name) != std::end(kSystem)) {
        return true;
    }
    // The C and C++ runtimes (release or debug), the ASan runtime in the ASan tree, and Qt.
    for (const std::string_view prefix : {"api-ms-win-crt-", "ucrtbase", "vcruntime140", "msvcp140",
                                          "clang_rt.asan_dynamic-x86_64", "qt6core", "qt6gui", "qt6widgets"}) {
        if (name.starts_with(prefix) && name.ends_with(".dll")) {
            return true;
        }
    }
    return false;
}

TEST(FrostProcess, AppDllImportsOnlyAllowedDlls) {
    /*
     * frost::diag (crashpad, cpptrace, spdlog; ADR 0022) adds only bcrypt.dll, a System32 DLL,
     * to frost_app.dll's static imports: no dbghelp.dll (the cpptrace overlay resolves nothing
     * in process), no networking DLL (nothing is uploaded, D26), and nothing delay-loaded that
     * could change how the launcher's restart rules apply (ADR 0012).
     *
     * The one delay import allowed is ws2_32.dll, which every binary that links frost_io
     * delay-loads (src/io/CMakeLists.txt, ADR 0020): frost_app.dll links frost_io for the crash
     * handler's IProcessLauncher, and asio's Windows backend references Winsock. It is a
     * System32 KnownDLL, so where it would load from does not depend on frost's image path,
     * and reactor.cpp's guard keeps it from ever loading (FileWatcherWindows.WinsockIsNeverLoaded).
     * Every delay descriptor is checked, so any other delay-loaded DLL still fails.
     */
    const MappedImage app(appDll());
    ASSERT_TRUE(app.valid());
    const std::vector<std::string> imports = importedDlls(app);
    EXPECT_FALSE(imports.empty());
    for (const std::string& name : imports) {
        EXPECT_TRUE(allowedAppImport(name)) << name;
    }
    EXPECT_NE(std::ranges::find(imports, "bcrypt.dll"), imports.end());
    for (const std::string& name : delayImportedDlls(app)) {
        EXPECT_EQ(name, "ws2_32.dll") << "frost_app.dll delay-loads " << name;
    }
}

// The crashpad_handler.exe processes whose parent is `parent` (a Toolhelp snapshot).
std::vector<DWORD> crashHandlersOf(DWORD parent) {
    std::vector<DWORD> found;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return found;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Process32FirstW(snapshot, &entry); more != FALSE; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ParentProcessID == parent && std::wstring_view(entry.szExeFile) == L"crashpad_handler.exe") {
            found.push_back(entry.th32ProcessID);
        }
    }
    CloseHandle(snapshot);
    return found;
}

// The image path of process `pid` (UTF-8, the form the kernel reports), or "".
std::string imagePathOf(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path.data(), &size);
    CloseHandle(process);
    if (ok == FALSE) {
        return {};
    }
    path.resize(size);
    return frost::io::fromWide(path).value_or(std::string());
}

#endif

// --- Crash and freeze reports (ADR 0022) ---

/*
 * frost's cache for one test: LOCALAPPDATA (Windows) or XDG_CACHE_HOME (Linux) redirected into
 * the scratch directory `base`, so no report reaches the user's own cache, and the directories
 * frost writes below it (frost/diag/paths.hpp). On Linux XDG_DATA_HOME goes below `base` too, so
 * the GUI's hot-exit journal and sessions (ADR 0042) stay out of the user's data directory.
 */
struct TestCache {
    Environment environment;
    std::string crashpad;
    std::string freezes;
    std::string logs;
};

TestCache testCacheIn(const std::string& base) {
    EXPECT_TRUE(frost::io::realFileSystem().createDirectories(base).has_value()) << base;
    const auto join = [](std::string path, std::initializer_list<std::string_view> parts) {
        for (const std::string_view part : parts) {
            path = frost::io::joinPath(path, part).value();
        }
        return path;
    };
    TestCache cache;
#ifdef _WIN32
    cache.environment = {{"LOCALAPPDATA", base}};
    const std::string root = join(base, {"FrostIDE", "caches"});
#else
    cache.environment = {{"XDG_CACHE_HOME", base}, {"XDG_DATA_HOME", join(base, {"data"})}};
    const std::string root = join(base, {"FrostIDE"});
#endif
    cache.crashpad = join(root, {"crashes", "crashpad"});
    cache.freezes = join(root, {"crashes", "freezes"});
    cache.logs = join(root, {"logs"});
    return cache;
}

// <build>/selftest-artifacts/<config>/<kind>, emptied: where verify -Full's symbolize step
// (tools/symbolize.py check-selftest) finds this run's artifacts.
std::string freshArtifactDirectory(std::string_view kind) {
    const std::string dir =
        frost::io::joinPath(frost::io::normalizePath(FROST_SELFTEST_ARTIFACTS).value(), kind).value();
    EXPECT_TRUE(frost::test::removeTree(frost::io::realFileSystem(), dir)) << dir;
    EXPECT_TRUE(frost::io::realFileSystem().createDirectories(dir).has_value()) << dir;
    return dir;
}

/*
 * Copies `file` into `dir` and writes dir/artifact.json, which tells tools/symbolize.py what the
 * artifact must symbolize to: `function` at the source line of `source` that holds `marker`.
 */
void saveArtifact(const std::string& dir, const fs::path& file, std::string_view kind, std::string_view function,
                  std::string_view marker, std::string_view source, int status, int signal) {
    std::error_code error;
    fs::copy_file(file, anyLengthPath(dir) / file.filename(), fs::copy_options::overwrite_existing, error);
    EXPECT_FALSE(error) << "copy " << file.string() << ": " << error.message();
    const std::string json = std::format("{{\n  \"schema\": \"frost-selftest-artifact/1\",\n  \"kind\": \"{}\",\n"
                                         "  \"file\": \"{}\",\n  \"function\": \"{}\",\n  \"marker\": \"{}\",\n"
                                         "  \"source\": \"{}\",\n  \"status\": {},\n  \"signal\": {}\n}}\n",
                                         kind, frost::io::fromStdPath(file.filename()).value(), function, marker,
                                         source, static_cast<std::uint32_t>(status), signal);
    const auto written =
        frost::io::realFileSystem().writeFileAtomic(frost::io::joinPath(dir, "artifact.json").value(), json);
    EXPECT_TRUE(written.has_value()) << written.error().describe();
}

// The one finished crash report in `database`, parsed; nullopt after a recorded failure.
std::optional<frost::test::MinidumpReader> onlyReport(const std::string& database, const std::string& context,
                                                      fs::path* file = nullptr) {
    const frost::test::DatabaseReports reports = frost::test::waitForReports(anyLengthPath(database), 1);
    EXPECT_TRUE(reports.unfinished.empty()) << context;
    if (reports.finished.size() != 1) {
        ADD_FAILURE() << reports.finished.size() << " finished reports, expected 1\n" << context;
        return std::nullopt;
    }
    frost::test::MinidumpReader dump(frost::test::readBytes(reports.finished.front()));
    if (!dump.error().empty() || !dump.exception().has_value()) {
        ADD_FAILURE() << "unreadable minidump: " << dump.error();
        return std::nullopt;
    }
    if (file != nullptr) {
        *file = reports.finished.front();
    }
    return dump;
}

std::string annotationOf(const frost::test::MinidumpReader& dump, const std::string& key) {
    return dump.annotations().contains(key) ? dump.annotations().at(key) : std::string();
}

// The checks of a --selftest=crash run whose cache is `cache`: the status, one report, the
// fault at 0xF00D in frost_app.dll (Linux: frost) and the selftest annotation.
void expectSelfTestCrash(const FrostRun& run, const TestCache& cache, fs::path* file = nullptr) {
    const std::string context =
        std::format("exit {} signal {} {}\n{}", run.exitCode, run.signal, run.startError, run.output);
    EXPECT_TRUE(run.output.contains("selftest: crash-reporter ok pid=")) << context;
    EXPECT_TRUE(run.output.contains("selftest: crashing now")) << context;
#ifdef _WIN32
    EXPECT_EQ(static_cast<std::uint32_t>(run.exitCode), 0xC0000005u) << context;
#else
    EXPECT_EQ(run.signal, SIGSEGV) << context;
#endif
    const auto dump = onlyReport(cache.crashpad, context, file);
    if (!dump) {
        return;
    }
    const frost::test::MinidumpException& exception = *dump->exception();
#ifdef _WIN32
    EXPECT_EQ(exception.code, 0xC0000005u);
    ASSERT_GE(exception.information.size(), 2u);
    EXPECT_EQ(exception.information[0], 1u) << "a write";
    EXPECT_EQ(exception.information[1], 0xF00Du);
    const std::string_view module = "frost_app.dll";
#else
    EXPECT_EQ(exception.code, static_cast<std::uint32_t>(SIGSEGV));
    EXPECT_EQ(exception.address, 0xF00Du);
    const std::string_view module = "frost";
#endif
    const frost::test::MinidumpModule* faulting = dump->moduleAt(exception.contextRip);
    ASSERT_NE(faulting, nullptr) << std::format("rip 0x{:x} in no module", exception.contextRip);
    EXPECT_EQ(frost::io::fileName(faulting->name), module) << faulting->name;
    EXPECT_EQ(annotationOf(*dump, "frost.selftest"), "crash");
    EXPECT_FALSE(annotationOf(*dump, "frost.image_path").empty());
}

TEST(FrostProcess, SelfTestCrashWritesASymbolizableDump) {
    /*
     * --selftest=crash from the build directory: the crash handler starts, frost writes to
     * 0xF00D and crashpad's handler writes exactly one minidump into <cache>/crashes/crashpad;
     * the fault is in frost_app.dll (Linux: frost). The dump goes to selftest-artifacts/crash
     * for verify -Full, which symbolizes it to selfTestCrash's marker line.
     */
    const std::string artifacts = freshArtifactDirectory("crash");
    const ScratchDir scratch("app-process-crash");
    const TestCache cache = testCacheIn(scratch.join("cache"));
    const FrostRun run = runFrost(frostExe(), {"--selftest=crash"}, frost::test::crashEnvironment(cache.environment));
    fs::path dump;
    expectSelfTestCrash(run, cache, &dump);
    ASSERT_FALSE(dump.empty());
    saveArtifact(artifacts, dump, "crash", "frost::diag::selfTestCrash", "FROST-SELFTEST-CRASH-SITE",
                 "src/diag/src/selftest_faults.cpp", run.exitCode, run.signal);
}

TEST(FrostProcess, SelfTestFreezeWritesAReport) {
    /*
     * --selftest=freeze: frost blocks its GUI thread for 300 ms and the watchdog writes one
     * final freeze report into <cache>/crashes/freezes with at least 5 samples over at least
     * 250 ms. The report goes to selftest-artifacts/freeze for verify -Full.
     */
    const std::string artifacts = freshArtifactDirectory("freeze");
    const ScratchDir scratch("app-process-freeze");
    const TestCache cache = testCacheIn(scratch.join("cache"));
    const FrostRun run = runFrost(frostExe(), {"--selftest=freeze"}, cache.environment);
    ASSERT_EQ(run.exitCode, 0) << run.startError << run.output;
    // "selftest: freeze-report <path> samples=<n> duration-ms=<ms>"
    const std::string_view label = "selftest: freeze-report ";
    const std::size_t at = run.output.find(label);
    ASSERT_NE(at, std::string::npos) << run.output;
    const std::size_t end = run.output.find(" samples=", at);
    ASSERT_NE(end, std::string::npos) << run.output;
    const std::string path = run.output.substr(at + label.size(), end - at - label.size());
    EXPECT_EQ(frost::io::parentPath(path), cache.freezes) << path;
    const std::string json = frost::test::readBytes(anyLengthPath(path));
    EXPECT_TRUE(json.contains("\"schema\": \"frost-freeze-report/1\"")) << json.substr(0, 300);
    EXPECT_TRUE(json.contains("\"ongoing\": false")) << json.substr(0, 300);
    std::size_t samples = 0;
    for (std::size_t found = json.find("\"t_ms\":"); found != std::string::npos;
         found = json.find("\"t_ms\":", found + 1)) {
        ++samples;
    }
    EXPECT_GE(samples, 5u);
    const std::size_t duration = json.find("\"duration_ms\": ");
    ASSERT_NE(duration, std::string::npos);
    EXPECT_GE(std::stoll(json.substr(duration + 15)), 250);
    saveArtifact(artifacts, anyLengthPath(path), "freeze", "frost::diag::selfTestFreeze", "FROST-SELFTEST-FREEZE-SITE",
                 "src/diag/src/selftest_faults.cpp", run.exitCode, 0);
}

TEST(FrostProcess, ContractViolationDumpIsCollected) {
    /*
     * An enforced FROST_PRE in crash_child (frost::diag's fatal paths set up as frost's program
     * sets them up) ends in a crash report whose annotations name the reason, the violated
     * expression and the frames; verify -Full symbolizes the frames to the violating function
     * at its marker line (selftest-artifacts/contract).
     */
    const std::string artifacts = freshArtifactDirectory("contract");
    const ScratchDir scratch("app-process-contract");
    const std::string database = scratch.join("crashpad");
    const frost::test::ChildRun run =
        frost::test::runChild(frost::io::normalizePath(FROST_CRASH_CHILD).value(), {"contract-enforce", database},
                              frost::test::crashEnvironment());
    const std::string context =
        std::format("exit {} signal {} {}\n{}", run.exitCode, run.signal, run.error, run.output);
#ifdef _WIN32
    EXPECT_EQ(static_cast<std::uint32_t>(run.exitCode), 0xE0464154u) << context; // kFatalExceptionCode
#else
    EXPECT_EQ(run.signal, SIGABRT) << context;
#endif
    fs::path file;
    const auto dump = onlyReport(database, context, &file);
    ASSERT_TRUE(dump.has_value());
    EXPECT_EQ(annotationOf(*dump, "frost.fatal_reason"), "contract-violation");
    EXPECT_TRUE(annotationOf(*dump, "frost.message").contains("precondition violated: value >= 0"));
    EXPECT_TRUE(annotationOf(*dump, "frost.frames").starts_with("0x"));
    saveArtifact(artifacts, file, "contract", "contractEnforceSite", "FROST-SELFTEST-CONTRACT-SITE",
                 "tests/diag/crash_child.cpp", run.exitCode, run.signal);
}

TEST(FrostProcess, SelfTestCrashFromEveryDirectoryLength) {
    /*
     * The crash handler starts beside frost wherever frost is installed: Windows directories of
     * 249, 259 and 300 UTF-16 units, plain and \\?\ (the crash self-test never restarts through
     * the short link, so it runs in place); Linux a non-ASCII directory with spaces.
     */
    const ScratchDir scratch("app-process-crash-dirs");
#ifdef _WIN32
    const std::string base = longFormOf(scratch);
    for (const ImagePathForm form : {ImagePathForm::Plain, ImagePathForm::Extended}) {
        if (form == ImagePathForm::Plain && !parentLongPathsEffective()) {
            continue; // CreateProcessW refuses long plain paths then (StartsWithLongPathsIneffective).
        }
        for (const std::size_t units : {249u, 259u, 300u}) {
            SCOPED_TRACE(std::format("{} units, {}", units, reportedForm(form)));
            const std::string exe = copyFrostTo(imagePathCell(base, units, form), Plugins::None);
            const TestCache cache =
                testCacheIn(scratch.join(std::format("cache {} {}", units, static_cast<int>(form))));
            const FrostRun run =
                runFrost(exe, {"--selftest=crash"}, frost::test::crashEnvironment(cache.environment), form, 60s);
            expectSelfTestCrash(run, cache);
        }
    }
#else
    const std::string exe = copyFrostTo(scratch.join("fr\xC3\xB8st \xE6\xB5\x8B dir"));
    const TestCache cache = testCacheIn(scratch.join("cache"));
    expectSelfTestCrash(runFrost(exe, {"--selftest=crash"}, frost::test::crashEnvironment(cache.environment)), cache);
#endif
}

// The log lines of the frost log files in `logs`, in file order.
std::string readLogs(const std::string& logs) {
    std::string text;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(anyLengthPath(logs), error)) {
        if (entry.path().extension() == ".log") {
            text += frost::test::readBytes(entry.path());
        }
    }
    return text;
}

// The value after " <key>=" in `text`: up to the closing quote when quoted, else up to the next
// space or line end.
std::string fieldAfter(std::string_view text, std::string_view key) {
    const std::string needle = " " + std::string(key) + "=";
    const std::size_t at = text.find(needle);
    if (at == std::string_view::npos) {
        return {};
    }
    std::size_t begin = at + needle.size();
    std::size_t end = std::string_view::npos;
    if (begin < text.size() && text[begin] == '"') {
        ++begin;
        end = text.find('"', begin);
    } else {
        end = text.find_first_of(" \r\n", begin);
    }
    return std::string(text.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
}

/*
 * Waits up to `limit` for the log in `logs` to name the registered crash handler; returns the
 * whole log text then, or what it holds at the deadline.
 */
std::string waitForHandlerLine(const std::string& logs, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    std::string text;
    while (std::chrono::steady_clock::now() < deadline) {
        text = readLogs(logs);
        if (text.contains("crash handler registered pid=")) {
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    return text;
}

TEST(FrostProcess, GuiStartsTheCrashHandlerAfterTheFirstFrame) {
    /*
     * The GUI registers the crash handler only after its first frame (B1): the log names
     * "crash handler registered pid=N path=..." after "first frame shown", the handler lives
     * beside frost_app.dll (Linux: frost), and it ends with frost: within 2 s of a close
     * (Windows WM_CLOSE) or SIGTERM (Linux).
     */
    const ScratchDir scratch("app-process-gui-handler");
    const TestCache cache = testCacheIn(scratch.join("cache"));
#ifdef _WIN32
    Environment environment = ownPluginsOnly();
    environment.insert(environment.end(), cache.environment.begin(), cache.environment.end());
    const frost::test::PrivateDesktop desktop;
    ASSERT_TRUE(desktop.valid());
    const auto process = startGui(frostExe(), environment, ImagePathForm::Auto, desktop);
    ASSERT_NE(process, nullptr);
    // From the build directory frost starts in place: frost.exe itself owns the window.
    HWND window = nullptr;
    for (const auto deadline = std::chrono::steady_clock::now() + 60s;
         window == nullptr && std::chrono::steady_clock::now() < deadline && !process->wait(0ms).has_value();
         std::this_thread::sleep_for(20ms)) {
        const auto shown = shownWindowsTitled(desktop.handle(), L"FrostIDE");
        window = shown.empty() ? nullptr : shown.front().first;
    }
    ASSERT_NE(window, nullptr) << "no FrostIDE window";
#else
    frost::io::ProcessSpec spec;
    spec.executable = frostExe();
    spec.environment = cache.environment;
    spec.environment.emplace_back("QT_QPA_PLATFORM", "offscreen");
    spec.stderrMode = frost::io::Stdio::MergeIntoStdout;
    auto launched = frost::io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(launched.has_value()) << launched.error().describe();
    const std::unique_ptr<frost::io::IProcess> process = std::move(*launched);
#endif
    const std::string log = waitForHandlerLine(cache.logs, 60s);
    const std::size_t firstFrame = log.find("first frame shown");
    const std::size_t registered = log.find("crash handler registered pid=");
    ASSERT_NE(registered, std::string::npos) << log;
    ASSERT_NE(firstFrame, std::string::npos) << log;
    EXPECT_LT(firstFrame, registered) << log;
    const std::string line = log.substr(registered, log.find('\n', registered) - registered);
    const std::uint64_t handler = std::stoull(fieldAfter(line, "pid"));
    EXPECT_TRUE(frost::test::processAlive(handler)) << line;
#ifdef _WIN32
    EXPECT_EQ(frost::io::parentPath(fieldAfter(line, "path")), frost::io::parentPath(appDll())) << line;
    EXPECT_EQ(fieldAfter(line, "restarted-from"), "") << "a start in place: " << line;
    EXPECT_EQ(crashHandlersOf(static_cast<DWORD>(process->pid())), std::vector<DWORD>{static_cast<DWORD>(handler)});
    ASSERT_NE(PostMessageW(window, WM_CLOSE, 0, 0), FALSE) << GetLastError();
#else
    EXPECT_EQ(frost::io::parentPath(fieldAfter(line, "path")), frost::io::parentPath(frostExe())) << line;
    ASSERT_EQ(::kill(static_cast<pid_t>(process->pid()), SIGTERM), 0);
#endif
    const auto result = process->communicate({}, 30s);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_TRUE(frost::test::waitForProcessGone(handler, 2s)) << "the crash handler outlived frost";
}

#ifdef _WIN32
TEST(FrostProcess, GuiStartsTheCrashHandlerThroughTheShortLink) {
    /*
     * A GUI start from a 310-unit image path restarts through the short link (ADR 0012); the
     * restarted frost starts exactly one crash handler, from the link (its path is under
     * <LOCALAPPDATA>\FrostIDE\i\), annotated with the first launcher as frost.restarted_from;
     * the first launcher starts none, and the handler ends with frost after a close.
     */
    const ScratchDir scratch("app-process-gui-link");
    const ImagePathForm form = longStartForm();
    const std::string exe = copyFrostTo(imagePathCell(longFormOf(scratch), 310, form));
    const std::string localAppData = localAppDataIn(scratch, "lad");
    const TestCache cache = testCacheIn(localAppData);
    Environment environment = ownPluginsOnly();
    environment.insert(environment.end(), cache.environment.begin(), cache.environment.end());
    const frost::test::PrivateDesktop desktop;
    ASSERT_TRUE(desktop.valid());
    const auto process = startGui(exe, environment, form, desktop);
    ASSERT_NE(process, nullptr);
    const auto [window, owner] = waitForRestartedWindow(desktop, L"FrostIDE", *process, 60s);
    if (window == nullptr) {
        const auto rest = process->communicate({}, 10s);
        FAIL() << "no FrostIDE window from a restarted frost; launcher output: "
               << (rest ? rest->stdoutBytes : rest.error().describe());
    }
    const std::string log = waitForHandlerLine(cache.logs, 60s);
    const std::size_t registered = log.find("crash handler registered pid=");
    ASSERT_NE(registered, std::string::npos) << log;
    const std::string line = log.substr(registered, log.find('\n', registered) - registered);
    const auto handler = static_cast<DWORD>(std::stoul(fieldAfter(line, "pid")));
    EXPECT_EQ(crashHandlersOf(owner), std::vector<DWORD>{handler}) << line;
    EXPECT_TRUE(crashHandlersOf(static_cast<DWORD>(process->pid())).empty()) << "the first launcher starts none";
    // frost started the handler by its path through the link; the kernel names the image by the
    // resolved path, which is the handler beside the long-path copy.
    const std::string linkFolder = frost::io::joinPath(localAppData, "FrostIDE\\i").value();
    EXPECT_TRUE(fieldAfter(line, "path").starts_with(linkFolder + "\\")) << line << " not under " << linkFolder;
    const std::string handlerImage = imagePathOf(handler);
    const std::string handlerPlain = handlerImage.starts_with("\\\\?\\") ? handlerImage.substr(4) : handlerImage;
    EXPECT_EQ(handlerPlain, frost::io::joinPath(frost::io::parentPath(exe), "crashpad_handler.exe").value());
    // frost.restarted_from is annotated with this value just before the line is logged.
    const std::string restartedFrom = fieldAfter(line, "restarted-from");
    EXPECT_TRUE(restartedFrom == startedAs(exe, form) || restartedFrom == exe) << line;
    ASSERT_NE(PostMessageW(window, WM_CLOSE, 0, 0), FALSE) << GetLastError();
    const auto result = process->communicate({}, 30s);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 0) << result->stdoutBytes;
    EXPECT_TRUE(frost::test::waitForProcessGone(handler, 2s)) << "the crash handler outlived frost";
}
#endif

} // namespace
