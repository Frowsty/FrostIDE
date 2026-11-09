// SPDX-License-Identifier: Apache-2.0
//
// The CMake toolchain (ADR 0046): finding cmake and ninja, and on Windows the MSVC developer
// environment captured from vcvarsall.bat, so Ninja builds find cl or clang-cl, their headers and
// libraries as they do in a Developer Command Prompt (CLion's "Visual Studio" toolchain).
//
/*
 * Tool discovery (findCMakeTools), first match wins for each tool:
 *   1. the settings (tools.cmake.path, tools.ninja.path: application-scope `executes` keys);
 *   2. the pinned SDK (CMakeToolOptions::sdkDirectory, which the workbench takes from FROST_SDK;
 *      tools/env.ps1 and ~/sdk/env.sh set it for development and tests):
 *      <sdk>/cmake-<pinned>/bin/cmake[.exe] and <sdk>/ninja-<pinned>/ninja[.exe], the versions
 *      sdk/sdk.lock.json pins (pinnedCMakeVersion, pinnedNinjaVersion). The same rule as the
 *      clangd, LuaLS and lldb-dap searches, so tests and journeys run the pinned tools (R1, CMK-1)
 *      rather than whatever Visual Studio bundles;
 *   3. the CMake and Ninja that Visual Studio bundles, in the instances vswhere listed (newest
 *      first): <installationPath>\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
 *      and ...\CMake\Ninja\ninja.exe;
 *   4. the absolute directories of the PATH variable, in order ("cmake.exe" / "cmake"). Relative
 *      entries are skipped: the current or project directory is never searched (PLAN.md 3.5).
 * Each candidate must be a regular file. The result is absolute; children start through
 * IProcessLauncher only.
 *
 * MSVC environment (Windows). captureMsvcEnvironment runs the absolute %SystemRoot%\System32\cmd.exe
 * (IProcessLauncher refuses batch files, ADR 0011) as
 *
 *     cmd.exe /d /u /c set && echo <marker> && call "<vcvarsall.bat>" <arch> >nul && set
 *
 * With a VcvarsSelection (SLN-14) the call names the Windows SDK and the MSVC toolset too, in
 * vcvarsall's own order: `call "<vcvarsall.bat>" <arch> [<sdk version>] [-vcvars_ver=<toolset>]`.
 * Both are version text (digits and dots) or the call is refused. vcvarsall exits non-zero when
 * either is not installed; the capture then fails, and a capture whose VCToolsVersion or
 * WindowsSDKVersion is not the requested one is refused too (IoError), so the environment is
 * always the one asked for.
 *
 * Each word is its own argument and only the vcvarsall path is quoted (by the launcher's CRT
 * rules); because the command after /c does not start with a quote, cmd.exe keeps that quoting,
 * so the path needs no cmd-specific escaping. Paths containing '"', '%', '^' or '!' are refused,
 * and the architecture must be one vcvarsall knows (isVcvarsArchitecture), so nothing else reaches
 * the command line. /u makes `set` and `echo` write UTF-16LE, so non-ASCII values survive. The
 * environment before the marker is the one cmd.exe started with; the result is the difference
 * (variables vcvarsall set or changed, ASCII case ignored in names). VSCMD_SKIP_SENDTELEMETRY=1
 * is set for the child. Off Windows it fails with Unsupported.
 *
 * Cache (MsvcEnvironmentCache): in memory per (instance, architecture, selection) and on disk as
 * <cacheDirectory>/msvc-env-<instanceId>-<arch>[-t<toolset>][-s<sdk>].txt, which records the instance's
 * installationVersion. An entry whose version differs from the instance's (Visual Studio was
 * updated) is captured again; so is one that does not parse. File format: a header line
 * "# FrostIDE MSVC environment v1", "version\t<installationVersion>", then "set\tNAME=VALUE" and
 * "unset\tNAME" lines.
 *
 * Thread safety: the free functions are thread-safe; MsvcEnvironmentCache is internally locked.
 */
#pragma once

#include "frost/build/msbuild_locator.hpp"
#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"
#include "frost/io/process.hpp"
#include "frost/project/cmake_presets.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frost::io {
class IFileSystem;
} // namespace frost::io

namespace frost::build {

// Where a tool was found.
enum class ToolOrigin : std::uint8_t { NotFound, Settings, VisualStudio, Path, Sdk };

// "not found", "settings", "Visual Studio", "PATH" or "pinned SDK".
[[nodiscard]] std::string_view toString(ToolOrigin origin) noexcept;

struct CMakeToolOptions {
    std::string cmakePath{};               // tools.cmake.path; "" detects.
    std::string ninjaPath{};               // tools.ninja.path; "" detects.
    std::vector<VsInstance> vsInstances{}; // vswhere's instances, newest first; empty skips step 2.
    std::string pathVariable{};            // The PATH value to search; "" searches nothing.
    std::string sdkDirectory{};            // The pinned SDK root (step 2); "" skips that step.
};

struct CMakeTools {
    std::string cmake{}; // Absolute; "" when not found.
    ToolOrigin cmakeOrigin = ToolOrigin::NotFound;
    std::string ninja{}; // Absolute; "" when not found (only Ninja generators need it).
    ToolOrigin ninjaOrigin = ToolOrigin::NotFound;
};

// The cmake and Ninja versions sdk/sdk.lock.json pins for this platform ("4.4.5", "1.13.2").
[[nodiscard]] std::string_view pinnedCMakeVersion() noexcept;
[[nodiscard]] std::string_view pinnedNinjaVersion() noexcept;

// The pinned SDK's tools under `sdkDirectory` (header comment, step 2), whether or not they exist.
// Errors: InvalidArgument (not an absolute path).
[[nodiscard]] Result<std::string> sdkCMakePath(std::string_view sdkDirectory);
[[nodiscard]] Result<std::string> sdkNinjaPath(std::string_view sdkDirectory);

// The bundled tools of a Visual Studio installation (header comment). Errors: InvalidArgument.
[[nodiscard]] Result<std::string> vsBundledCMakePath(std::string_view installationPath);
[[nodiscard]] Result<std::string> vsBundledNinjaPath(std::string_view installationPath);

// The absolute entries of a PATH value, split at ';' (Windows) or ':' (POSIX), normalised, in order,
// without duplicates and without surrounding quotes.
[[nodiscard]] std::vector<std::string> splitPathVariable(std::string_view value);

// This process's PATH ("" when unset).
[[nodiscard]] std::string processPathVariable();

/*
 * Finds cmake and ninja (header comment). A settings path that is not an absolute regular file is
 * an error rather than a fallback, so a typo is reported. Errors: InvalidArgument or NotFound for a
 * bad settings path; NotFound when no cmake exists anywhere.
 */
[[nodiscard]] Result<CMakeTools> findCMakeTools(io::IFileSystem& fs, const CMakeToolOptions& options);

/*
 * `cmake --version`'s version ("4.4.5"). Blocking (about 50 ms). Errors: the launcher's; IoError
 * (cmake failed), Corrupt (no "cmake version" line), Timeout.
 */
[[nodiscard]] Result<std::string> cmakeVersion(io::IProcessLauncher& launcher, std::string_view cmakePath);

// Whether `version` ("3.28.1", "4.4.5-rc1") is at least major.minor.
[[nodiscard]] bool versionAtLeast(std::string_view version, int major, int minor) noexcept;

// The architectures vcvarsall.bat accepts: x86, x64, amd64, arm64 and the cross forms (x64_arm64, ...).
[[nodiscard]] bool isVcvarsArchitecture(std::string_view architecture) noexcept;

// <installationPath>\VC\Auxiliary\Build\vcvarsall.bat. Errors: InvalidArgument.
[[nodiscard]] Result<std::string> vcvarsallPath(std::string_view installationPath);

// %SystemRoot%\System32\cmd.exe (GetSystemDirectoryW). Errors: Unsupported (not Windows), NotFound.
[[nodiscard]] Result<std::string> systemCmdPath();

/*
 * The cmd.exe spec of the capture (header comment). Pure and portable. Errors: InvalidArgument (a
 * relative path, a path with '"', '%', '^' or '!', an unknown architecture).
 */
[[nodiscard]] Result<io::ProcessSpec> vcvarsProcessSpec(std::string_view cmdPath, std::string_view vcvarsall,
                                                        std::string_view architecture);

/*
 * Which toolset and Windows SDK vcvarsall sets up (SLN-14, toolchain.hpp lists what is installed).
 * Empty members leave vcvarsall's defaults: the toolset Microsoft.VCToolsVersion.default.txt
 * names and the newest SDK. A toolset may be a prefix ("14.44") that vcvarsall completes.
 */
struct VcvarsSelection {
    std::string toolsetVersion{}; // "14.44.35207", "14.44" or "".
    std::string sdkVersion{};     // "10.0.26100.0" or "".

    [[nodiscard]] bool empty() const noexcept { return toolsetVersion.empty() && sdkVersion.empty(); }
    friend bool operator==(const VcvarsSelection&, const VcvarsSelection&) = default;
};

// Whether `text` is version text vcvarsall may get: 1 to 32 of digits and dots, starting with a digit.
[[nodiscard]] bool isVcvarsVersionText(std::string_view text) noexcept;

/*
 * As above, with `selection`'s SDK and toolset after the architecture (header comment). Errors:
 * InvalidArgument (those above, or a selection member that is not version text).
 */
[[nodiscard]] Result<io::ProcessSpec> vcvarsProcessSpec(std::string_view cmdPath, std::string_view vcvarsall,
                                                        std::string_view architecture,
                                                        const VcvarsSelection& selection);

// The marker line between the two `set` listings of the capture.
inline constexpr std::string_view kVcvarsMarker = "FROST-VCVARS-AFTER";

/*
 * Reads the capture's output (UTF-8, already decoded from UTF-16LE): the "NAME=VALUE" lines before
 * and after the marker, and returns what changed (header comment). Errors: Corrupt (no marker, or
 * no line after it).
 */
[[nodiscard]] Result<project::CMakeEnvironment> parseVcvarsCapture(std::string_view output);

// UTF-16LE bytes (cmd.exe /u) as UTF-8; an odd last byte is dropped, invalid units become U+FFFD.
[[nodiscard]] std::string decodeUtf16Le(std::string_view bytes);

// One captured environment.
struct MsvcDevEnvironment {
    std::string instanceId{};
    std::string installationVersion{};
    std::string architecture{};
    VcvarsSelection selection{};         // What was asked for (empty: vcvarsall's defaults).
    std::string toolsetVersion{};        // The captured VCToolsVersion ("14.44.35207"); "" when unset.
    std::string sdkVersion{};            // The captured WindowsSDKVersion without its ''; "" when unset.
    project::CMakeEnvironment changes{}; // What vcvarsall set (header comment).
};

// The value of `name` (ASCII case ignored) in `environment`, or nullopt (absent or unset).
[[nodiscard]] std::optional<std::string> environmentValue(const project::CMakeEnvironment& environment,
                                                          std::string_view name);

/*
 * Runs vcvarsall.bat of `instance` for `architecture` (header comment). Blocking (1-5 s); call it
 * off the UI thread. Errors: Unsupported (not Windows), InvalidArgument, NotFound (no vcvarsall.bat:
 * the instance has no C++ tools), IoError (vcvarsall failed), Timeout, the launcher's errors.
 */
[[nodiscard]] Result<MsvcDevEnvironment> captureMsvcEnvironment(io::IProcessLauncher& launcher, io::IFileSystem& fs,
                                                                const VsInstance& instance,
                                                                std::string_view architecture,
                                                                const VcvarsSelection& selection = {});

// The cached captures (header comment). `fs` and `launcher` must outlive the cache.
class MsvcEnvironmentCache {
public:
    // `cacheDirectory` (absolute) holds the files; "" keeps the cache in memory only.
    MsvcEnvironmentCache(io::IFileSystem& fs, io::IProcessLauncher& launcher, std::string cacheDirectory);

    // The environment of `instance` and `architecture`: from memory, else from the cache file when it
    // records the instance's current version, else captured and written. A failed write is ignored.
    // Errors: those of captureMsvcEnvironment.
    [[nodiscard]] Result<MsvcDevEnvironment> get(const VsInstance& instance, std::string_view architecture,
                                                 const VcvarsSelection& selection = {});

    // The cache file of `instanceId`, `architecture` and `selection`; "" without a cache directory.
    [[nodiscard]] std::string cacheFile(std::string_view instanceId, std::string_view architecture,
                                        const VcvarsSelection& selection = {}) const;

    // How many captures ran (vcvarsall started), for tests.
    [[nodiscard]] std::size_t captures() const;

private:
    io::IFileSystem& m_fs;
    io::IProcessLauncher& m_launcher;
    const std::string m_directory;
    mutable Mutex m_mutex;
    std::map<std::string, MsvcDevEnvironment> m_memory FROST_GUARDED_BY(m_mutex);
    std::size_t m_captures FROST_GUARDED_BY(m_mutex) = 0;
};

// The cache file text for `environment`, and its reader (nullopt when it does not parse).
[[nodiscard]] std::string formatMsvcEnvironmentFile(const MsvcDevEnvironment& environment);
[[nodiscard]] std::optional<MsvcDevEnvironment> parseMsvcEnvironmentFile(std::string_view text);

} // namespace frost::build
