// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the toolchain discovery core (toolchain.hpp, SLN-14) over a MemFs laid out like a
// Visual Studio installation and a Windows Kits root: toolsets (newest first, the default, partial
// ones skipped), platform toolsets, SDKs, the inventory's unavailable reasons, choice checking, the
// description lines, and the vcvarsall command line and cache names of a VcvarsSelection.
// The real tools are driven by toolchain_real_test.cpp.

#include "frost/build/toolchain.hpp"
#include "frost/io/scripted_process_launcher.hpp"

#include "build_test_util.hpp"
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace frost::build {
namespace {

using test::MemTree;

// A Build Tools-like installation under <root>/VS with `toolsets` (each with an x64 cl.exe), a
// default file naming `defaultToolset`, vcvarsall.bat and the v143/ClangCL platform toolsets.
void writeInstallation(MemTree& tree, const std::vector<std::string>& toolsets, const std::string& defaultToolset) {
    for (const std::string& version : toolsets) {
        tree.write("VS/VC/Tools/MSVC/" + version + "/bin/Hostx64/x64/cl.exe", "MZ");
    }
    tree.write("VS/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt", defaultToolset + "\r\n");
    tree.write("VS/VC/Auxiliary/Build/vcvarsall.bat", "@echo off");
    tree.write("VS/MSBuild/Microsoft/VC/v170/Platforms/x64/PlatformToolsets/v143/Toolset.props", "<Project/>");
    tree.write("VS/MSBuild/Microsoft/VC/v170/Platforms/x64/PlatformToolsets/ClangCL/Toolset.props", "<Project/>");
    tree.write("VS/MSBuild/Microsoft/VC/v160/Platforms/x64/PlatformToolsets/v143/Toolset.props", "<Project/>");
}

void writeSdk(MemTree& tree, const std::string& version, bool headers, bool libraries) {
    tree.write("Kits/Include/" + version + "/ucrt/stdio.h", "");
    if (headers) {
        tree.write("Kits/Include/" + version + "/um/windows.h", "");
    }
    if (libraries) {
        tree.write("Kits/Lib/" + version + "/um/x64/kernel32.lib", "");
    }
}

VsInstance instanceAt(std::string id, std::string path) {
    VsInstance instance;
    instance.instanceId = std::move(id);
    instance.displayName = "Visual Studio Build Tools 2022";
    instance.installationPath = std::move(path);
    instance.installationVersion = "17.14.37710.0";
    return instance;
}

TEST(Toolchain, VersionsOrderNumerically) {
    EXPECT_TRUE(versionNewer("14.44.35207", "14.38.33130"));
    EXPECT_TRUE(versionNewer("14.100.1", "14.99.1"));
    EXPECT_FALSE(versionNewer("10.0.22621.0", "10.0.26100.0"));
    EXPECT_FALSE(versionNewer("14.44", "14.44"));
}

TEST(Toolchain, ListsToolsetsNewestFirstWithTheDefaultAndSkipsPartialOnes) {
    MemTree tree;
    writeInstallation(tree, {"14.38.33130", "14.44.35207"}, "14.38.33130");
    tree.write("VS/VC/Tools/MSVC/14.29.30133/include/vector", ""); // No x64 compiler: skipped.

    auto toolsets = listMsvcToolsets(tree.fs, MemTree::path("VS"));
    ASSERT_TRUE(toolsets.has_value()) << toolsets.error().describe();
    ASSERT_EQ(toolsets->size(), 2U);
    EXPECT_EQ((*toolsets)[0].version, "14.44.35207");
    EXPECT_FALSE((*toolsets)[0].isDefault);
    EXPECT_EQ((*toolsets)[0].compiler, MemTree::path("VS/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe"));
    EXPECT_EQ((*toolsets)[1].version, "14.38.33130");
    EXPECT_TRUE((*toolsets)[1].isDefault);
    EXPECT_EQ(defaultMsvcToolset(tree.fs, MemTree::path("VS")), "14.38.33130");

    EXPECT_EQ(listPlatformToolsets(tree.fs, MemTree::path("VS")), (std::vector<std::string>{"ClangCL", "v143"}));
    EXPECT_EQ(listMsvcToolsets(tree.fs, MemTree::path("nothing")).error().code(), ErrorCode::NotFound);
    EXPECT_EQ(listMsvcToolsets(tree.fs, "relative").error().code(), ErrorCode::InvalidArgument);
}

TEST(Toolchain, ListsSdksNewestFirstWithWhatTheyHold) {
    MemTree tree;
    writeSdk(tree, "10.0.22621.0", true, true);
    writeSdk(tree, "10.0.26100.0", true, false);
    tree.write("Kits/Include/wdf/kmdf.h", ""); // Not an SDK version folder.

    auto sdks = listWindowsSdks(tree.fs, MemTree::path("Kits"));
    ASSERT_TRUE(sdks.has_value()) << sdks.error().describe();
    ASSERT_EQ(sdks->size(), 2U);
    EXPECT_EQ((*sdks)[0].version, "10.0.26100.0");
    EXPECT_FALSE((*sdks)[0].usable());
    EXPECT_TRUE((*sdks)[0].hasDesktopHeaders);
    EXPECT_EQ((*sdks)[1].version, "10.0.22621.0");
    EXPECT_TRUE((*sdks)[1].usable());
    EXPECT_EQ((*sdks)[1].includeDirectory, MemTree::path("Kits/Include/10.0.22621.0"));
    EXPECT_EQ(listWindowsSdks(tree.fs, MemTree::path("missing")).error().code(), ErrorCode::NotFound);
}

TEST(Toolchain, TheInventoryNamesWhatIsUnavailable) {
    MemTree tree;
    io::ScriptedProcessLauncher launcher;
    // vswhere listed one instance without the C++ tools, and there is no SDK folder.
    tree.write("Bare/Common7/IDE/devenv.exe", "MZ");
    ToolchainDiscoveryOptions options;
    options.instances = std::vector<VsInstance>{instanceAt("bare", MemTree::path("Bare"))};
    options.windowsKitsRoot = MemTree::path("Kits");
    const ToolchainInventory inventory = discoverToolchains(launcher, tree.fs, options);
    ASSERT_EQ(inventory.visualStudio.size(), 1U);
    EXPECT_FALSE(inventory.visualStudio[0].hasCppTools());
    ASSERT_TRUE(inventory.visualStudioError.has_value());
    EXPECT_NE(inventory.visualStudioError->message().find("C++ build tools"), std::string::npos);
    ASSERT_TRUE(inventory.windowsSdkError.has_value());
    EXPECT_EQ(inventory.windowsSdkError->code(), ErrorCode::NotFound);
    EXPECT_TRUE(launcher.launches().empty()); // The given instances replaced vswhere.

    const auto lines = describeToolchains(inventory);
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_NE(lines[0].find("no C++ build tools"), std::string::npos) << lines[0];
    EXPECT_TRUE(lines[1].starts_with("Windows SDK: unavailable")) << lines[1];

    auto resolved = resolveToolchain(inventory, {});
    ASSERT_FALSE(resolved.has_value());
    EXPECT_EQ(resolved.error().code(), ErrorCode::NotFound);
}

TEST(Toolchain, NoVswhereMeansVisualStudioIsUnavailable) {
    MemTree tree;
    io::ScriptedProcessLauncher launcher;
    ToolchainDiscoveryOptions options;
    options.vswhere.vswherePath = MemTree::path("Installer/vswhere.exe");
    options.windowsKitsRoot = MemTree::path("Kits");
    const ToolchainInventory inventory = discoverToolchains(launcher, tree.fs, options);
    EXPECT_TRUE(inventory.visualStudio.empty());
    ASSERT_TRUE(inventory.visualStudioError.has_value());
    // Windows: no vswhere.exe; elsewhere there is no Visual Studio at all.
    EXPECT_EQ(inventory.visualStudioError->code(), test::kWindows ? ErrorCode::NotFound : ErrorCode::Unsupported);
    EXPECT_TRUE(describeToolchains(inventory).front().starts_with("Visual Studio: unavailable"));
}

TEST(Toolchain, ResolvesChoicesAgainstWhatIsInstalled) {
    MemTree tree;
    io::ScriptedProcessLauncher launcher;
    writeInstallation(tree, {"14.38.33130", "14.44.35207"}, "14.44.35207");
    writeSdk(tree, "10.0.26100.0", true, true);
    writeSdk(tree, "10.0.19041.0", false, true);
    ToolchainDiscoveryOptions options;
    options.instances = std::vector<VsInstance>{instanceAt("tools", MemTree::path("VS"))};
    options.windowsKitsRoot = MemTree::path("Kits");
    const ToolchainInventory inventory = discoverToolchains(launcher, tree.fs, options);
    EXPECT_FALSE(inventory.visualStudioError.has_value());
    EXPECT_FALSE(inventory.windowsSdkError.has_value());
    ASSERT_NE(inventory.firstWithCppTools(), nullptr);
    EXPECT_EQ(inventory.firstWithCppTools()->vcvarsall, MemTree::path("VS/VC/Auxiliary/Build/vcvarsall.bat"));
    EXPECT_EQ(inventory.firstWithCppTools()->defaultToolset, "14.44.35207");

    // Defaults: the default toolset, vcvarsall's own SDK choice (nothing in the selection).
    auto defaults = resolveToolchain(inventory, {});
    ASSERT_TRUE(defaults.has_value()) << defaults.error().describe();
    EXPECT_EQ(defaults->toolset.version, "14.44.35207");
    EXPECT_TRUE(defaults->selection.empty());
    EXPECT_EQ(defaults->architecture, "x64");

    // A prefix picks the newest toolset it matches; the SDK is checked.
    auto chosen = resolveToolchain(
        inventory,
        {.instanceId = "tools", .toolsetVersion = "14.38", .sdkVersion = "10.0.26100.0", .architecture = "x64"});
    ASSERT_TRUE(chosen.has_value()) << chosen.error().describe();
    EXPECT_EQ(chosen->toolset.version, "14.38.33130");
    EXPECT_EQ(chosen->selection, (VcvarsSelection{.toolsetVersion = "14.38", .sdkVersion = "10.0.26100.0"}));

    const auto refusal = [&](const ToolchainChoice& choice) {
        auto result = resolveToolchain(inventory, choice);
        EXPECT_FALSE(result.has_value());
        return result ? std::string() : result.error().message();
    };
    EXPECT_NE(refusal({.toolsetVersion = "14.3"}).find("14.44.35207, 14.38.33130"), std::string::npos);
    EXPECT_NE(refusal({.sdkVersion = "10.0.22000.0"}).find("not installed"), std::string::npos);
    EXPECT_NE(refusal({.sdkVersion = "10.0.19041.0"}).find("no desktop headers"), std::string::npos);
    EXPECT_NE(refusal({.instanceId = "other"}).find("not installed"), std::string::npos);
    EXPECT_NE(refusal({.architecture = "mips"}).find("architecture"), std::string::npos);
    EXPECT_NE(refusal({.toolsetVersion = "14.44 & calc"}).find("not a version"), std::string::npos);

    const auto lines = describeToolchains(inventory);
    ASSERT_EQ(lines.size(), 3U);
    EXPECT_NE(lines[0].find("MSVC 14.44.35207 (default), 14.38.33130; platform toolsets ClangCL, v143"),
              std::string::npos)
        << lines[0];
    EXPECT_EQ(lines[1], "Windows SDK 10.0.26100.0");
    EXPECT_EQ(lines[2], "Windows SDK 10.0.19041.0 (no desktop headers)");
}

TEST(VcvarsSelectionTest, TheCallNamesTheSdkAndTheToolsetInVcvarsallsOrder) {
    const std::string cmd = "C:\\Windows\\System32\\cmd.exe";
    const std::string vcvars = "C:\\VS\\VC\\Auxiliary\\Build\\vcvarsall.bat";
    auto spec = vcvarsProcessSpec(cmd, vcvars, "x64", {.toolsetVersion = "14.44", .sdkVersion = "10.0.26100.0"});
    ASSERT_TRUE(spec.has_value()) << spec.error().describe();
    EXPECT_EQ(spec->arguments,
              (std::vector<std::string>{"/d", "/u", "/c", "set", "&&", "echo", std::string(kVcvarsMarker), "&&", "call",
                                        vcvars, "x64", "10.0.26100.0", "-vcvars_ver=14.44", ">nul", "&&", "set"}));
    auto toolsetOnly = vcvarsProcessSpec(cmd, vcvars, "x64", {.toolsetVersion = "14.44.35207", .sdkVersion = {}});
    ASSERT_TRUE(toolsetOnly.has_value());
    EXPECT_EQ(toolsetOnly->arguments[11], "-vcvars_ver=14.44.35207");
    // Anything but version text never reaches cmd.exe.
    EXPECT_EQ(
        vcvarsProcessSpec(cmd, vcvars, "x64", {.toolsetVersion = "14.44 & calc", .sdkVersion = {}}).error().code(),
        ErrorCode::InvalidArgument);
    EXPECT_EQ(vcvarsProcessSpec(cmd, vcvars, "x64", {.toolsetVersion = {}, .sdkVersion = "%PATH%"}).error().code(),
              ErrorCode::InvalidArgument);
    EXPECT_TRUE(isVcvarsVersionText("10.0.26100.0"));
    EXPECT_FALSE(isVcvarsVersionText(".14"));
    EXPECT_FALSE(isVcvarsVersionText(""));
}

TEST(VcvarsSelectionTest, EachSelectionHasItsOwnCacheFile) {
    MemTree tree;
    io::ScriptedProcessLauncher launcher;
    MsvcEnvironmentCache cache(tree.fs, launcher, MemTree::path("cache"));
    const std::string plain = cache.cacheFile("abc", "X64");
    EXPECT_EQ(plain, MemTree::path("cache/msvc-env-abc-x64.txt")); // Unchanged for the default selection.
    EXPECT_EQ(cache.cacheFile("abc", "x64", {.toolsetVersion = "14.44", .sdkVersion = "10.0.26100.0"}),
              MemTree::path("cache/msvc-env-abc-x64-t14.44-s10.0.26100.0.txt"));
}

TEST(VcvarsSelectionTest, EnvironmentValueIgnoresCaseAndTakesTheLastChange) {
    const project::CMakeEnvironment environment{{"VCToolsVersion", std::string("14.38.33130")},
                                                {"Path", std::string("C:\\a")},
                                                {"vctoolsversion", std::string("14.44.35207")},
                                                {"Gone", std::nullopt}};
    EXPECT_EQ(environmentValue(environment, "VCTOOLSVERSION").value_or(""), "14.44.35207");
    EXPECT_FALSE(environmentValue(environment, "Gone").has_value());
    EXPECT_FALSE(environmentValue(environment, "Missing").has_value());
}

} // namespace
} // namespace frost::build
