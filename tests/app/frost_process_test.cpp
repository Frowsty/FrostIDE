// SPDX-License-Identifier: Apache-2.0
//
// Integration tests that run the real frost executable (PLAN.md section 7,
// M0 exit): --version, --help, error exit codes, the built-in self-test, and
// launching from a directory whose path has non-ASCII characters and spaces,
// and from one longer than MAX_PATH (260 characters) on Windows.

#include "support/process_runner.hpp"
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

const fs::path kFrost = fs::path(FROST_EXE_PATH);

// Copies frost and the runtime files it needs (DLLs and Qt platform plugins on
// Windows) from the build directory into `dir`. Returns the copied executable.
fs::path copyFrostTo(const fs::path& dir) {
    fs::create_directories(dir);
    const fs::path buildDir = kFrost.parent_path();
    fs::copy_file(kFrost, dir / kFrost.filename(), fs::copy_options::overwrite_existing);
#ifdef _WIN32
    for (const auto& entry : fs::directory_iterator(buildDir)) {
        if (entry.path().extension() == L".dll") {
            fs::copy_file(entry.path(), dir / entry.path().filename(), fs::copy_options::overwrite_existing);
        }
    }
    fs::copy(buildDir / "platforms", dir / "platforms",
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
#endif
    return dir / kFrost.filename();
}

// A scratch directory under the system temp directory, removed on destruction.
class ScratchDir {
public:
    explicit ScratchDir(const fs::path& leaf) : m_root(fs::temp_directory_path() / "frost-process-tests") {
        m_path = m_root / leaf;
        std::error_code ignored;
        fs::remove_all(m_path, ignored);
    }
    ~ScratchDir() {
        std::error_code ignored;
        fs::remove_all(m_root, ignored);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    [[nodiscard]] const fs::path& path() const { return m_path; }

private:
    fs::path m_root;
    fs::path m_path;
};

TEST(FrostProcess, VersionPrintsProductCompilerAndQt) {
    const auto result = frost::test::runProcess(kFrost, {"--version"});
    ASSERT_EQ(result.exitCode, 0) << result.startError << result.output;
    EXPECT_NE(result.output.find("FrostIDE 0.0.1"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("Clang"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("Qt 6."), std::string::npos) << result.output;
}

TEST(FrostProcess, HelpPrintsUsage) {
    const auto result = frost::test::runProcess(kFrost, {"--help"});
    ASSERT_EQ(result.exitCode, 0) << result.output;
    EXPECT_NE(result.output.find("Usage: frost"), std::string::npos) << result.output;
}

TEST(FrostProcess, UnknownOptionExitsWithStatus2) {
    const auto result = frost::test::runProcess(kFrost, {"--no-such-option"});
    EXPECT_EQ(result.exitCode, 2) << result.output;
    EXPECT_NE(result.output.find("unknown option --no-such-option"), std::string::npos) << result.output;
}

TEST(FrostProcess, SelfTestPasses) {
    const auto result = frost::test::runProcess(kFrost, {"--selftest"});
    ASSERT_EQ(result.exitCode, 0) << result.output;
    EXPECT_NE(result.output.find("selftest: PASSED"), std::string::npos) << result.output;
#ifdef _WIN32
    // The manifest's effects are visible only inside the real executable.
    EXPECT_NE(result.output.find("ok   utf8-code-page"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("ok   long-path-manifest"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("ok   dpi-awareness"), std::string::npos) << result.output;
#endif
}

TEST(FrostProcess, RunsFromNonAsciiPathWithSpaces) {
    // UTF-8 literal: Latin accents, CJK and spaces in one directory name.
    const ScratchDir scratch(fs::path(u8"frost ünïcødé 测试 dir"));
    const fs::path exe = copyFrostTo(scratch.path());
    const auto result = frost::test::runProcess(exe, {"--selftest"});
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
    if (!systemLongPathsEnabled()) {
        GTEST_SKIP() << "LongPathsEnabled=0 on this machine; enabling it needs administrator rights";
    }
    fs::path leaf;
    while (leaf.native().size() < 300) {
        leaf /= std::wstring(40, L'x') + L" segment";
    }
    const ScratchDir scratch(leaf);
    // The test executable has no long-path manifest, so it creates the tree through \\?\ paths.
    const fs::path exe = copyFrostTo(fs::path(L"\\\\?\\" + fs::absolute(scratch.path()).wstring()));
    ASSERT_GT(scratch.path().native().size(), 260u);
    const auto result = frost::test::runProcess(exe, {"--selftest"});
    ASSERT_EQ(result.exitCode, 0) << result.startError << result.output;
}

#endif

} // namespace
