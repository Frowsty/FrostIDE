// SPDX-License-Identifier: Apache-2.0
//
// Finding MSBuild: the Visual Studio instances vswhere.exe reports (full IDEs and Build Tools
// alike), the MSBuild.exe inside each, and the choice of one (the newest, or one the user picked).
//
/*
 * Discovery runs %ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe, the locator the
 * Visual Studio Installer always installs there, through IProcessLauncher with
 *
 *     -sort -prerelease -products * -requires Microsoft.Component.MSBuild -format json -utf8
 *
 * -sort lists every complete instance, newest version and last installed first, which is the
 * order -latest picks its single instance from; so the first entry is what -latest would return,
 * and the rest stay available for a user who picked another instance. -products * includes Build
 * Tools; -prerelease includes previews. Each instance's MSBuild is
 * <installationPath>\MSBuild\Current\Bin\amd64\MSBuild.exe (VS 2019 and later), checked on disk.
 * TODO(M2): the VS Setup COM API as the primary source (PLAN.md section 2, toolchain detection),
 * cached per instance, with vswhere as the fallback.
 *
 * Off Windows there is no MSBuild to find: listVsInstances and findMsBuild fail with Unsupported.
 * The JSON reader and the selection are portable and tested on every platform.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/io/process.hpp"

#include <chrono>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace frost::build {

// One Visual Studio installation with MSBuild, as vswhere describes it.
struct VsInstance {
    std::string instanceId{};            // "39ecdf9a"; stable for the life of the installation.
    std::string displayName{};           // "Visual Studio Build Tools 2022".
    std::string productId{};             // "Microsoft.VisualStudio.Product.BuildTools".
    std::string installationPath{};      // "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools".
    std::string installationVersion{};   // "17.14.37710.0".
    std::string productDisplayVersion{}; // "17.14.41 (September 2026)" (catalog.productDisplayVersion), or empty.
    bool isPrerelease = false;
    // The absolute path of the instance's 64-bit MSBuild.exe; empty when that file does not exist
    // (an instance whose MSBuild component is broken or still being installed).
    std::string msbuildPath{};

    friend bool operator==(const VsInstance&, const VsInstance&) = default;
};

// The arguments discovery passes to vswhere.exe (header comment).
[[nodiscard]] std::vector<std::string> vswhereArguments();

/*
 * Reads vswhere's -format json output: an array with one object per instance, in vswhere's order.
 * Objects without an installationPath are skipped; msbuildPath is left empty (listVsInstances fills
 * it). Errors: Corrupt (not JSON, or not an array of objects).
 */
[[nodiscard]] Result<std::vector<VsInstance>> parseVswhereJson(std::string_view json);

// <installationPath>\MSBuild\Current\Bin\amd64\MSBuild.exe, with Windows path rules on every
// platform. Errors: InvalidArgument (not an absolute Windows path).
[[nodiscard]] Result<std::string> msbuildPathFor(std::string_view installationPath);

// The absolute path of vswhere.exe under the Program Files (x86) known folder. Errors: Unsupported
// (not Windows), NotFound (the folder is unknown).
[[nodiscard]] Result<std::string> defaultVswherePath();

struct MsBuildDiscoveryOptions {
    std::string vswherePath{}; // Absolute path of vswhere.exe; empty uses defaultVswherePath().
    std::string instanceId{};  // The instance to use (the user's choice); empty picks the newest.
    IClock::Duration timeout = std::chrono::seconds(30);
};

/*
 * Every instance vswhere reports, newest first, with msbuildPath set where MSBuild.exe exists
 * (checked through `fs`). Blocking (vswhere takes about 50 ms); call it off the UI thread.
 * Errors: Unsupported (not Windows); NotFound (no vswhere.exe, so no Visual Studio is installed);
 * Corrupt (unreadable output); IoError (vswhere failed); Timeout; and launch errors.
 */
[[nodiscard]] Result<std::vector<VsInstance>> listVsInstances(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                                              const MsBuildDiscoveryOptions& options = {});

/*
 * The instance to build with: the one whose instanceId equals `instanceId` (it must have an
 * MSBuild.exe), or, when `instanceId` is empty, the first one that has an MSBuild.exe. Errors:
 * NotFound (no instance at all, the chosen one is missing, or it has no MSBuild.exe).
 */
[[nodiscard]] Result<VsInstance> selectVsInstance(std::span<const VsInstance> instances,
                                                  std::string_view instanceId = {});

// listVsInstances followed by selectVsInstance with options.instanceId. Errors: those of both.
[[nodiscard]] Result<VsInstance> findMsBuild(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                             const MsBuildDiscoveryOptions& options = {});

} // namespace frost::build
