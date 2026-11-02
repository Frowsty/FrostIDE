// SPDX-License-Identifier: Apache-2.0
//
// The MSVC toolset and Windows SDK include directories for synthesized compile commands (W2,
// ADR 0036): Visual Studio through vswhere.exe (launched by absolute path through
// IProcessLauncher), the toolset version from Microsoft.VCToolsVersion.default.txt, and the
// Windows 10/11 SDK through the registry's "Installed Roots".
/*
 * Lookup (Windows):
 *   1. vswhere.exe in "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer" (the known-folder
 *      path, never PATH), run as `-latest -products * -requires
 *      Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json -utf8`, gives the
 *      installation path (Build Tools included).
 *   2. <install>\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt names the toolset;
 *      <install>\VC\Tools\MSVC\<version>\include (and ATLMFC\include when present) are added.
 *   3. HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots, value KitsRoot10 (32-bit registry
 *      view, where the SDK installer writes it), gives the SDK root; the highest version under
 *      <root>\Include that has um\windows.h contributes ucrt, shared, um, winrt and cppwinrt.
 * Linux and other platforms: Unsupported (there is no MSVC there; clang-cl commands still work
 * for code that does not include the Windows headers).
 * Blocking (vswhere takes about 0.2 s); call it on a worker and cache the result.
 */
#pragma once

#include "frost/core/error.hpp"

#include <string>
#include <vector>

namespace frost::io {
class IFileSystem;
class IProcessLauncher;
} // namespace frost::io

namespace frost::lang {

struct MsvcEnvironment {
    std::string installationPath;                // The Visual Studio (or Build Tools) installation.
    std::string toolsetVersion;                  // "14.44.35207".
    std::string sdkVersion;                      // "10.0.26100.0"; "" when no SDK was found.
    std::vector<std::string> includeDirectories; // MSVC first, then the SDK, in that order.
};

// See the header comment. Errors: Unsupported (not Windows), NotFound (no vswhere, no
// installation with the C++ tools, no toolset directory), IoError and Timeout (vswhere failed).
[[nodiscard]] Result<MsvcEnvironment> findMsvcEnvironment(io::IProcessLauncher& launcher, io::IFileSystem& fs);

} // namespace frost::lang
