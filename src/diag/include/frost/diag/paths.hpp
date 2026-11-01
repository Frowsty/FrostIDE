// SPDX-License-Identifier: Apache-2.0
//
// Where frost keeps its diagnostics: the cache root and the crash, freeze, log and trace
// directories below it (PLAN.md section 2, the Privacy and network row; ADR 0022, D-M).
//
/*
 *   <cache>/crashes/crashpad   crashpad's report database (minidumps)
 *   <cache>/crashes/freezes    freeze reports (freeze-<UTC>-<pid>-<seq>.json)
 *   <cache>/logs               log files (frost-<UTC>-<pid>-<n>.log)
 *   <cache>/traces             ETW, Perfetto and Tracy traces (written by later milestones)
 *
 * The cache root is %LOCALAPPDATA%\FrostIDE\caches on Windows and
 * $XDG_CACHE_HOME/FrostIDE (or $HOME/.cache/FrostIDE) on Linux.
 * TODO(M1): adopt the shared cache-root resolver once the settings slice provides one; this one
 * follows the same rules.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/path.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace frost::diag {

// Looks up one environment variable: its value, or nullopt when it is unset.
using EnvironmentLookup = std::function<std::optional<std::string>(std::string_view name)>;

/*
 * The cache root from the environment, normalised. Windows rules (`style` Windows):
 * LOCALAPPDATA must be an absolute drive or UNC path (a drive-relative, relative or \\.\ device
 * value is refused), then "FrostIDE\caches" is appended. POSIX rules: XDG_CACHE_HOME when it is
 * set to an absolute path (a relative value is ignored, as the XDG base directory specification
 * says), else HOME, which must be absolute, with ".cache" appended; then "FrostIDE".
 * Errors: NotFound (the variable is unset or empty), InvalidArgument (not absolute, or a NUL).
 * Pure; thread-safe if `lookup` is.
 */
[[nodiscard]] Result<std::string> resolveCacheRoot(const EnvironmentLookup& lookup, io::PathStyle style);

// resolveCacheRoot() with this process's environment and the native rules.
[[nodiscard]] Result<std::string> resolveCacheRoot();

// The directories below a cache root (see the header comment). Paths only; nothing is created.
struct DiagnosticsDirectories {
    std::string cacheRoot;
    std::string crashes;
    std::string crashpadDatabase;
    std::string freezes;
    std::string logs;
    std::string traces;
};

// Joins the layout onto `cacheRoot` (normalised, absolute). Errors: InvalidArgument. Pure.
[[nodiscard]] Result<DiagnosticsDirectories> diagnosticsDirectories(std::string_view cacheRoot,
                                                                    io::PathStyle style = io::kNativePathStyle);

/*
 * Creates crashes/, crashes/crashpad/, crashes/freezes/ and logs/ (and missing parents). On
 * POSIX crashes/ and logs/ are created with mode 0700 and checked afterwards: each must be a
 * real directory (not a symlink); one that is group- or other-writable is refused unless this
 * user owns it, in which case its mode is narrowed to 0700. Errors: PermissionDenied (an unsafe
 * directory), AlreadyExists (a non-directory in the way), IoError.
 */
[[nodiscard]] Result<void> prepareDiagnosticsDirectories(const DiagnosticsDirectories& directories);

} // namespace frost::diag
