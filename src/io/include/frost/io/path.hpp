// SPDX-License-Identifier: Apache-2.0
//
// Path handling for frost::io: absolute-path checks, lexical normalisation and
// the Windows extended-length (\\?\) form.
//
/*
 * Paths in FrostIDE are UTF-8 strings. Every API in frost::io that touches the
 * OS takes an *absolute* path and rejects relative ones with InvalidArgument:
 * resolving against the process's current directory is a source of bugs (and,
 * for executables, of planted-binary attacks, PLAN.md section 3.5).
 *
 * The rules come in two styles so both can be unit-tested on either platform:
 *
 * PathStyle::Posix. Absolute means "starts with /". normalizePath collapses
 * repeated separators, drops "." components and a trailing separator, and
 * keeps ".." (symlinks make lexical ".." resolution unsound on POSIX).
 *
 * PathStyle::Windows. Absolute forms:
 *   C:\dir or C:/dir           drive-absolute
 *   \\server\share\dir         UNC (either separator)
 *   \\?\...                    verbatim (extended-length), taken literally
 *   \\.\...                    Win32 device namespace (pipes, devices), taken literally
 * Rejected as not absolute: "dir", "C:dir" (relative to the drive's current
 * directory), "\dir" (relative to the current drive), "\\server" without a
 * share, and the ambiguous "//?/" and "//./" spellings. normalizePath turns
 * separators into "\", collapses repeats, drops "." and resolves ".."
 * lexically, stopping at the root, as GetFullPathNameW does. Unlike
 * GetFullPathNameW it keeps trailing dots and spaces in names and does not
 * treat reserved names (CON, NUL, ...) specially: the extended form passes
 * names to the file system literally, so "a." stays "a.". Verbatim and device
 * paths are returned unchanged.
 *
 * Extended-length form (Windows). Win32 file APIs limit ordinary paths to
 * MAX_PATH (260) UTF-16 units unless both the application manifest and the
 * system-wide LongPathsEnabled switch allow more; the \\?\ prefix lifts the
 * limit (to about 32,767 units) regardless of either. frost::io therefore
 * hands every file path to Win32 in that form: "C:\a" becomes "\\?\C:\a" and
 * "\\server\share\a" becomes "\\?\UNC\server\share\a". Because the prefix
 * turns off the OS's own normalisation, the path is normalised first.
 *
 * Every function here rejects embedded NUL characters (OS APIs would silently
 * truncate the path) with InvalidArgument. All functions are pure, thread-safe
 * and O(n) in the path length.
 */
#pragma once

#include "frost/core/error.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace frost::io {

enum class PathStyle : unsigned char { Posix, Windows };

// The style of the platform we run on.
#ifdef _WIN32
inline constexpr PathStyle kNativePathStyle = PathStyle::Windows;
#else
inline constexpr PathStyle kNativePathStyle = PathStyle::Posix;
#endif

// True when `path` is absolute under `style` (see the rules above). Never fails.
[[nodiscard]] bool isAbsolutePath(std::string_view path, PathStyle style = kNativePathStyle) noexcept;

// Validates that `path` is absolute and NUL-free and returns it normalised.
// Errors: InvalidArgument for relative paths, embedded NULs or malformed roots.
[[nodiscard]] Result<std::string> normalizePath(std::string_view path, PathStyle style = kNativePathStyle);

// Windows rules: the normalised extended-length spelling of `path` ("\\?\C:\a",
// "\\?\UNC\server\share\a"); verbatim and device paths are returned unchanged.
// Portable (UTF-8 in, UTF-8 out) so the rules are testable everywhere.
// Errors: as normalizePath.
[[nodiscard]] Result<std::string> extendedLengthPath(std::string_view path);

// Appends `name` (one or more relative components) to `base` with the style's
// separator. `base` should already be normalised; `name` must be relative and
// non-empty. Errors: InvalidArgument otherwise.
[[nodiscard]] Result<std::string> joinPath(std::string_view base, std::string_view name,
                                           PathStyle style = kNativePathStyle);

// For a normalised absolute path: the parent directory, or "" for a root.
// The result views into `path`.
[[nodiscard]] std::string_view parentPath(std::string_view path, PathStyle style = kNativePathStyle) noexcept;

// For a normalised absolute path: the last component, or "" for a root.
// The result views into `path`.
[[nodiscard]] std::string_view fileName(std::string_view path, PathStyle style = kNativePathStyle) noexcept;

// Converts a std::filesystem::path to a UTF-8 string, for interop with code
// that already holds one (tests, Qt-free tools). On Windows the native UTF-16
// form must be valid (InvalidArgument otherwise); on POSIX the bytes are copied.
[[nodiscard]] Result<std::string> fromStdPath(const std::filesystem::path& path);

// The inverse of fromStdPath. Errors: InvalidArgument for ill-formed UTF-8 on Windows.
[[nodiscard]] Result<std::filesystem::path> toStdPath(std::string_view utf8);

#ifdef _WIN32
// The UTF-16 extended-length path handed to Win32 file APIs: extendedLengthPath
// converted with toWide. Errors: InvalidArgument (relative, NUL, ill-formed UTF-8).
[[nodiscard]] Result<std::wstring> toWin32Path(std::string_view path);
#endif

} // namespace frost::io
