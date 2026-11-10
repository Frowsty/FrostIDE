// SPDX-License-Identifier: Apache-2.0
//
// Toolchain discovery core (SLN-14, PLAN.md Appendix C R3): what MSVC toolchains this machine has,
// read from the real tools and the disk, shared by the CMake path (vcvarsall environments,
// cmake_toolchain.hpp) and the MSBuild path (the instance's MSBuild.exe, msbuild_locator.hpp).
//
/*
 * What is discovered (discoverToolchains), on Windows:
 *   - Visual Studio instances: vswhere.exe through IProcessLauncher (listVsInstances, the MSBuild
 *     locator's own query: full IDEs and Build Tools, newest first), each with its MSBuild.exe;
 *   - per instance, its MSVC toolsets: the directories <install>\VC\Tools\MSVC\<version> that hold
 *     bin\Hostx64\x64\cl.exe, newest first, and the default one that
 *     <install>\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt names (what vcvarsall picks
 *     without -vcvars_ver);
 *   - per instance, the MSBuild platform toolsets it can build x64 with (v143, ClangCL, ...): the
 *     directories <install>\MSBuild\Microsoft\VC\<v1xx>\Platforms\x64\PlatformToolsets\<name>;
 *   - the Windows 10/11 SDKs: KitsRoot10 under HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots
 *     (32-bit registry view, where the SDK installer writes it), and each <root>\Include\10.* folder,
 *     newest first, with whether it has the desktop headers (um\windows.h) and x64 import libraries
 *     (Lib\<version>\um\x64\kernel32.lib).
 * Nothing is guessed: a missing vswhere, a failed query, an instance without the C++ tools, or a
 * machine without a Windows SDK leave the matching list empty with the reason in the inventory's
 * error members, which the UI shows as "unavailable" (truth rule). The environment of a chosen
 * toolset and SDK comes from vcvarsall.bat itself (captureMsvcEnvironment with a VcvarsSelection);
 * resolveToolchain checks a choice against the inventory first, so a typo is named, not run.
 *
 * Off Windows there is no MSVC: discoverToolchains reports Unsupported for both parts. The listing
 * functions over an IFileSystem are portable (tests use a MemFs) and take native paths.
 *
 * Thread safety: free functions over values; discoverToolchains blocks (vswhere about 50-200 ms
 * plus directory listings), so call it off the UI thread and keep the result.
 */
#pragma once

#include "frost/build/cmake_toolchain.hpp"
#include "frost/build/msbuild_locator.hpp"
#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/process.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::build {

// One MSVC toolset of an instance (header comment).
struct MsvcToolset {
    std::string version{};   // "14.44.35207", the directory's name.
    std::string directory{}; // <install>\VC\Tools\MSVC\<version>.
    std::string compiler{};  // bin\Hostx64\x64\cl.exe under it (it exists).
    bool isDefault = false;  // The version Microsoft.VCToolsVersion.default.txt names.

    friend bool operator==(const MsvcToolset&, const MsvcToolset&) = default;
};

// One Visual Studio instance with what it can build with.
struct VsToolchain {
    VsInstance instance{};
    std::string vcvarsall{};             // <install>\VC\Auxiliary\Build\vcvarsall.bat; "" without the C++ tools.
    std::vector<MsvcToolset> toolsets{}; // Newest first; empty without the C++ tools.
    std::string defaultToolset{};        // The default toolset's version; "" when the file is missing.
    std::vector<std::string> platformToolsets{}; // "ClangCL", "v143", ... sorted; MSBuild's PlatformToolset values.

    [[nodiscard]] bool hasCppTools() const noexcept { return !vcvarsall.empty() && !toolsets.empty(); }
    friend bool operator==(const VsToolchain&, const VsToolchain&) = default;
};

// One installed Windows SDK (header comment).
struct WindowsSdk {
    std::string version{};          // "10.0.26100.0".
    std::string includeDirectory{}; // <root>\Include\<version>.
    bool hasDesktopHeaders = false; // um\windows.h exists.
    bool hasX64Libraries = false;   // Lib\<version>\um\x64\kernel32.lib exists.

    [[nodiscard]] bool usable() const noexcept { return hasDesktopHeaders && hasX64Libraries; }
    friend bool operator==(const WindowsSdk&, const WindowsSdk&) = default;
};

// Everything discoverToolchains found, with the reason for each part that is unavailable.
struct ToolchainInventory {
    std::vector<VsToolchain> visualStudio{};  // vswhere's order (newest first).
    std::optional<Error> visualStudioError{}; // Why visualStudio is empty (no vswhere, a failed query, ...).
    std::string windowsKitsRoot{};            // KitsRoot10 ("C:\Program Files (x86)\Windows Kits\10"); "" if unknown.
    std::vector<WindowsSdk> windowsSdks{};    // Newest first.
    std::optional<Error> windowsSdkError{};   // Why windowsSdks is empty.

    // The first instance with the C++ tools, or null.
    [[nodiscard]] const VsToolchain* firstWithCppTools() const noexcept;
    // The instance `instanceId`, or null.
    [[nodiscard]] const VsToolchain* find(std::string_view instanceId) const noexcept;
};

// Orders "14.44.35207" against "14.38.33130" numerically, part by part; non-digits count as 0. Pure.
[[nodiscard]] bool versionNewer(std::string_view left, std::string_view right) noexcept;

/*
 * The toolsets under <installationPath>\VC\Tools\MSVC (header comment), newest first, isDefault set
 * from the default version file. Errors: InvalidArgument (not an absolute path), NotFound (no
 * VC\Tools\MSVC folder: the instance has no C++ tools). Blocking (directory listings).
 */
[[nodiscard]] Result<std::vector<MsvcToolset>> listMsvcToolsets(io::IFileSystem& fs, std::string_view installationPath);

// The trimmed content of Microsoft.VCToolsVersion.default.txt; "" when it is missing or empty.
[[nodiscard]] std::string defaultMsvcToolset(io::IFileSystem& fs, std::string_view installationPath);

// The x64 platform toolsets of an instance (header comment), sorted, without duplicates; empty when none.
[[nodiscard]] std::vector<std::string> listPlatformToolsets(io::IFileSystem& fs, std::string_view installationPath);

// The SDKs under `kitsRoot` (header comment), newest first. Errors: InvalidArgument (not absolute),
// NotFound (no Include folder).
[[nodiscard]] Result<std::vector<WindowsSdk>> listWindowsSdks(io::IFileSystem& fs, std::string_view kitsRoot);

// KitsRoot10 from the registry (header comment), normalised. Errors: Unsupported (not Windows),
// NotFound (no Windows 10/11 SDK registered).
[[nodiscard]] Result<std::string> windowsKitsRoot();

// One instance's toolsets, default and platform toolsets (header comment). Never fails: an instance
// without the C++ tools has empty lists.
[[nodiscard]] VsToolchain describeInstance(io::IFileSystem& fs, const VsInstance& instance);

struct ToolchainDiscoveryOptions {
    MsBuildDiscoveryOptions vswhere{}; // vswhere's path and timeout (empty path: the default one).
    // The SDK root to list; "" asks the registry (windowsKitsRoot). For tests.
    std::string windowsKitsRoot{};
    // The instances to describe instead of running vswhere (tests; nullopt runs vswhere).
    std::optional<std::vector<VsInstance>> instances{};
};

// Discovers everything (header comment). Never fails as a whole; see the error members.
[[nodiscard]] ToolchainInventory discoverToolchains(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                                    const ToolchainDiscoveryOptions& options = {});

// A choice of toolchain, as settings or a CMake profile give it; empty members take the defaults.
struct ToolchainChoice {
    std::string instanceId{};     // "" takes the first instance with the C++ tools.
    std::string toolsetVersion{}; // "" takes the instance's default; a prefix such as "14.44" is allowed.
    std::string sdkVersion{};     // "" takes vcvarsall's default (the newest SDK).
    std::string architecture = "x64";
};

// A choice checked against the inventory: what vcvarsall is asked for.
struct ResolvedToolchain {
    VsToolchain toolchain{};
    MsvcToolset toolset{};       // The toolset that will be set up (the default one for an empty choice).
    VcvarsSelection selection{}; // Empty members where the choice took vcvarsall's defaults.
    std::string architecture{};
};

/*
 * Checks `choice` against `inventory` (header comment). Errors: NotFound naming what is missing (no
 * instance with the C++ tools, the chosen instance, a toolset or SDK that is not installed, an SDK
 * without the desktop headers), InvalidArgument (an architecture vcvarsall does not know, version
 * text that is not digits and dots), or the inventory's own error when it found nothing. Pure.
 */
[[nodiscard]] Result<ResolvedToolchain> resolveToolchain(const ToolchainInventory& inventory,
                                                         const ToolchainChoice& choice);

/*
 * The inventory as lines for a log or the Build window: one per instance (toolsets, default,
 * platform toolsets, MSBuild) and one per SDK, or the reason a part is unavailable. Pure.
 */
[[nodiscard]] std::vector<std::string> describeToolchains(const ToolchainInventory& inventory);

} // namespace frost::build
