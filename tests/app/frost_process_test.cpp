// SPDX-License-Identifier: Apache-2.0
//
// Integration tests that run the real frost executable (PLAN.md section 7,
// M0 exit): --version, --help, error exit codes, the built-in self-test, and
// launching from a directory whose path has non-ASCII characters and spaces,
// and from one longer than MAX_PATH (260 characters) on Windows. Processes are
// started through frost::io's production IProcessLauncher.

#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
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
    int exitCode = -1;      // The child's exit status; -1 if it could not be run.
    std::string output;     // stdout and stderr, interleaved as the child wrote them.
    std::string startError; // Why it could not be run, when exitCode == -1.
};

// Runs `exe` with `args` and collects its combined output (stderr merged into stdout).
FrostRun runFrost(const std::string& exe, std::vector<std::string> args) {
    frost::io::ProcessSpec spec;
    spec.executable = exe;
    spec.arguments = std::move(args);
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

// Copies frost and the runtime files it needs (DLLs and Qt platform plugins on
// Windows) from the build directory into `dir`, creating it. Returns the copied
// executable's path.
std::string copyFrostTo(const std::string& dir) {
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
    fs::copy(buildDir / "platforms", target / "platforms",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, error);
    EXPECT_FALSE(error) << "copy platforms: " << error.message();
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
#endif
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

// Reads HKLM\SYSTEM\CurrentControlSet\Control\FileSystem\LongPathsEnabled.
bool systemLongPathsEnabled() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                                        L"LongPathsEnabled", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status == ERROR_SUCCESS && value != 0;
}

TEST(FrostProcess, RunsFromPathLongerThanMaxPath) {
    /*
     * frost::io creates the tree and starts the process through \\?\ paths, which work
     * whatever LongPathsEnabled says (io_integration_tests launches a helper from such a
     * directory), and the loader does find frost's DLLs there. Qt, though, loads its
     * platform plugin through ordinary Win32 paths, which the frost manifest's
     * longPathAware setting extends beyond MAX_PATH only when the system-wide switch is
     * also on. Observed with the switch off (2026-10-31): Qt reports "no Qt platform
     * plugin could be initialized" and aborts, and the Debug CRT's abort dialog keeps the
     * child alive until the timeout. So the test needs the switch.
     */
    if (!systemLongPathsEnabled()) {
        GTEST_SKIP() << "LongPathsEnabled=0 on this machine: Qt cannot load its platform plugin from a directory "
                        "beyond MAX_PATH; enabling the switch needs administrator rights";
    }
    const ScratchDir scratch("app-process-long");
    std::string dir = scratch.path();
    while (dir.size() < 300) {
        dir = frost::io::joinPath(dir, std::string(40, 'x') + " segment").value();
    }
    ASSERT_GT(dir.size(), 260u);
    const std::string exe = copyFrostTo(dir);
    const auto result = runFrost(exe, {"--selftest"});
    ASSERT_EQ(result.exitCode, 0) << result.startError << result.output;
    EXPECT_NE(result.output.find("selftest: PASSED"), std::string::npos) << result.output;
}

#endif

} // namespace
