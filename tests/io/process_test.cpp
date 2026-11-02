// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for the real IProcessLauncher, driving io_test_child (see
// io_test_child.cpp): exact argv round trips (spaces, quotes, backslashes,
// empty and non-ASCII arguments), exit codes, stdin piping, more than 1 MB on
// both output pipes at once, timeouts with termination, environment overrides,
// working directories, handle inheritance, the absolute-path and batch-file
// rules, process-tree containment (listing, ending one process, releasing the
// container, abandoning held output) and concurrent launches; on Windows also the
// image-path forms (ProcessSpec::imagePathForm) and the child's desktop
// (ProcessSpec::desktop: a private desktop from tests/support/private_desktop.hpp, as
// the child reports it and as its window shows, and one that does not exist); on POSIX,
// that both fields are ignored.

#include "frost/core/mutex.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include "internal.hpp"    // detail::maxWaitSliceMs and its test setter.
#include "process_env.hpp" // childSpec, pattern, ProcessWatch.
#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include "frost/io/utf.hpp"

#include "support/private_desktop.hpp"

#include <windows.h>
// ConvertSidToStringSidW; sddl.h needs windows.h first.
#include <sddl.h>
#else
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include <csignal>
#include <cstdlib>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using namespace std::string_literals;
using frost::ErrorCode;
using frost::io::ProcessResult;
using frost::io::ProcessSpec;
using frost::io::Stdio;
using frost::test::childPath;
using frost::test::childSpec;
using frost::test::pattern;
using frost::test::ProcessWatch;
using frost::test::ScratchDir;

frost::Result<ProcessResult> run(const ProcessSpec& spec, std::string_view stdinData = {},
                                 frost::IClock::Duration timeout = 120s) {
    return frost::io::runToCompletion(frost::io::realProcessLauncher(), spec, stdinData, timeout);
}

std::string unhex(std::string_view hex) {
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out += static_cast<char>(digit(hex[i]) * 16 + digit(hex[i + 1]));
    }
    return out;
}

std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        out.push_back(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    }
    return out;
}

// The arguments the child reported under `header` ("crt" or "shell").
std::vector<std::string> reportedArguments(std::string_view output, std::string_view header) {
    const std::vector<std::string_view> all = lines(output);
    std::vector<std::string> args;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].starts_with(header) && all[i].size() > header.size() && all[i][header.size()] == ' ') {
            const std::size_t count = std::stoul(std::string(all[i].substr(header.size() + 1)));
            for (std::size_t k = 1; k <= count && i + k < all.size(); ++k) {
                args.push_back(unhex(all[i + k].substr(4))); // Strip "arg:".
            }
        }
    }
    return args;
}

// The single "<prefix>:<hex>" value the child printed, decoded; "<unset>" for "unset".
std::string reportedValue(std::string_view output, std::string_view prefix) {
    for (const std::string_view line : lines(output)) {
        if (line == "unset") {
            return "<unset>";
        }
        if (line.starts_with(prefix) && line.size() > prefix.size() && line[prefix.size()] == ':') {
            return unhex(line.substr(prefix.size() + 1));
        }
    }
    return "<missing>";
}

#ifdef _WIN32
// The rest of the first line starting with `prefix` (not hex-encoded), or "<missing>".
std::string rawValue(std::string_view output, std::string_view prefix) {
    for (const std::string_view line : lines(output)) {
        if (line.starts_with(prefix)) {
            return std::string(line.substr(prefix.size()));
        }
    }
    return "<missing>: " + std::string(output);
}
#endif

void setParentVariable(const char* name, const char* value) {
#ifdef _WIN32
    const std::wstring wideName = frost::io::toWide(name).value();
    const std::wstring wideValue = frost::io::toWide(value).value();
    ASSERT_NE(SetEnvironmentVariableW(wideName.c_str(), wideValue.c_str()), FALSE);
#else
    ASSERT_EQ(setenv(name, value, 1), 0);
#endif
}

TEST(RealProcessLauncher, RelativeExecutablesAreRejected) {
    for (const char* relative : {"io_test_child", "./io_test_child", "bin/io_test_child"}) {
        ProcessSpec spec;
        spec.executable = relative;
        const auto result = frost::io::realProcessLauncher().launch(spec);
        ASSERT_FALSE(result.has_value()) << relative;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(RealProcessLauncher, MissingExecutableIsNotFound) {
    const ScratchDir dir("proc-missing");
    ProcessSpec spec;
    spec.executable = dir.join("no-such-tool.exe");
    const auto result = run(spec);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::NotFound) << result.error().describe();
}

TEST(RealProcessLauncher, ArgumentsRoundTripExactly) {
    const std::vector<std::string> tricky = {
        "plain",
        "with space",
        "tab\there",
        "quote\"inside",
        "\"",
        "\"\"",
        "\\\"",
        "trailing\\",
        "trailing two\\\\",
        "C:\\Program Files\\dir\\",
        "\\\\server\\share\\",
        "",
        "  ",
        "a\\\\\\\"b",
        "new\nline",
        "ünïcødé 测试 😀",
        "%PATH% $HOME `x` & | < > ^ ! ' ;",
        "*?[]{}",
        "",
    };
    std::vector<std::string> arguments{"argv"};
    arguments.insert(arguments.end(), tricky.begin(), tricky.end());
    const auto result = run(childSpec(arguments));
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    ASSERT_EQ(result->exitCode, 0) << result->stderrBytes;
    EXPECT_EQ(reportedArguments(result->stdoutBytes, "crt"), tricky) << result->stdoutBytes;
#ifdef _WIN32
    // The same command line read by CommandLineToArgvW, the other common Windows parser.
    EXPECT_EQ(reportedArguments(result->stdoutBytes, "shell"), tricky) << result->stdoutBytes;
#endif
}

#ifdef _WIN32
// ProcessSpec::rawArguments (ADR 0040): the tail reaches the child verbatim, so the C runtime's
// own rules split it (the run service passes the user's argument string this way to cmd.exe).
TEST(RealProcessLauncher, RawArgumentsReachTheChildVerbatim) {
    ProcessSpec spec = childSpec({});
    spec.rawArguments = "argv  \"two  words\" a\\\\\\\"b \"\" last";
    const auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    ASSERT_EQ(result->exitCode, 0) << result->stderrBytes;
    EXPECT_EQ(reportedArguments(result->stdoutBytes, "crt"),
              (std::vector<std::string>{"two  words", "a\\\"b", "", "last"}))
        << result->stdoutBytes;
}
#endif

/*
 * Lets a copy of io_test_child, started from another directory, find the DLLs that
 * the build deploys next to the original (the ASan runtime in ASan builds): the
 * original's directory goes on the copy's PATH, part of the loader's search order.
 * PATH rather than copying the DLLs, so it also works from directories beyond
 * MAX_PATH. POSIX executables need nothing (sanitizer runtimes are linked statically).
 */
void findChildDlls(ProcessSpec& spec) {
#ifdef _WIN32
    const std::string directory(frost::io::parentPath(childPath()));
    std::wstring path(32767, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"PATH", path.data(), static_cast<DWORD>(path.size()));
    path.resize(length < path.size() ? length : 0);
    spec.environment.emplace_back("PATH", directory + ";" + frost::io::fromWide(path).value());
#else
    static_cast<void>(spec);
#endif
}

TEST(RealProcessLauncher, ExecutableInADirectoryWithSpacesAndNonAscii) {
    const ScratchDir dir("proc-exe ünï 测试");
    std::error_code error;
    const std::filesystem::path source = frost::io::toStdPath(childPath()).value();
    const std::filesystem::path target =
        frost::io::toStdPath(dir.join("child copy ✓" + source.extension().string())).value();
    std::filesystem::copy_file(source, target, error);
    ASSERT_FALSE(error) << error.message();
    ProcessSpec spec = childSpec({"argv", "x y"});
    spec.executable = frost::io::fromStdPath(target).value();
    findChildDlls(spec);
    const auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    ASSERT_EQ(result->exitCode, 0) << "the copied child did not run: " << result->stderrBytes;
    EXPECT_EQ(reportedArguments(result->stdoutBytes, "crt"), (std::vector<std::string>{"x y"}));
}

#ifdef _WIN32
// Whether two spellings name the same directory: volume serial number and 128-bit file id.
bool sameDirectory(const std::string& a, const std::string& b) {
    const auto identity = [](const std::string& path) -> std::optional<FILE_ID_INFO> {
        auto wide = frost::io::toWin32Path(path);
        if (!wide) {
            return std::nullopt;
        }
        const HANDLE handle =
            CreateFileW(wide->c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return std::nullopt;
        }
        FILE_ID_INFO info{};
        const BOOL ok = GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info));
        CloseHandle(handle);
        return ok ? std::optional<FILE_ID_INFO>(info) : std::nullopt;
    };
    const auto first = identity(a);
    const auto second = identity(b);
    return first && second && first->VolumeSerialNumber == second->VolumeSerialNumber &&
           std::memcmp(first->FileId.Identifier, second->FileId.Identifier, sizeof(first->FileId.Identifier)) == 0;
}
#endif

// A directory under `root` whose path is longer than Windows' MAX_PATH (260 UTF-16 units).
std::string deepDirectory(const std::string& root) {
    std::string deep = root;
    while (deep.size() < 300) {
        deep = frost::io::joinPath(deep, std::string(40, 'x') + " segment").value();
    }
    EXPECT_TRUE(frost::io::realFileSystem().createDirectories(deep).has_value());
    return deep;
}

TEST(RealProcessLauncher, ExecutableBeyondMaxPath) {
    // Must work even with LongPathsEnabled = 0, as on the reference machine.
    const ScratchDir dir("proc-long-exe");
    const std::string deep = deepDirectory(dir.path());
#ifdef _WIN32
    const std::string copy = frost::io::joinPath(deep, "child ✓.exe").value();
#else
    const std::string copy = frost::io::joinPath(deep, "child ✓").value();
#endif
    const auto image = frost::io::realFileSystem().readFile(childPath());
    ASSERT_TRUE(image.has_value()) << image.error().describe();
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(copy, *image).has_value());
#ifndef _WIN32
    ASSERT_EQ(chmod(copy.c_str(), 0755), 0);
#endif
    ASSERT_GT(copy.size(), 300u);
    ProcessSpec spec = childSpec({"argv", "long"});
    spec.executable = copy;
    findChildDlls(spec);
    const auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    ASSERT_EQ(result->exitCode, 0) << "the copied child did not run: " << result->stderrBytes;
    EXPECT_EQ(reportedArguments(result->stdoutBytes, "crt"), (std::vector<std::string>{"long"}));
}

#ifdef _WIN32
// Whether this process's long paths are effective: RtlAreLongPathsEnabled (ntdll), true only
// when LongPathsEnabled=1 and the test's manifest (FROST_TEST_MANIFEST_RC) is longPathAware.
bool longPathsEffective() {
    using Query = BOOLEAN(NTAPI*)();
    const FARPROC proc = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlAreLongPathsEnabled");
    return proc != nullptr && reinterpret_cast<Query>(reinterpret_cast<void*>(proc))() != FALSE;
}

// A copy of io_test_child in a directory beyond MAX_PATH below `dir`; returns its path.
std::string deepChildCopy(const ScratchDir& dir) {
    const std::string copy = frost::io::joinPath(deepDirectory(dir.path()), "child ✓.exe").value();
    const auto image = frost::io::realFileSystem().readFile(childPath());
    EXPECT_TRUE(image.has_value()) << image.error().describe();
    EXPECT_TRUE(frost::io::realFileSystem().writeFileAtomic(copy, image.value_or(std::string())).has_value());
    return copy;
}

TEST(RealProcessLauncher, LaunchesThroughExtendedImagePathBelowMaxPath) {
    // Extended adds the prefix even where Auto would not, and the child sees that form as
    // its own image path (so does the loader for the DLLs beside it).
    ProcessSpec spec = childSpec({"image-path"});
    ASSERT_LT(frost::io::utf8ToUtf16(spec.executable).value().size(), std::size_t{MAX_PATH}) << spec.executable;
    spec.imagePathForm = frost::io::ImagePathForm::Extended;
    auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    ASSERT_EQ(result->exitCode, 0) << result->stderrBytes;
    EXPECT_EQ(reportedValue(result->stdoutBytes, "image"), "\\\\?\\" + childPath()) << result->stdoutBytes;

    // Control: Auto keeps the plain form below MAX_PATH.
    spec.imagePathForm = frost::io::ImagePathForm::Auto;
    result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "image"), childPath()) << result->stdoutBytes;

    // Beyond MAX_PATH, Auto and Extended agree.
    const ScratchDir dir("proc-extended-exe");
    spec.executable = deepChildCopy(dir);
    findChildDlls(spec);
    for (const auto form : {frost::io::ImagePathForm::Auto, frost::io::ImagePathForm::Extended}) {
        spec.imagePathForm = form;
        result = run(spec);
        ASSERT_TRUE(result.has_value()) << result.error().describe();
        ASSERT_EQ(result->exitCode, 0) << result->stderrBytes;
        EXPECT_EQ(reportedValue(result->stdoutBytes, "image"), "\\\\?\\" + spec.executable) << result->stdoutBytes;
    }
}

TEST(RealProcessLauncher, LaunchesThroughPlainImagePath) {
    // Below MAX_PATH, Plain is what Auto does.
    ProcessSpec spec = childSpec({"image-path"});
    spec.imagePathForm = frost::io::ImagePathForm::Plain;
    auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    ASSERT_EQ(result->exitCode, 0) << result->stderrBytes;
    EXPECT_EQ(reportedValue(result->stdoutBytes, "image"), childPath()) << result->stdoutBytes;

    // Beyond MAX_PATH, Plain still never adds the prefix. CreateProcessW then accepts the
    // path only if this process's long paths are effective, and the child sees it unchanged.
    const ScratchDir dir("proc-plain-exe");
    spec.executable = deepChildCopy(dir);
    findChildDlls(spec);
    result = run(spec);
    if (!longPathsEffective()) {
        EXPECT_FALSE(result.has_value()) << "a plain path of " << spec.executable.size()
                                         << " bytes started although long paths are not effective";
        return;
    }
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    ASSERT_EQ(result->exitCode, 0) << result->stderrBytes;
    EXPECT_EQ(reportedValue(result->stdoutBytes, "image"), spec.executable) << result->stdoutBytes;
}

TEST(RealProcessLauncher, RunsChildOnTheGivenDesktop) {
    const frost::test::PrivateDesktop desktop;
    ASSERT_TRUE(desktop.valid());
    ProcessSpec spec = childSpec({"desktop"});
    spec.desktop = desktop.name();
    auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    ASSERT_EQ(result->exitCode, 0) << result->stdoutBytes << result->stderrBytes;
    EXPECT_EQ(reportedValue(result->stdoutBytes, "desktop"), desktop.name()) << result->stdoutBytes;

    // Control: without a desktop the child shares this thread's, which is not the private one.
    const std::string own = frost::test::PrivateDesktop::threadDesktopName();
    ASSERT_FALSE(own.empty());
    spec.desktop.clear();
    result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "desktop"), own) << result->stdoutBytes;
    EXPECT_NE(own, desktop.name());
}

// The top-level windows of class "Static" that process `pid` owns on `desktop`, shown or not.
std::vector<HWND> staticWindowsOf(HDESK desktop, DWORD pid) {
    struct Search {
        DWORD pid;
        std::vector<HWND> found;
    } search{pid, {}};
    const WNDENUMPROC collect = [](HWND window, LPARAM context) -> BOOL {
        auto& into = *reinterpret_cast<Search*>(context);
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        std::array<wchar_t, 32> className{};
        const int length = GetClassNameW(window, className.data(), static_cast<int>(className.size()));
        if (owner == into.pid && std::wstring_view(className.data(), static_cast<std::size_t>(length)) == L"Static") {
            into.found.push_back(window);
        }
        return TRUE;
    };
    // FALSE also means "no windows at all", so the result alone is the answer.
    static_cast<void>(EnumDesktopWindows(desktop, collect, reinterpret_cast<LPARAM>(&search)));
    return search.found;
}

TEST(RealProcessLauncher, ChildWindowsLandOnTheGivenDesktop) {
    // RunsChildOnTheGivenDesktop trusts the child's report; this looks from the parent's side.
    // A window belongs to the desktop of the thread that creates it, so the child's window
    // (never shown, so it would stay invisible even on the user's desktop) must turn up when
    // this process enumerates the private desktop.
    const frost::test::PrivateDesktop desktop;
    ASSERT_TRUE(desktop.valid());
    ProcessSpec spec = childSpec({"window"});
    spec.stdinMode = Stdio::Pipe;
    spec.desktop = desktop.name();
    auto process = frost::io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const auto pid = static_cast<DWORD>((*process)->pid());
    // The child creates its window right after start and keeps it until stdin closes; a
    // child that exits first (it could not create one) ends the wait early.
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    std::vector<HWND> found = staticWindowsOf(desktop.handle(), pid);
    while (found.empty() && std::chrono::steady_clock::now() < deadline && !(*process)->wait(0ms).has_value()) {
        std::this_thread::sleep_for(10ms);
        found = staticWindowsOf(desktop.handle(), pid);
    }
    (*process)->closeStdin();
    const auto result = (*process)->communicate({}, 30s);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 0) << result->stdoutBytes << result->stderrBytes;
    ASSERT_EQ(found.size(), 1u) << "the child's window is not on the private desktop: " << result->stdoutBytes;
    EXPECT_EQ(rawValue(result->stdoutBytes, "window:"), std::to_string(reinterpret_cast<std::uintptr_t>(found[0])));
}

TEST(RealProcessLauncher, MissingDesktopEndsTheChildBeforeItRuns) {
    /*
     * A desktop that does not exist is not a launch error (see ProcessSpec::desktop):
     * CreateProcessW succeeds, the child's user32 cannot attach to the desktop and fails
     * to initialise, and the loader ends the child with STATUS_DLL_INIT_FAILED before its
     * main runs. So it never runs elsewhere, on the user's desktop say, and prints nothing.
     * The loader raises that as a hard error, which NoHardErrorBoxes keeps from becoming a box.
     */
    const frost::test::NoHardErrorBoxes noHardErrorBoxes;
    const frost::test::PrivateDesktop existing;
    ASSERT_TRUE(existing.valid());
    ProcessSpec spec = childSpec({"desktop"});
    spec.desktop = existing.name() + "-missing"; // In this window station, under a name nobody created.
    const auto result = run(spec, {}, 60s);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(static_cast<DWORD>(result->exitCode), DWORD{0xC0000142}) // STATUS_DLL_INIT_FAILED
        << result->exitCode << " " << result->stderrBytes;
    EXPECT_EQ(result->stdoutBytes, "");
}

TEST(RealProcessLauncher, RejectsDesktopNameWithNul) {
    // CreateProcessW would read a NUL as the end of the name and start the child elsewhere.
    // Should the check ever fail, the child dies as in MissingDesktopEndsTheChildBeforeItRuns,
    // so its hard error must not become a box either.
    const frost::test::NoHardErrorBoxes noHardErrorBoxes;
    for (const std::string& name : {std::string("WinSta0\\frost\0Default", 21), std::string("WinSta0\\\xff")}) {
        ProcessSpec spec = childSpec({"exit", "0"});
        spec.desktop = name;
        const auto result = frost::io::realProcessLauncher().launch(spec);
        ASSERT_FALSE(result.has_value()) << "started with desktop " << name;
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << result.error().describe();
    }
}
#else
TEST(RealProcessLauncher, WindowsOnlyFieldsAreIgnoredOnPosix) {
    // imagePathForm and desktop only steer CreateProcessW. posix_spawn never reads them, so a
    // \\?\ form or a desktop that exists nowhere changes nothing here.
    ProcessSpec spec = childSpec({"argv", "plain"});
    spec.imagePathForm = frost::io::ImagePathForm::Extended;
    spec.desktop = "WinSta0\\frost-no-such-desktop";
    const auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 0) << result->stderrBytes;
    EXPECT_EQ(reportedArguments(result->stdoutBytes, "crt"), (std::vector<std::string>{"plain"}));
}
#endif

TEST(RealProcessLauncher, WorkingDirectoryBeyondMaxPath) {
    const ScratchDir dir("proc-long-cwd");
    const std::string deep = deepDirectory(dir.path());
    ProcessSpec spec = childSpec({"cwd"});
    spec.workingDirectory = deep;
    const auto result = run(spec);
#ifdef _WIN32
    // Windows refuses such a current directory unless long paths are enabled system-wide
    // (see workingDirectoryError in process_win.cpp); the error must say so, not claim
    // the directory is missing.
    if (!result.has_value()) {
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << result.error().describe();
        EXPECT_NE(result.error().message().find("MAX_PATH"), std::string::npos) << result.error().describe();
        return;
    }
#endif
    ASSERT_TRUE(result.has_value()) << result.error().describe();
#ifdef _WIN32
    // The child may run in the directory's 8.3 short spelling (the fallback in process_win.cpp), so compare
    // identities rather than strings.
    EXPECT_TRUE(sameDirectory(reportedValue(result->stdoutBytes, "cwd"), deep)) << result->stdoutBytes;
#else
    EXPECT_EQ(frost::io::normalizePath(reportedValue(result->stdoutBytes, "cwd")).value(), deep) << result->stdoutBytes;
#endif
}

#ifdef _WIN32
// The fallback runs whatever LongPathsEnabled says: the test hook makes the launcher treat the long
// directory as refused, so the 8.3 retry is what makes the launch succeed.
TEST(RealProcessLauncher, LongWorkingDirectoryFallsBackToTheShortName) {
    const ScratchDir dir("proc-short-cwd");
    const std::string deep = deepDirectory(dir.path());
    struct Reset {
        ~Reset() { frost::io::detail::setRefuseLongWorkingDirectoryForTesting(false); }
    } reset;
    frost::io::detail::setRefuseLongWorkingDirectoryForTesting(true);
    ProcessSpec spec = childSpec({"cwd"});
    spec.workingDirectory = deep;
    const auto result = run(spec);
    const auto shortened = frost::io::detail::shortWorkingDirectory(frost::io::normalizePath(deep).value());
    if (!shortened) {
        // The volume makes no 8.3 names (fsutil 8dot3name): the refusal must still be explained.
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << result.error().describe();
        EXPECT_NE(result.error().message().find("MAX_PATH"), std::string::npos) << result.error().describe();
        GTEST_SKIP() << "the scratch volume has no 8.3 names";
    }
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    const std::string reported = reportedValue(result->stdoutBytes, "cwd");
    EXPECT_LT(reported.size(), std::size_t{MAX_PATH}) << reported;
    EXPECT_TRUE(sameDirectory(reported, deep)) << reported;
}

TEST(RealProcessLauncher, ShortWorkingDirectoryOfAShortPathIsItsOwnSpelling) {
    const ScratchDir dir("proc-short-name");
    ASSERT_TRUE(frost::io::realFileSystem().createDirectories(dir.path()).has_value());
    const auto shortened = frost::io::detail::shortWorkingDirectory(dir.path());
    ASSERT_TRUE(shortened.has_value());
    EXPECT_FALSE(shortened->starts_with(L"\\\\?\\"));
    EXPECT_TRUE(sameDirectory(frost::io::fromWide(*shortened).value(), dir.path()));
}
#endif

TEST(RealProcessLauncher, WorkingDirectoryThatIsAFile) {
    const ScratchDir dir("proc-cwd-file");
    const std::string file = dir.join("not-a-dir");
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(file, "x").has_value());
    ProcessSpec spec = childSpec({"cwd"});
    spec.workingDirectory = file;
    const auto result = run(spec);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument) << result.error().describe();
    EXPECT_NE(result.error().message().find("is not a directory"), std::string::npos) << result.error().describe();
}

TEST(RealProcessLauncher, ExitCodes) {
    for (const int code : {0, 1, 3, 42, 255}) {
        const auto result = run(childSpec({"exit", std::to_string(code)}));
        ASSERT_TRUE(result.has_value()) << result.error().describe();
        EXPECT_EQ(result->exitCode, code);
        EXPECT_EQ(result->signal, 0);
    }
}

TEST(RealProcessLauncher, StdinIsPiped) {
    ProcessSpec spec = childSpec({"cat"});
    spec.stdinMode = Stdio::Pipe;
    const std::string input = "first line\nsecond line\r\n\0\xff end"s; // The s suffix keeps the NUL.
    const auto result = run(spec, input);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->stdoutBytes, input);
    EXPECT_EQ(result->exitCode, 0);
}

TEST(RealProcessLauncher, MoreThanOneMegabyteOnBothPipes) {
    // The child alternates 4 KiB chunks between stdout and stderr: draining one pipe at a
    // time would deadlock as soon as the other pipe's buffer fills.
    constexpr std::size_t kSize = 3 * 1024 * 1024;
    const auto result = run(childSpec({"flood", std::to_string(kSize)}));
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 0);
    ASSERT_EQ(result->stdoutBytes.size(), kSize);
    ASSERT_EQ(result->stderrBytes.size(), kSize);
    const std::string expected = pattern(kSize);
    EXPECT_TRUE(result->stdoutBytes == expected);
    EXPECT_TRUE(result->stderrBytes == expected);
}

TEST(RealProcessLauncher, LargeStdinEchoedOnBothPipes) {
    // Feeding stdin while both outputs fill: stdin, stdout and stderr all exceed the pipe buffers.
    ProcessSpec spec = childSpec({"tee"});
    spec.stdinMode = Stdio::Pipe;
    const std::string input = pattern(2 * 1024 * 1024 + 17);
    const auto result = run(spec, input);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_TRUE(result->stdoutBytes == input);
    EXPECT_TRUE(result->stderrBytes == input);
}

TEST(RealProcessLauncher, ChildThatIgnoresStdinDoesNotBreakCommunicate) {
    // The child exits without reading; the broken pipe must not fail the call (or raise SIGPIPE).
    ProcessSpec spec = childSpec({"exit", "7"});
    spec.stdinMode = Stdio::Pipe;
    const auto result = run(spec, pattern(4 * 1024 * 1024));
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 7);
}

TEST(RealProcessLauncher, TimeoutTerminatesTheProcess) {
    const auto start = std::chrono::steady_clock::now();
    const auto result = run(childSpec({"sleep", "60000"}), {}, 300ms);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout) << result.error().describe();
    EXPECT_LT(elapsed, 20s);
}

TEST(RealProcessLauncher, WaitTimeoutThenTerminate) {
    auto process = frost::io::realProcessLauncher().launch(childSpec({"sleep", "60000"}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    EXPECT_GT((*process)->pid(), 0u);
    const auto early = (*process)->wait(50ms);
    ASSERT_FALSE(early.has_value());
    EXPECT_EQ(early.error().code(), ErrorCode::Timeout);
    ASSERT_TRUE((*process)->terminate().has_value());
    const auto status = (*process)->wait(30s);
    ASSERT_TRUE(status.has_value()) << status.error().describe();
    EXPECT_EQ(status->exitCode, frost::io::kTerminatedExitCode);
#ifndef _WIN32
    EXPECT_EQ(status->signal, SIGKILL);
#endif
    // terminate() after exit is harmless, and wait() keeps answering.
    EXPECT_TRUE((*process)->terminate().has_value());
    EXPECT_EQ((*process)->wait().value(), *status);
}

TEST(RealProcessLauncher, StreamingPipes) {
    ProcessSpec spec = childSpec({"cat"});
    spec.stdinMode = Stdio::Pipe;
    auto process = frost::io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    ASSERT_TRUE((*process)->writeStdin("hello ").has_value());
    ASSERT_TRUE((*process)->writeStdin("world").has_value());
    (*process)->closeStdin();
    (*process)->closeStdin(); // Idempotent.
    EXPECT_EQ((*process)->writeStdin("late").error().code(), ErrorCode::InvalidArgument);
    std::string out;
    std::array<char, 3> buffer{};
    for (;;) {
        const auto got = (*process)->readStdout(buffer);
        ASSERT_TRUE(got.has_value()) << got.error().describe();
        if (*got == 0) {
            break;
        }
        out.append(buffer.data(), *got);
    }
    EXPECT_EQ(out, "hello world");
    EXPECT_EQ((*process)->readStderr(buffer).value(), 0u);
    EXPECT_EQ((*process)->wait(30s).value().exitCode, 0);
}

TEST(RealProcessLauncher, StdioModes) {
    ProcessSpec merged = childSpec({"print", "OUT", "ERR"});
    merged.stderrMode = Stdio::MergeIntoStdout;
    auto result = run(merged);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->stdoutBytes, "OUTERR");
    EXPECT_EQ(result->stderrBytes, "");

    ProcessSpec nulled = childSpec({"print", "OUT", "ERR"});
    nulled.stdoutMode = Stdio::Null;
    result = run(nulled);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->stdoutBytes, "");
    EXPECT_EQ(result->stderrBytes, "ERR");

    ProcessSpec nullInput = childSpec({"cat"});
    nullInput.stdinMode = Stdio::Null;
    result = run(nullInput);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->stdoutBytes, "");

    ProcessSpec inherited = childSpec({"exit", "0"});
    inherited.stdinMode = Stdio::Inherit;
    inherited.stdoutMode = Stdio::Inherit;
    inherited.stderrMode = Stdio::Inherit;
    result = run(inherited);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 0);

    // Data for a stdin that is not a pipe is a caller bug.
    result = run(childSpec({"exit", "0"}), "data");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(RealProcessLauncher, EnvironmentOverrides) {
    setParentVariable("FROST_IO_TEST_INHERITED", "from parent");
    ProcessSpec spec = childSpec({"env", "FROST_IO_TEST_INHERITED"});
    auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "from parent");

    spec.environment = {{"FROST_IO_TEST_INHERITED", "välue ✓ with = sign"}};
    result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "välue ✓ with = sign");

    spec.environment = {{"FROST_IO_TEST_INHERITED", std::nullopt}};
    result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "<unset>");

    // Later overrides win.
    spec.environment = {{"FROST_IO_TEST_INHERITED", "first"}, {"FROST_IO_TEST_INHERITED", "second"}};
    result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "second");

#ifdef _WIN32
    // Windows treats variable names case-insensitively, so must the overrides.
    spec.environment = {{"frost_io_test_inherited", std::nullopt}};
    result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "<unset>");
#endif
}

TEST(RealProcessLauncher, CleanEnvironment) {
    setParentVariable("FROST_IO_TEST_INHERITED", "from parent");
    ProcessSpec spec = childSpec({"env", "FROST_IO_TEST_INHERITED"});
    spec.cleanEnvironment = true;
    spec.environment = {{"ONLY_THIS", "1"}};
    auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "<unset>");

    spec.arguments = {"env", "ONLY_THIS"};
    result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "1");

    spec.arguments = {"env", "PATH"};
    result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(reportedValue(result->stdoutBytes, "set"), "<unset>");
}

TEST(RealProcessLauncher, WorkingDirectory) {
    const ScratchDir dir("proc-cwd ünïcødé 测试");
    ProcessSpec spec = childSpec({"cwd"});
    spec.workingDirectory = dir.path();
    const auto result = run(spec);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(frost::io::normalizePath(reportedValue(result->stdoutBytes, "cwd")).value(), dir.path());

    spec.workingDirectory = dir.join("does-not-exist");
    const auto missing = run(spec);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound) << missing.error().describe();

    spec.workingDirectory = "relative";
    EXPECT_EQ(run(spec).error().code(), ErrorCode::InvalidArgument);
}

#ifndef _WIN32
// The probe identity of an open descriptor: "<st_dev>:<st_ino>".
std::string fileIdentity(int fd) {
    struct stat info{};
    EXPECT_EQ(fstat(fd, &info), 0);
    return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
}
#endif

TEST(RealProcessLauncher, OnlyTheStandardHandlesAreInherited) {
    // The child compares object identity, not just handle validity (see io_test_child.cpp),
    // so its own handles with the same numeric value cannot fake a result.
#ifdef _WIN32
    // An inheritable handle that a plain CreateProcessW(bInheritHandles=TRUE) would pass on,
    // to an event whose unique name lets the child find the same object independently.
    const std::string name = std::format("Local\\frost-io-probe-{}-{}", GetCurrentProcessId(),
                                         std::chrono::steady_clock::now().time_since_epoch().count());
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const HANDLE event = CreateEventW(&inherit, TRUE, FALSE, std::wstring(name.begin(), name.end()).c_str());
    ASSERT_NE(event, nullptr);
    const auto result = run(childSpec({"probe-handle", std::to_string(reinterpret_cast<std::uintptr_t>(event)), name}));

    // Control: the probe recognises the event when the child really has it, here as its
    // inherited stdin (Stdio::Inherit duplicates the parent's standard input handle).
    const HANDLE savedStdin = GetStdHandle(STD_INPUT_HANDLE);
    ASSERT_NE(SetStdHandle(STD_INPUT_HANDLE, event), FALSE);
    ProcessSpec controlSpec = childSpec({"probe-handle", "stdin", name});
    controlSpec.stdinMode = Stdio::Inherit;
    const auto control = run(controlSpec);
    SetStdHandle(STD_INPUT_HANDLE, savedStdin);
    CloseHandle(event);
    ASSERT_TRUE(control.has_value()) << control.error().describe();
    EXPECT_EQ(control->stdoutBytes, "inherited\n");
#else
    // A descriptor without O_CLOEXEC, as careless code elsewhere in the process might open.
    const ScratchDir dir("proc-probe");
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(dir.join("probe"), "").has_value());
    const int fd = open(dir.join("probe").c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    const auto result = run(childSpec({"probe-handle", std::to_string(fd), fileIdentity(fd)}));
    close(fd);
#endif
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->stdoutBytes, "not inherited\n");
#ifndef _WIN32
    // Control: the probe recognises a descriptor the child really has (stdin = /dev/null).
    const int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(null, 0);
    const std::string nullIdentity = fileIdentity(null);
    close(null);
    const auto control = run(childSpec({"probe-handle", "stdin", nullIdentity}));
    ASSERT_TRUE(control.has_value()) << control.error().describe();
    EXPECT_EQ(control->stdoutBytes, "inherited\n");
#endif
}

#ifndef _WIN32
// ProcessSpec::posixInheritedFds entry 0 arrives as fd 3: a socket the child writes into.
TEST(RealProcessLauncher, PassesInheritedSocketAsFd3) {
    int pair[2] = {-1, -1};
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair), 0);
    ProcessSpec spec = childSpec({"fd-write", "3", "hello from fd 3"});
    spec.posixInheritedFds = {pair[1]};
    const auto result = run(spec);
    close(pair[1]);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->stdoutBytes, "wrote\n");
    std::array<char, 64> buffer{};
    const ssize_t got = recv(pair[0], buffer.data(), buffer.size(), MSG_DONTWAIT);
    close(pair[0]);
    ASSERT_GT(got, 0);
    EXPECT_EQ(std::string_view(buffer.data(), static_cast<std::size_t>(got)), "hello from fd 3");
}

// A descriptor passed to one child reaches neither a child started without it (even while the
// first runs) nor, through the launcher's staged copy, any other descriptor number.
TEST(RealProcessLauncher, InheritedFdIsNotLeakedToOtherChildren) {
    const ScratchDir dir("proc-inherit");
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(dir.join("probe"), "").has_value());
    // Without O_CLOEXEC, so only the launcher's closefrom keeps it from other children.
    const int fd = open(dir.join("probe").c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    const std::string identity = fileIdentity(fd);

    ProcessSpec holder = childSpec({"find-identity", identity});
    holder.posixInheritedFds = {fd};
    const auto passed = run(holder);
    ASSERT_TRUE(passed.has_value()) << passed.error().describe();
    EXPECT_EQ(passed->stdoutBytes, "found 3\ndone\n"); // Only at fd 3, never at the staged number.

    // A long-running child that holds the descriptor, then another child without it.
    ProcessSpec sleeper = childSpec({"sleep", "2000"});
    sleeper.posixInheritedFds = {fd};
    auto running = frost::io::realProcessLauncher().launch(sleeper);
    ASSERT_TRUE(running.has_value()) << running.error().describe();
    const auto other = run(childSpec({"find-identity", identity}));
    (void)(*running)->terminate();
    close(fd);
    ASSERT_TRUE(other.has_value()) << other.error().describe();
    EXPECT_EQ(other->stdoutBytes, "done\n");
}

/*
 * A source that already has its target number keeps FD_CLOEXEC under dup2(fd, fd), so the
 * launcher must stage it: here the source is fd k (k >= 3), listed at index k - 3 so its target
 * is k, and it is close-on-exec in this process.
 */
TEST(RealProcessLauncher, InheritedFdThatEqualsItsTargetStillReachesTheChild) {
    const ScratchDir dir("proc-inherit-same");
    ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(dir.join("probe"), "").has_value());
    const int fd = open(dir.join("probe").c_str(), O_RDONLY | O_CLOEXEC);
    ASSERT_GE(fd, 3);
    if (fd - 3 >= static_cast<int>(frost::io::kMaxInheritedFds)) {
        close(fd);
        GTEST_SKIP() << "fd " << fd << " is beyond the inherited-descriptor limit";
    }
    // Fillers for the indices before k - 3: /dev/null descriptors, all distinct.
    std::vector<int> fillers;
    for (int i = 0; i < fd - 3; ++i) {
        const int filler = open("/dev/null", O_RDONLY | O_CLOEXEC);
        ASSERT_GE(filler, 0);
        fillers.push_back(filler);
    }
    std::vector<int> inherited = fillers;
    inherited.push_back(fd);
    ProcessSpec spec = childSpec({"find-identity", fileIdentity(fd)});
    spec.posixInheritedFds = inherited;
    const auto result = run(spec);
    for (const int filler : fillers) {
        close(filler);
    }
    const std::string expected = std::format("found {}\ndone\n", fd);
    close(fd);
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->stdoutBytes, expected);
}
#endif

#ifdef _WIN32
TEST(RealProcessLauncher, BatchFilesAreRefused) {
    // Real batch files, so a spelling that slipped through would visibly run cmd.exe. The
    // other names hold batch content too, so they exist and are refused for their content.
    const ScratchDir dir("proc-bat");
    for (const char* name : {"t.bat", "T.CMD", "t.batx", "t.cmdline", "t"}) {
        ASSERT_TRUE(frost::io::realFileSystem().writeFileAtomic(dir.join(name), "@echo INJECTED\r\n").has_value());
    }
    const std::string& d = dir.path();
    std::string slashed = d;
    std::ranges::replace(slashed, '\\', '/');
    for (const std::string& exe :
         {d + "\\t.bat", d + "\\T.CMD", d + "\\t.BAT", d + "\\t.bat.", d + "\\t.bat. ", d + "\\t.bat\\",
          slashed + "/t.bat/", d + "\\t.bat\\.", d + "\\t.bat\\x\\..", "\\\\.\\" + d + "\\t.bat\\.",
          "\\\\.\\" + d + "\\t.bat", "\\\\?\\" + d + "\\t.bat", "\\\\?\\" + d + "\\t.bat.", d + "\\T.CMD\\."}) {
        ProcessSpec spec;
        spec.executable = exe;
        const auto result = frost::io::realProcessLauncher().launch(spec);
        ASSERT_FALSE(result.has_value()) << exe;
        EXPECT_TRUE(result.error().code() == ErrorCode::Unsupported ||
                    result.error().code() == ErrorCode::InvalidArgument)
            << exe << ": " << result.error().describe();
    }
    // Batch content under other names never starts: CreateProcessW picks cmd.exe by the
    // .bat/.cmd extension only, and the content is no executable image (ERROR_BAD_EXE_FORMAT).
    // Stream syntax is refused by the path rules before anything is started.
    for (const std::string& exe : {d + "\\t.batx", d + "\\t.cmdline", d + "\\t", d + "\\t.bat::$DATA",
                                   d + "\\t.bat:", "\\\\?\\" + d + "\\t.bat::$DATA"}) {
        ProcessSpec spec;
        spec.executable = exe;
        const auto result = frost::io::realProcessLauncher().launch(spec);
        EXPECT_FALSE(result.has_value()) << exe << " started";
    }
}

TEST(RealProcessLauncher, PipesAreUnpredictableAndPrivate) {
    // The child inspects its own end of the stdout pipe (its write access includes READ_CONTROL).
    const auto result = run(childSpec({"pipe-info"}));
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    const std::string dacl = rawValue(result->stdoutBytes, "dacl:");
    const std::string name = rawValue(result->stdoutBytes, "name:");

    // This process's user SID, as SDDL writes it.
    HANDLE token = nullptr;
    ASSERT_NE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), FALSE);
    std::vector<DWORD> buffer(256);
    DWORD size = 0;
    const BOOL got =
        GetTokenInformation(token, TokenUser, buffer.data(), static_cast<DWORD>(buffer.size() * sizeof(DWORD)), &size);
    CloseHandle(token);
    ASSERT_NE(got, FALSE);
    wchar_t* sidText = nullptr;
    ASSERT_NE(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sidText), FALSE);
    const std::string sid = frost::io::fromWide(sidText).value();
    LocalFree(sidText);

    // A protected DACL ("P": nothing inherited) with exactly one ACE, an allow for this user.
    EXPECT_TRUE(dacl.starts_with("D:P(A;;")) << dacl;
    EXPECT_EQ(std::ranges::count(dacl, '('), 1) << dacl;
    EXPECT_TRUE(dacl.ends_with(";;;" + sid + ")")) << dacl << " (user " << sid << ")";

    // "\frost-io-<pid>-<32 random hex digits>".
    const std::string prefix = std::format("\\frost-io-{}-", GetCurrentProcessId());
    ASSERT_TRUE(name.starts_with(prefix)) << name;
    const std::string random = name.substr(prefix.size());
    EXPECT_EQ(random.size(), 32u) << name;
    EXPECT_TRUE(std::ranges::all_of(random, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        << name;
    // And different for the next pipe.
    const auto again = run(childSpec({"pipe-info"}));
    ASSERT_TRUE(again.has_value()) << again.error().describe();
    EXPECT_NE(rawValue(again->stdoutBytes, "name:"), name);
}
#endif

// Reads the child's "grandchild <pid>" line from its stdout pipe; 0 if it never comes.
frost::io::ProcessId readGrandchildPid(frost::io::IProcess& process) {
    std::string text;
    std::array<char, 256> buffer{};
    while (text.find('\n') == std::string::npos) {
        const auto got = process.readStdout(buffer);
        if (!got || *got == 0) {
            break;
        }
        text.append(buffer.data(), *got);
    }
    constexpr std::string_view prefix = "grandchild ";
    if (!text.starts_with(prefix)) {
        ADD_FAILURE() << "unexpected child output: " << text;
        return 0;
    }
    return static_cast<frost::io::ProcessId>(std::stoul(text.substr(prefix.size())));
}

TEST(RealProcessLauncher, TerminateEndsTheWholeTree) {
    auto process = frost::io::realProcessLauncher().launch(childSpec({"grandchild", "wait"}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const frost::io::ProcessId pid = readGrandchildPid(**process);
    ASSERT_NE(pid, 0u);
    const ProcessWatch grandchild(pid);
    ASSERT_TRUE(grandchild.valid());
    EXPECT_FALSE(grandchild.exitsWithin(0ms));
    ASSERT_TRUE((*process)->terminate().has_value());
    EXPECT_EQ((*process)->wait(30s).value().exitCode, frost::io::kTerminatedExitCode);
    EXPECT_TRUE(grandchild.exitsWithin(30s)) << "terminate() left the grandchild running";
    grandchild.kill();
}

TEST(RealProcessLauncher, DestroyingTheProcessEndsTheWholeTree) {
    // The child exits at once; only the containment (job object / process group) still
    // reaches the grandchild it left behind.
    auto process = frost::io::realProcessLauncher().launch(childSpec({"grandchild", "exit"}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const frost::io::ProcessId pid = readGrandchildPid(**process);
    ASSERT_NE(pid, 0u);
    const ProcessWatch grandchild(pid);
    ASSERT_TRUE(grandchild.valid());
    EXPECT_EQ((*process)->wait(30s).value().exitCode, 0);
    EXPECT_FALSE(grandchild.exitsWithin(200ms));
    process->reset();
    EXPECT_TRUE(grandchild.exitsWithin(30s)) << "the destructor left the grandchild running";
    grandchild.kill();
}

TEST(RealProcessLauncher, WithoutKillOnCloseTheChildOutlivesItsProcessObject) {
    ProcessSpec spec = childSpec({"sleep", "60000"});
    spec.killOnClose = false;
    auto process = frost::io::realProcessLauncher().launch(spec);
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const frost::io::ProcessId pid = (*process)->pid();
    const ProcessWatch child(pid);
    ASSERT_TRUE(child.valid());
    process->reset();
    EXPECT_FALSE(child.exitsWithin(500ms)) << "the destructor ended a child launched without killOnClose";
    child.kill();
    EXPECT_TRUE(child.exitsWithin(30s));
#ifndef _WIN32
    // Still this process's child: reap it.
    int status = 0;
    EXPECT_EQ(waitpid(static_cast<pid_t>(pid), &status, 0), static_cast<pid_t>(pid));
#endif
}

TEST(RealProcessLauncher, GrandchildHoldingStdoutDoesNotHangCommunicate) {
    // The child exits, but its grandchild keeps the stdout pipe open, so EOF never arrives:
    // communicate must give up at its deadline and end the tree.
    auto process = frost::io::realProcessLauncher().launch(childSpec({"grandchild", "exit"}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const frost::io::ProcessId pid = readGrandchildPid(**process);
    ASSERT_NE(pid, 0u);
    const ProcessWatch grandchild(pid);
    ASSERT_TRUE(grandchild.valid());
    const auto start = std::chrono::steady_clock::now();
    const auto result = (*process)->communicate({}, 2s);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Timeout) << result.error().describe();
    EXPECT_GE(elapsed, 1500ms);
    EXPECT_LT(elapsed, 30s);
    EXPECT_TRUE(grandchild.exitsWithin(30s)) << "the timeout left the grandchild running";
    grandchild.kill();
}

TEST(RealProcessLauncher, ContainedProcessesListsTheTreeAndEndsOneOfIt) {
    auto process = frost::io::realProcessLauncher().launch(childSpec({"grandchild", "exit"}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const frost::io::ProcessId pid = readGrandchildPid(**process);
    ASSERT_NE(pid, 0u);
    const ProcessWatch grandchild(pid);
    ASSERT_TRUE(grandchild.valid());
    EXPECT_EQ((*process)->wait(30s).value().exitCode, 0);

    auto listed = (*process)->containedProcesses();
    ASSERT_TRUE(listed.has_value()) << listed.error().describe();
    const auto entry =
        std::ranges::find_if(*listed, [pid](const frost::io::ContainedProcess& p) { return p.pid == pid; });
    ASSERT_NE(entry, listed->end()) << "the grandchild is not listed in its container";
    EXPECT_TRUE(entry->imageName.starts_with("io_test_child")) << entry->imageName;

    // A process outside the container (this test) is refused, not ended.
#ifdef _WIN32
    const auto self = static_cast<frost::io::ProcessId>(GetCurrentProcessId());
#else
    const auto self = static_cast<frost::io::ProcessId>(getpid());
#endif
    EXPECT_EQ((*process)->terminateContained(self).error().code(), ErrorCode::InvalidArgument);

    ASSERT_TRUE((*process)->terminateContained(pid).has_value());
    EXPECT_TRUE(grandchild.exitsWithin(30s)) << "terminateContained left the grandchild running";
    grandchild.kill();
}

TEST(RealProcessLauncher, AbandonedOutputEndsReadsAndAReleasedContainerOutlivesTheProcess) {
    auto process = frost::io::realProcessLauncher().launch(childSpec({"grandchild", "exit"}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const frost::io::ProcessId pid = readGrandchildPid(**process);
    ASSERT_NE(pid, 0u);
    const ProcessWatch grandchild(pid);
    ASSERT_TRUE(grandchild.valid());
    EXPECT_EQ((*process)->wait(30s).value().exitCode, 0);

    // The grandchild holds stdout open, so a read blocks until the output is abandoned.
    std::atomic<bool> readDone{false};
    frost::Result<std::size_t> got = std::size_t{99};
    std::thread reader([&] {
        std::array<char, 64> buffer{};
        got = (*process)->readStdout(buffer);
        readDone = true;
    });
    std::this_thread::sleep_for(200ms);
    EXPECT_FALSE(readDone.load()) << "the read returned while the grandchild still held stdout";
    (*process)->abandonOutput();
    reader.join();
    ASSERT_TRUE(got.has_value()) << got.error().describe();
    EXPECT_EQ(*got, 0u);
    std::array<char, 64> buffer{};
    EXPECT_EQ((*process)->readStderr(buffer).value(), 0u); // Later reads end at once too.

    ASSERT_TRUE((*process)->releaseContainer().has_value());
    process->reset();
    EXPECT_FALSE(grandchild.exitsWithin(500ms)) << "the released container still ended the grandchild";
    grandchild.kill();
    EXPECT_TRUE(grandchild.exitsWithin(30s));
}

TEST(RealProcessLauncher, ConcurrentLaunchesDoNotLeakPipeEnds) {
    // Pipe ends made for one launch must never reach a child started at the same time by
    // another thread. A leaked stdin or stdout write end in one of the long-lived sleepers
    // would hold back a quick child's EOF for 60 s, so its run would time out.
    constexpr int kThreads = 4;
    constexpr int kRounds = 4;
    frost::Mutex mutex;
    std::vector<std::unique_ptr<frost::io::IProcess>> sleepers;
    std::vector<std::string> failures;
    const auto start = std::chrono::steady_clock::now();
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int round = 0; round < kRounds; ++round) {
                    ProcessSpec sleeperSpec = childSpec({"sleep", "60000"});
                    sleeperSpec.stdinMode = Stdio::Pipe;
                    auto sleeper = frost::io::realProcessLauncher().launch(sleeperSpec);
                    ProcessSpec quick = childSpec({"tee"});
                    quick.stdinMode = Stdio::Pipe;
                    const std::string input = std::format("thread {} round {}\n", t, round);
                    const auto result = run(quick, input, 20s);
                    const frost::LockGuard lock(mutex);
                    if (!sleeper) {
                        failures.push_back("sleeper: " + sleeper.error().describe());
                    } else {
                        sleepers.push_back(std::move(*sleeper));
                    }
                    if (!result) {
                        failures.push_back("quick: " + result.error().describe());
                    } else if (result->stdoutBytes != input || result->stderrBytes != input) {
                        failures.push_back("quick: wrong output for " + input);
                    }
                }
            });
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_TRUE(failures.empty()) << failures.size()
                                  << " failures, first: " << (failures.empty() ? "" : failures.front());
    EXPECT_EQ(sleepers.size(), static_cast<std::size_t>(kThreads * kRounds));
    EXPECT_LT(elapsed, 50s);
    sleepers.clear(); // killOnClose ends them.
}

TEST(RealProcessLauncher, MaximalTimeoutsMeanNoDeadline) {
    // now() + Duration::max() would overflow; the deadline saturates instead.
    const auto result = run(childSpec({"exit", "3"}), {}, frost::IClock::Duration::max());
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 3);
    auto process = frost::io::realProcessLauncher().launch(childSpec({"exit", "4"}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    EXPECT_EQ((*process)->wait(frost::IClock::Duration::max()).value().exitCode, 4);
}

// Lowers detail::maxWaitSliceMs() for one scope and restores the previous value.
class ShortWaitSlices {
public:
    explicit ShortWaitSlices(std::int64_t milliseconds) : m_saved(frost::io::detail::maxWaitSliceMs()) {
        frost::io::detail::setMaxWaitSliceMsForTesting(milliseconds);
    }
    ~ShortWaitSlices() { frost::io::detail::setMaxWaitSliceMsForTesting(m_saved); }
    ShortWaitSlices(const ShortWaitSlices&) = delete ("restores the slice once");
    ShortWaitSlices& operator=(const ShortWaitSlices&) = delete ("restores the slice once");

private:
    std::int64_t m_saved;
};

TEST(RealProcessLauncher, FarDeadlinesOutlastManyWaitSlices) {
    // A single poll() or Wait* call blocks for one slice at most (2^30 ms, about 12 days, by
    // default); a slice that ends must not count as the deadline. With 20 ms slices a child that
    // sleeps 300 ms spans many of them.
    const ShortWaitSlices slices(20);
    const auto result = run(childSpec({"sleep", "300"}), {}, frost::IClock::Duration::max());
    ASSERT_TRUE(result.has_value()) << result.error().describe();
    EXPECT_EQ(result->exitCode, 0);
    auto process = frost::io::realProcessLauncher().launch(childSpec({"sleep", "300"}));
    ASSERT_TRUE(process.has_value()) << process.error().describe();
    const auto status = (*process)->wait(frost::IClock::Duration::max());
    ASSERT_TRUE(status.has_value()) << status.error().describe();
    EXPECT_EQ(status->exitCode, 0);
    // A near deadline still ends both kinds of wait.
    auto sleeper = frost::io::realProcessLauncher().launch(childSpec({"sleep", "60000"}));
    ASSERT_TRUE(sleeper.has_value()) << sleeper.error().describe();
    EXPECT_EQ((*sleeper)->wait(100ms).error().code(), ErrorCode::Timeout);
    const auto timedOut = run(childSpec({"sleep", "60000"}), {}, 100ms);
    ASSERT_FALSE(timedOut.has_value());
    EXPECT_EQ(timedOut.error().code(), ErrorCode::Timeout) << timedOut.error().describe();
}

#ifndef _WIN32
// Runs `body` with descriptor 0 replaced by `fd` (closed when `fd` is negative), then restores it.
template<class Body>
void withStdin(int fd, Body body) {
    const int saved = fcntl(0, F_DUPFD_CLOEXEC, 3);
    if (fd < 0) {
        close(0);
    } else {
        ASSERT_EQ(dup2(fd, 0), 0);
    }
    body();
    if (saved >= 0) {
        dup2(saved, 0);
        close(saved);
    } else {
        close(0);
    }
}

std::string identityOf(const char* path) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    EXPECT_GE(fd, 0) << path;
    std::string identity = fileIdentity(fd);
    close(fd);
    return identity;
}

TEST(RealProcessLauncher, InheritedStdinFallsBackToTheNullDevice) {
    const std::string nullIdentity = identityOf("/dev/null");
    ProcessSpec spec = childSpec({"probe-handle", "stdin", nullIdentity});
    spec.stdinMode = Stdio::Inherit;

    // A closed descriptor 0: the child still gets a valid stdin. (The launcher's own pipes
    // must not land on descriptor 0 meanwhile.)
    std::optional<frost::Result<ProcessResult>> closed;
    withStdin(-1, [&] { closed = run(spec); });
    ASSERT_TRUE(closed->has_value()) << closed->error().describe();
    EXPECT_EQ((*closed)->stdoutBytes, "inherited\n");

    // The same while an earlier child is alive: its pidfd (close-on-exec) took the free
    // slot 0, which must not count as a stdin a child could inherit.
    std::optional<frost::Result<ProcessResult>> second;
    bool slotTaken = false;
    withStdin(-1, [&] {
        auto first = frost::io::realProcessLauncher().launch(childSpec({"sleep", "60000"}));
        ASSERT_TRUE(first.has_value()) << first.error().describe();
        const int flags = fcntl(0, F_GETFD);
        slotTaken = flags >= 0 && (flags & FD_CLOEXEC) != 0;
        second = run(spec);
        first->reset(); // Before descriptor 0 is restored: the pidfd may be descriptor 0.
    });
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(second->has_value()) << second->error().describe();
    EXPECT_EQ((*second)->stdoutBytes, "inherited\n") << "slot 0 held a close-on-exec descriptor: " << slotTaken;
    // The same case made deterministic (without pidfd support slot 0 would stay free above).
    std::optional<frost::Result<ProcessResult>> occupied;
    withStdin(-1, [&] {
        const int occupant = open("/dev/zero", O_RDONLY | O_CLOEXEC);
        ASSERT_EQ(occupant, 0);
        occupied = run(spec);
        close(occupant);
    });
    ASSERT_TRUE(occupied.has_value());
    ASSERT_TRUE(occupied->has_value()) << occupied->error().describe();
    EXPECT_EQ((*occupied)->stdoutBytes, "inherited\n");

    // A terminal stdin reaches only an uncontained child, which shares this process's job
    // control. A contained child leads its own session, outside that job control, so it
    // could read the terminal while the shell is in the foreground; it gets the null device.
    const int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    ASSERT_GE(master, 0);
    ASSERT_EQ(grantpt(master), 0);
    ASSERT_EQ(unlockpt(master), 0);
    const int terminal = open(ptsname(master), O_RDWR | O_NOCTTY | O_CLOEXEC);
    ASSERT_GE(terminal, 0);
    ASSERT_EQ(isatty(terminal), 1);
    ProcessSpec onTerminal = childSpec({"probe-handle", "stdin", fileIdentity(terminal)});
    onTerminal.stdinMode = Stdio::Inherit;
    ProcessSpec onNull = spec; // Probes for /dev/null, also with an inherited stdin.
    std::optional<frost::Result<ProcessResult>> containedTerminal;
    std::optional<frost::Result<ProcessResult>> containedNull;
    withStdin(terminal, [&] {
        containedTerminal = run(onTerminal);
        containedNull = run(onNull);
    });
    onTerminal.killOnClose = false;
    std::optional<frost::Result<ProcessResult>> shared;
    withStdin(terminal, [&] { shared = run(onTerminal); });
    close(terminal);
    close(master);
    ASSERT_TRUE(containedTerminal->has_value()) << containedTerminal->error().describe();
    EXPECT_EQ((*containedTerminal)->stdoutBytes, "not inherited\n");
    ASSERT_TRUE(containedNull->has_value()) << containedNull->error().describe();
    EXPECT_EQ((*containedNull)->stdoutBytes, "inherited\n");
    ASSERT_TRUE(shared->has_value()) << shared->error().describe();
    EXPECT_EQ((*shared)->stdoutBytes, "inherited\n");
}

TEST(RealProcessLauncher, ContainedChildrenLeadASessionWithoutATerminal) {
    // killOnClose: a new session, so /dev/tty cannot be opened (ENXIO) and a prompt for a
    // passphrase fails fast instead of stopping the child with SIGTTIN, whatever terminal
    // this test runs under. (Whether this process has a terminal depends on how ctest was
    // started, so the uncontained case checks only the session.)
    const auto contained = run(childSpec({"session"}));
    ASSERT_TRUE(contained.has_value()) << contained.error().describe();
    EXPECT_EQ(contained->stdoutBytes, "session leader\nno tty\n");

    ProcessSpec shared = childSpec({"session"});
    shared.killOnClose = false;
    const auto uncontained = run(shared);
    ASSERT_TRUE(uncontained.has_value()) << uncontained.error().describe();
    EXPECT_TRUE(uncontained->stdoutBytes.starts_with("not session leader\n")) << uncontained->stdoutBytes;
}
#endif

} // namespace
