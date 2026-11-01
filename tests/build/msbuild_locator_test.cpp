// SPDX-License-Identifier: Apache-2.0
//
// Tests for MSBuild discovery (frost/build/msbuild_locator.hpp): vswhere's JSON, the MSBuild.exe
// path, instance selection, and on Windows the discovery flow against the scripted launcher and
// MemFs. The real vswhere is exercised by msbuild_build_test.cpp.

#include "frost/build/msbuild_locator.hpp"
#include "frost/core/clock.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/scripted_process_launcher.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using frost::ErrorCode;
using frost::build::VsInstance;

// Two instances as vswhere -sort prints them (trimmed): a preview first, then Build Tools whose path
// holds an escaped non-ASCII character.
constexpr std::string_view kTwoInstances = R"json([
  {
    "instanceId": "aa11",
    "installationPath": "C:\\VS\\Preview",
    "installationVersion": "18.0.11010.1",
    "displayName": "Visual Studio Community 2026 Preview",
    "productId": "Microsoft.VisualStudio.Product.Community",
    "isPrerelease": true,
    "catalog": { "productDisplayVersion": "18.0.0 Preview 1", "nested": [1, {"x": null}] }
  },
  {
    "instanceId": "39ecdf9a",
    "installationPath": "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\Build T\u00f6ols",
    "installationVersion": "17.14.37710.0",
    "displayName": "Visual Studio Build Tools 2022",
    "productId": "Microsoft.VisualStudio.Product.BuildTools",
    "isPrerelease": false,
    "catalog": { "productDisplayVersion": "17.14.41 (September 2026)" },
    "properties": { "nickname": "" }
  }
]
)json";

TEST(VswhereJson, ReadsEveryInstanceInOrder) {
    auto instances = frost::build::parseVswhereJson(kTwoInstances);
    ASSERT_TRUE(instances.has_value()) << instances.error().describe();
    ASSERT_EQ(instances->size(), 2U);
    const VsInstance& preview = (*instances)[0];
    EXPECT_EQ(preview.instanceId, "aa11");
    EXPECT_TRUE(preview.isPrerelease);
    EXPECT_EQ(preview.productDisplayVersion, "18.0.0 Preview 1");
    const VsInstance& tools = (*instances)[1];
    EXPECT_EQ(tools.instanceId, "39ecdf9a");
    EXPECT_EQ(tools.displayName, "Visual Studio Build Tools 2022");
    EXPECT_EQ(tools.productId, "Microsoft.VisualStudio.Product.BuildTools");
    EXPECT_EQ(tools.installationPath, "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\Build Töols");
    EXPECT_EQ(tools.installationVersion, "17.14.37710.0");
    EXPECT_EQ(tools.productDisplayVersion, "17.14.41 (September 2026)");
    EXPECT_FALSE(tools.isPrerelease);
    EXPECT_EQ(tools.msbuildPath, ""); // Filled by listVsInstances after checking the disk.
}

TEST(VswhereJson, EmptyListAndMalformedOutput) {
    auto none = frost::build::parseVswhereJson("[]\r\n");
    ASSERT_TRUE(none.has_value());
    EXPECT_TRUE(none->empty());
    auto skipped = frost::build::parseVswhereJson(R"([{"instanceId": "x"}])");
    ASSERT_TRUE(skipped.has_value());
    EXPECT_TRUE(skipped->empty()); // No installationPath.
    for (const std::string_view bad : {"", "Visual Studio Locator version 3.1.7", "{}", "[1]", "[{"}) {
        auto parsed = frost::build::parseVswhereJson(bad);
        ASSERT_FALSE(parsed.has_value()) << bad;
        EXPECT_EQ(parsed.error().code(), ErrorCode::Corrupt) << bad;
    }
}

TEST(VswhereArguments, ListAllInstancesNewestFirstAsJson) {
    EXPECT_EQ(frost::build::vswhereArguments(),
              (std::vector<std::string>{"-sort", "-prerelease", "-products", "*", "-requires",
                                        "Microsoft.Component.MSBuild", "-format", "json", "-utf8"}));
}

TEST(MsBuildPath, IsTheAmd64MsBuildOfTheInstallation) {
    EXPECT_EQ(frost::build::msbuildPathFor("C:\\VS\\BuildTools").value_or(""),
              "C:\\VS\\BuildTools\\MSBuild\\Current\\Bin\\amd64\\MSBuild.exe");
    EXPECT_EQ(frost::build::msbuildPathFor("C:/VS/BuildTools/").value_or(""),
              "C:\\VS\\BuildTools\\MSBuild\\Current\\Bin\\amd64\\MSBuild.exe");
    auto relative = frost::build::msbuildPathFor("VS\\BuildTools");
    ASSERT_FALSE(relative.has_value());
    EXPECT_EQ(relative.error().code(), ErrorCode::InvalidArgument);
}

std::vector<VsInstance> sampleInstances() {
    VsInstance broken{.instanceId = "new", .installationPath = "C:\\VS\\New"}; // No MSBuild.exe.
    VsInstance tools{.instanceId = "tools", .installationPath = "C:\\VS\\Tools", .msbuildPath = "C:\\VS\\Tools\\m.exe"};
    VsInstance old{.instanceId = "old", .installationPath = "C:\\VS\\Old", .msbuildPath = "C:\\VS\\Old\\m.exe"};
    return {broken, tools, old};
}

TEST(SelectVsInstance, PicksTheNewestWithMsBuildOrTheChosenOne) {
    const std::vector<VsInstance> instances = sampleInstances();
    EXPECT_EQ(frost::build::selectVsInstance(instances).value_or(VsInstance{}).instanceId, "tools");
    EXPECT_EQ(frost::build::selectVsInstance(instances, "old").value_or(VsInstance{}).instanceId, "old");

    auto missing = frost::build::selectVsInstance(instances, "gone");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    auto broken = frost::build::selectVsInstance(instances, "new");
    ASSERT_FALSE(broken.has_value());
    EXPECT_EQ(broken.error().code(), ErrorCode::NotFound);
    auto none = frost::build::selectVsInstance({});
    ASSERT_FALSE(none.has_value());
    EXPECT_EQ(none.error().code(), ErrorCode::NotFound);
    const std::vector<VsInstance> onlyBroken{instances[0]};
    EXPECT_FALSE(frost::build::selectVsInstance(onlyBroken).has_value());
}

#ifdef _WIN32

constexpr std::string_view kVswhere = "C:\\Installer\\vswhere.exe";

TEST(ListVsInstances, NoVswhereMeansNoVisualStudio) {
    frost::ManualClock clock;
    frost::io::MemFs fs(clock);
    frost::io::ScriptedProcessLauncher launcher;
    auto listed = frost::build::listVsInstances(launcher, fs, {.vswherePath = std::string(kVswhere)});
    ASSERT_FALSE(listed.has_value());
    EXPECT_EQ(listed.error().code(), ErrorCode::NotFound);
    EXPECT_NE(listed.error().message().find("no Visual Studio"), std::string::npos);
    EXPECT_TRUE(launcher.launches().empty());
}

TEST(ListVsInstances, RunsVswhereAndChecksEachMsBuild) {
    frost::ManualClock clock;
    frost::io::MemFs fs(clock);
    ASSERT_TRUE(fs.createDirectories("C:\\Installer").has_value());
    ASSERT_TRUE(fs.writeFileAtomic(std::string(kVswhere), "").has_value());
    const std::string msbuild =
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\Build Töols\\MSBuild\\Current\\Bin\\amd64";
    ASSERT_TRUE(fs.createDirectories(msbuild).has_value());
    ASSERT_TRUE(fs.writeFileAtomic(msbuild + "\\MSBuild.exe", "").has_value());

    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.expectExecutable = std::string(kVswhere), .stdoutBytes = std::string(kTwoInstances)});
    auto listed = frost::build::listVsInstances(launcher, fs, {.vswherePath = std::string(kVswhere)});
    ASSERT_TRUE(listed.has_value()) << listed.error().describe();
    ASSERT_EQ(listed->size(), 2U);
    EXPECT_EQ((*listed)[0].msbuildPath, ""); // The preview has no MSBuild.exe on this disk.
    EXPECT_EQ((*listed)[1].msbuildPath, msbuild + "\\MSBuild.exe");

    const auto launches = launcher.launches();
    ASSERT_EQ(launches.size(), 1U);
    EXPECT_EQ(launches[0].spec.arguments, frost::build::vswhereArguments());

    // findMsBuild picks the Build Tools, the newest instance with an MSBuild.exe.
    launcher.expect({.stdoutBytes = std::string(kTwoInstances)});
    auto found = frost::build::findMsBuild(launcher, fs, {.vswherePath = std::string(kVswhere)});
    ASSERT_TRUE(found.has_value()) << found.error().describe();
    EXPECT_EQ(found->instanceId, "39ecdf9a");
    // The user's choice of an instance without MSBuild is refused.
    launcher.expect({.stdoutBytes = std::string(kTwoInstances)});
    auto chosen = frost::build::findMsBuild(launcher, fs, {.vswherePath = std::string(kVswhere), .instanceId = "aa11"});
    ASSERT_FALSE(chosen.has_value());
    EXPECT_EQ(chosen.error().code(), ErrorCode::NotFound);
}

TEST(ListVsInstances, VswhereFailureIsAnError) {
    frost::ManualClock clock;
    frost::io::MemFs fs(clock);
    ASSERT_TRUE(fs.createDirectories("C:\\Installer").has_value());
    ASSERT_TRUE(fs.writeFileAtomic(std::string(kVswhere), "").has_value());
    frost::io::ScriptedProcessLauncher launcher;
    launcher.expect({.exitCode = 87, .stderrBytes = "Error 0x57: bad argument\r\n"});
    auto listed = frost::build::listVsInstances(launcher, fs, {.vswherePath = std::string(kVswhere)});
    ASSERT_FALSE(listed.has_value());
    EXPECT_EQ(listed.error().code(), ErrorCode::IoError);
    EXPECT_NE(listed.error().message().find("bad argument"), std::string::npos);
}

TEST(DefaultVswherePath, IsUnderProgramFilesX86) {
    auto path = frost::build::defaultVswherePath();
    ASSERT_TRUE(path.has_value()) << path.error().describe();
    EXPECT_TRUE(path->ends_with("\\Microsoft Visual Studio\\Installer\\vswhere.exe")) << *path;
}

#else

TEST(ListVsInstances, IsUnsupportedOffWindows) {
    frost::ManualClock clock;
    frost::io::MemFs fs(clock);
    frost::io::ScriptedProcessLauncher launcher;
    auto listed = frost::build::listVsInstances(launcher, fs);
    ASSERT_FALSE(listed.has_value());
    EXPECT_EQ(listed.error().code(), ErrorCode::Unsupported);
    auto found = frost::build::findMsBuild(launcher, fs);
    ASSERT_FALSE(found.has_value());
    EXPECT_EQ(found.error().code(), ErrorCode::Unsupported);
    EXPECT_EQ(frost::build::defaultVswherePath().error().code(), ErrorCode::Unsupported);
    EXPECT_TRUE(launcher.launches().empty());
}

#endif

} // namespace
