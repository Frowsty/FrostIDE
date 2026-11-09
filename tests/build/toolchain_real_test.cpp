// SPDX-License-Identifier: Apache-2.0
//
// Integration tests of the toolchain discovery core (toolchain.hpp, SLN-14) against the real tools of
// this machine: vswhere.exe, reg.exe, vcvarsall.bat, cl.exe and the pinned cmake. Every expected
// value comes from one of those tools, not from FrostIDE's own reading:
//   - the instances equal what vswhere lists itself (-property instanceId);
//   - the default toolset is the VCToolsVersion vcvarsall sets up without -vcvars_ver, each listed
//     toolset is the VCToolsVersion vcvarsall sets up with it, and its cl.exe reports the matching
//     compiler version (cl 19.NN for toolset 14.NN);
//   - the Windows Kits root and the SDK versions are the ones reg.exe reads from Installed Roots, and
//     the newest usable SDK is the WindowsSDKVersion vcvarsall picks by default;
//   - a toolset that is not installed is refused by resolveToolchain and by vcvarsall itself;
//   - a CMake configure with a chosen toolset and SDK records that toolset's cl.exe in CMakeCache.txt.
// Windows only (target build_toolchain_tests), labels "integration" and "msvc". Without Visual Studio
// (no vswhere.exe) the tests are skipped and reported as skipped.

#include "frost/build/cmake_service.hpp"
#include "frost/build/toolchain.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"
#include "frost/project/trust_store.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace frost::build {
namespace {

using namespace std::chrono_literals;

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; });
    return out;
}

// Paths compared as Windows does: separators, repeated separators (vcvarsall writes
// "10\\include") and ASCII case ignored.
std::string pathKey(std::string_view path) {
    std::string out = lowered(path);
    std::ranges::replace(out, '/', '\\');
    for (std::size_t i = out.find("\\\\", 1); i != std::string::npos; i = out.find("\\\\", i)) {
        out.erase(i, 1);
    }
    while (out.size() > 3 && out.back() == '\\') {
        out.pop_back();
    }
    return out;
}

std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string line(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            out.push_back(std::move(line));
        }
    }
    return out;
}

// Runs `program` (absolute) with `arguments`; fails the test on a launch error.
io::ProcessResult run(const std::string& program, std::vector<std::string> arguments) {
    io::ProcessSpec spec;
    spec.executable = program;
    spec.arguments = std::move(arguments);
    spec.stdinMode = io::Stdio::Null;
    auto result = io::runToCompletion(io::realProcessLauncher(), spec, {}, 120s);
    EXPECT_TRUE(result.has_value()) << program << ": " << (result ? std::string() : result.error().describe());
    return result.value_or(io::ProcessResult{});
}

std::string systemTool(std::string_view name) {
    auto cmd = systemCmdPath();
    EXPECT_TRUE(cmd.has_value());
    return io::joinPath(io::parentPath(cmd.value_or("C:\\Windows\\System32\\cmd.exe")), name).value_or("");
}

// The rig: the inventory, and skipping without Visual Studio.
class RealToolchain : public ::testing::Test {
protected:
    void SetUp() override {
        auto vswhere = defaultVswherePath();
        if (!vswhere || !io::realFileSystem().exists(*vswhere).value_or(false)) {
            GTEST_SKIP() << "no Visual Studio is installed (no vswhere.exe)";
        }
        m_vswhere = *vswhere;
        m_inventory = discoverToolchains(io::realProcessLauncher(), io::realFileSystem());
        for (const std::string& line : describeToolchains(m_inventory)) {
            std::cout << "[toolchains] " << line << '\n';
        }
        m_cpp = m_inventory.firstWithCppTools();
        if (m_cpp == nullptr) {
            GTEST_SKIP() << "no Visual Studio instance has the C++ build tools: "
                         << (m_inventory.visualStudioError ? m_inventory.visualStudioError->message() : "");
        }
    }

    std::string m_vswhere;
    ToolchainInventory m_inventory;
    const VsToolchain* m_cpp = nullptr;
};

TEST_F(RealToolchain, TheInstancesAreTheOnesVswhereLists) {
    // Oracle: vswhere's own -property listing with the locator's filter.
    std::vector<std::string> arguments = vswhereArguments();
    const auto format = std::ranges::find(arguments, "-format");
    ASSERT_NE(format, arguments.end());
    arguments.erase(format, format + 2); // "-format json": -property prints one value per line instead.
    arguments.insert(arguments.end(), {"-property", "instanceId"});
    const io::ProcessResult listed = run(m_vswhere, arguments);
    ASSERT_EQ(listed.exitCode, 0) << listed.stderrBytes;
    std::vector<std::string> discovered;
    for (const VsToolchain& entry : m_inventory.visualStudio) {
        discovered.push_back(entry.instance.instanceId);
        EXPECT_FALSE(entry.instance.msbuildPath.empty()) << entry.instance.displayName << " has no MSBuild.exe";
    }
    EXPECT_EQ(discovered, lines(listed.stdoutBytes));
}

TEST_F(RealToolchain, EachToolsetIsTheOneVcvarsallSetsUpAndItsCompilerSaysSo) {
    auto& fs = io::realFileSystem();
    auto& launcher = io::realProcessLauncher();
    // The default: vcvarsall without -vcvars_ver.
    auto defaults = captureMsvcEnvironment(launcher, fs, m_cpp->instance, "x64");
    ASSERT_TRUE(defaults.has_value()) << defaults.error().describe();
    EXPECT_EQ(defaults->toolsetVersion, m_cpp->defaultToolset);
    const auto defaultEntry = std::ranges::find_if(m_cpp->toolsets, &MsvcToolset::isDefault);
    ASSERT_NE(defaultEntry, m_cpp->toolsets.end());
    EXPECT_EQ(defaultEntry->version, defaults->toolsetVersion);

    // Every toolset (at most three, newest first, to bound the run time): vcvarsall sets it up, and
    // its cl.exe is the compiler of that toolset (cl 19.NN belongs to toolset 14.NN).
    const std::regex banner(R"(Version (\d+)\.(\d+)\.)");
    const std::size_t count = std::min<std::size_t>(m_cpp->toolsets.size(), 3);
    for (std::size_t i = 0; i < count; ++i) {
        const MsvcToolset& toolset = m_cpp->toolsets[i];
        SCOPED_TRACE(toolset.version);
        auto captured = captureMsvcEnvironment(launcher, fs, m_cpp->instance, "x64",
                                               {.toolsetVersion = toolset.version, .sdkVersion = {}});
        ASSERT_TRUE(captured.has_value()) << captured.error().describe();
        EXPECT_EQ(captured->toolsetVersion, toolset.version);
        const auto toolsDir = environmentValue(captured->changes, "VCToolsInstallDir");
        ASSERT_TRUE(toolsDir.has_value());
        EXPECT_EQ(pathKey(*toolsDir), pathKey(toolset.directory));

        const io::ProcessResult cl = run(toolset.compiler, {});
        std::smatch match;
        const std::string text = cl.stderrBytes + cl.stdoutBytes;
        ASSERT_TRUE(std::regex_search(text, match, banner)) << text;
        EXPECT_EQ(match[1].str(), "19");
        const std::string minor = toolset.version.substr(3, toolset.version.find('.', 3) - 3);
        EXPECT_EQ(match[2].str(), minor) << "cl.exe of toolset " << toolset.version << ": " << text;
    }
}

TEST_F(RealToolchain, TheWindowsSdksAreTheRegisteredOnesAndVcvarsallPicksTheNewest) {
    // Oracle: reg.exe over the 32-bit view of Installed Roots: KitsRoot10 and one subkey per SDK.
    const io::ProcessResult reg =
        run(systemTool("reg.exe"), {"query", "HKLM\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots", "/reg:32"});
    ASSERT_EQ(reg.exitCode, 0) << reg.stderrBytes;
    std::string kitsRoot;
    std::set<std::string> registered;
    const std::regex value(R"(^\s+KitsRoot10\s+REG_SZ\s+(.+)$)");
    const std::regex subkey(R"(\\Installed Roots\\(10\.[0-9.]+)$)");
    for (const std::string& line : lines(reg.stdoutBytes)) {
        std::smatch match;
        if (std::regex_match(line, match, value)) {
            kitsRoot = match[1].str();
        } else if (std::regex_search(line, match, subkey)) {
            registered.insert(match[1].str());
        }
    }
    ASSERT_FALSE(kitsRoot.empty()) << reg.stdoutBytes;
    ASSERT_FALSE(m_inventory.windowsSdkError.has_value()) << m_inventory.windowsSdkError->describe();
    EXPECT_EQ(pathKey(m_inventory.windowsKitsRoot), pathKey(kitsRoot));
    std::vector<std::string> usable;
    for (const WindowsSdk& sdk : m_inventory.windowsSdks) {
        if (sdk.usable()) {
            usable.push_back(sdk.version);
            EXPECT_TRUE(registered.contains(sdk.version)) << sdk.version << " is not registered";
        }
    }
    ASSERT_FALSE(usable.empty());

    // vcvarsall's default SDK is the newest usable one, and naming it explicitly sets up the same.
    auto& fs = io::realFileSystem();
    auto& launcher = io::realProcessLauncher();
    auto defaults = captureMsvcEnvironment(launcher, fs, m_cpp->instance, "x64");
    ASSERT_TRUE(defaults.has_value()) << defaults.error().describe();
    EXPECT_EQ(defaults->sdkVersion, usable.front());
    auto chosen = resolveToolchain(m_inventory, {.instanceId = m_cpp->instance.instanceId,
                                                 .toolsetVersion = m_cpp->toolsets.front().version,
                                                 .sdkVersion = usable.back(),
                                                 .architecture = "x64"});
    ASSERT_TRUE(chosen.has_value()) << chosen.error().describe();
    auto captured =
        captureMsvcEnvironment(launcher, fs, chosen->toolchain.instance, chosen->architecture, chosen->selection);
    ASSERT_TRUE(captured.has_value()) << captured.error().describe();
    EXPECT_EQ(captured->sdkVersion, usable.back());
    EXPECT_EQ(captured->toolsetVersion, m_cpp->toolsets.front().version);
    const auto include = environmentValue(captured->changes, "INCLUDE");
    ASSERT_TRUE(include.has_value());
    EXPECT_NE(pathKey(*include).find(pathKey("\\Include\\" + usable.back() + "\\um")), std::string::npos) << *include;
}

TEST_F(RealToolchain, AToolsetThatIsNotInstalledIsRefusedBeforeAndByVcvarsall) {
    // 14.1 (Visual Studio 2017's toolsets) unless this machine has one; then 14.9, which no release has.
    std::string missing = "14.1";
    if (std::ranges::any_of(m_cpp->toolsets, [](const MsvcToolset& t) { return t.version.starts_with("14.1"); })) {
        missing = "14.9";
    }
    auto resolved = resolveToolchain(m_inventory, {.toolsetVersion = missing});
    ASSERT_FALSE(resolved.has_value());
    EXPECT_EQ(resolved.error().code(), ErrorCode::NotFound);
    EXPECT_NE(resolved.error().message().find(m_cpp->toolsets.front().version), std::string::npos)
        << resolved.error().message();
    // vcvarsall itself refuses it (exit code 1), so no other toolset's environment is ever used.
    auto captured = captureMsvcEnvironment(io::realProcessLauncher(), io::realFileSystem(), m_cpp->instance, "x64",
                                           {.toolsetVersion = missing, .sdkVersion = {}});
    ASSERT_FALSE(captured.has_value());
    EXPECT_EQ(captured.error().code(), ErrorCode::IoError);
    EXPECT_NE(captured.error().message().find("failed with exit code"), std::string::npos)
        << captured.error().message();
}

// The CMake path: a configure with a chosen toolset and SDK finds that toolset's cl.exe.
TEST_F(RealToolchain, ACMakeConfigureUsesTheChosenToolsetsCompiler) {
    const MsvcToolset& toolset = m_cpp->toolsets.front();
    const auto sdk = std::ranges::find_if(m_inventory.windowsSdks, &WindowsSdk::usable);
    ASSERT_NE(sdk, m_inventory.windowsSdks.end());

    frost::test::ScratchDir scratch("toolchain-cmake");
    auto& fs = io::realFileSystem();
    const std::string folder = scratch.join("hello");
    ASSERT_TRUE(fs.createDirectories(folder).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(io::joinPath(folder, "CMakeLists.txt").value(),
                                   "cmake_minimum_required(VERSION 3.21)\nproject(Hello C)\n"
                                   "add_executable(hello hello.c)\n")
                    .has_value());
    ASSERT_TRUE(
        fs.writeFileAtomic(io::joinPath(folder, "hello.c").value(), "int main(void) { return 0; }\n").has_value());
    auto trust = project::TrustStore::open(fs, scratch.join("trusted-roots.txt"));
    ASSERT_TRUE(trust.has_value()) << trust.error().describe();
    ASSERT_TRUE((*trust)->trust(folder).has_value());

    CMakeServiceOptions options;
    options.cmakePath = io::normalizePath(FROST_TEST_CMAKE).value_or("");
    options.ninjaPath = io::normalizePath(FROST_TEST_NINJA).value_or("");
    options.msvcCacheDirectory = scratch.join("msvc");
    options.vsInstanceId = m_cpp->instance.instanceId;
    options.msvcToolsetVersion = toolset.version;
    options.windowsSdkVersion = sdk->version;
    // "cl": CMake finds the compiler on the PATH of the captured environment.
    options.profileRecords = {
        project::CMakeProfileRecord{.name = "Debug", .cacheVariables = {std::string("CMAKE_C_COMPILER:STRING=cl")}}};
    std::vector<std::string> notes;
    CMakeServiceCallbacks callbacks;
    callbacks.onConfigureOutput = [&notes](BuildOutputKind kind, std::string_view line) {
        std::cout << "[configure] " << line << '\n';
        if (kind == BuildOutputKind::Status) {
            notes.emplace_back(line);
        }
    };
    {
        auto service = CMakeService::create(fs, io::realProcessLauncher(), **trust, options, callbacks);
        ASSERT_TRUE(service->openFolder(folder).has_value());
        ASSERT_TRUE(service->waitForIdle(240s));
        ASSERT_TRUE(service->lastConfigure().has_value());
        ASSERT_TRUE(service->lastConfigure()->succeeded())
            << (service->lastConfigure()->error ? service->lastConfigure()->error->describe() : "cmake failed");
        // Oracle: CMake's own cache names the compiler it found.
        auto cache = fs.readFile(io::joinPath(service->activeBuildDirectory(), "CMakeCache.txt").value());
        ASSERT_TRUE(cache.has_value()) << cache.error().describe();
        std::smatch match;
        const std::regex compiler(R"(CMAKE_C_COMPILER:[A-Z]+=(.+))");
        std::string compilerPath;
        for (const std::string& line : lines(*cache)) {
            if (std::regex_match(line, match, compiler)) {
                compilerPath = match[1].str();
            }
        }
        EXPECT_EQ(pathKey(compilerPath), pathKey(toolset.compiler)) << "CMakeCache.txt: " << compilerPath;
    }
    const auto note = std::ranges::find_if(notes, [&](const std::string& line) {
        return line.find("MSVC toolset " + toolset.version) != std::string::npos &&
               line.find("Windows SDK " + sdk->version) != std::string::npos;
    });
    EXPECT_NE(note, notes.end()) << "the configure does not say which toolset and SDK it set up";
}

} // namespace
} // namespace frost::build
