// SPDX-License-Identifier: Apache-2.0
//
// Integration test of the MSVC environment capture (cmake_toolchain.hpp, ADR 0046): vswhere finds
// the installed Visual Studio, vcvarsall.bat x64 runs through the absolute cmd.exe, and the
// difference holds the compiler's INCLUDE, LIB, VCToolsInstallDir and a PATH that reaches cl.exe;
// MsvcEnvironmentCache captures once and later reads its file. Windows only; label "integration"
// with the tag "msvc".

#include "frost/build/cmake_toolchain.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/path.hpp"
#include "frost/io/process.hpp"

#include "support/scratch_dir.hpp"
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace frost::build {
namespace {

std::optional<std::string> valueOf(const project::CMakeEnvironment& environment, std::string_view name) {
    for (const auto& [key, value] : environment) {
        if (key.size() == name.size() && std::ranges::equal(key, name, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            })) {
            return value;
        }
    }
    return std::nullopt;
}

TEST(MsvcEnvironmentCapture, RunsVcvarsallAndCaches) {
    auto& fs = io::realFileSystem();
    auto& launcher = io::realProcessLauncher();
    auto instances = listVsInstances(launcher, fs);
    ASSERT_TRUE(instances.has_value()) << instances.error().describe();
    const auto withTools = std::ranges::find_if(*instances, [&](const VsInstance& instance) {
        auto vcvarsall = vcvarsallPath(instance.installationPath);
        return vcvarsall && fs.exists(*vcvarsall).value_or(false);
    });
    ASSERT_NE(withTools, instances->end()) << "no Visual Studio instance with the C++ tools";

    auto captured = captureMsvcEnvironment(launcher, fs, *withTools, "x64");
    ASSERT_TRUE(captured.has_value()) << captured.error().describe();
    EXPECT_EQ(captured->instanceId, withTools->instanceId);
    const auto include = valueOf(captured->changes, "INCLUDE");
    const auto lib = valueOf(captured->changes, "LIB");
    const auto tools = valueOf(captured->changes, "VCToolsInstallDir");
    const auto path = valueOf(captured->changes, "PATH");
    ASSERT_TRUE(include.has_value() && lib.has_value() && tools.has_value() && path.has_value());
    EXPECT_NE(include->find("include"), std::string::npos);
    EXPECT_TRUE(io::isAbsolutePath(*tools));
    // cl.exe for x64 lies on the new PATH.
    bool foundCl = false;
    for (const std::string& directory : splitPathVariable(*path)) {
        if (auto cl = io::joinPath(directory, "cl.exe"); cl && fs.exists(*cl).value_or(false)) {
            foundCl = true;
            break;
        }
    }
    EXPECT_TRUE(foundCl) << *path;

    frost::test::ScratchDir scratch("msvc-env");
    {
        MsvcEnvironmentCache cache(fs, launcher, scratch.path());
        auto first = cache.get(*withTools, "x64");
        ASSERT_TRUE(first.has_value()) << first.error().describe();
        ASSERT_TRUE(cache.get(*withTools, "x64").has_value());
        EXPECT_EQ(cache.captures(), 1U);
        EXPECT_TRUE(fs.exists(cache.cacheFile(withTools->instanceId, "x64")).value_or(false));
    }
    MsvcEnvironmentCache again(fs, launcher, scratch.path());
    auto fromFile = again.get(*withTools, "x64");
    ASSERT_TRUE(fromFile.has_value()) << fromFile.error().describe();
    EXPECT_EQ(again.captures(), 0U); // The file is current for this Visual Studio version.
    EXPECT_EQ(valueOf(fromFile->changes, "INCLUDE"), include);
}

} // namespace
} // namespace frost::build
