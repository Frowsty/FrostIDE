// SPDX-License-Identifier: Apache-2.0
//
// The compiled theme blob (.ftheme, format v1): the encoder the build-time compiler uses, the
// validating decoder that loads a theme at start-up without any JSON parsing, and the file
// loaders over IFileSystem (ADR 0015 decision 4). Qt-free.
//
/*
 * Layout, little-endian, every multi-byte read through memcpy (no alignment requirement):
 *
 *   Header, 48 bytes
 *     [0,8)   magic "FROSTTHM"
 *     [8,10)  formatVersion = 1
 *     [10,12) flags: bit 0 dark, bit 1 highContrast; other bits 0
 *     [12,16) reserved, 0
 *     [16,24) registryHash = kRegistryHash of the compiler that wrote it
 *     [24,26) colorCount  [26,28) metricCount  [28,30) roleCount  (the registry's counts)
 *     [30,32) idLength    [32,34) nameLength   [34,36) reserved, 0
 *     [36,40) totalSize (bytes, header included)
 *     [40,48) checksum: FNV-1a 64 over bytes [0,40) followed by [48,totalSize)
 *   Body
 *     u32 ARGB x colorCount, f32 x metricCount x 2 (normal, compact), u32 ARGB x roleCount,
 *     then the id bytes and the name bytes (UTF-8, no terminator).
 *
 * A blob is about 530 bytes (48 header, 456 body arrays, the id and the name; the shipped ones
 * are 528 to 546). Any single changed byte changes the FNV-1a checksum (see
 * fnv1a.hpp), so every one-byte corruption is detected.
 */
#pragma once

#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"
#include "frost/ui/model/theme.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace frost::ui {

inline constexpr std::uint16_t kThemeBlobVersion = 1;
inline constexpr std::size_t kThemeBlobHeaderSize = 48;
// Files above this size are refused before they are read (a blob is about 530 bytes).
inline constexpr std::uint64_t kMaxThemeFileBytes = 64 * 1024;
// The file name suffix of compiled themes: <themes dir>/<id>.ftheme.
inline constexpr std::string_view kThemeFileSuffix = ".ftheme";

// Encodes `data` as a v1 blob. Deterministic (equal data, equal bytes). Precondition: `data`
// passes Theme::fromData's checks (a contract violation otherwise). O(size).
[[nodiscard]] std::vector<std::byte> encodeThemeBlob(const ThemeData& data);

/*
 * Decodes and validates a v1 blob. Every field is checked before use. Errors:
 *   Corrupt  a size below 48 or other than totalSize; a wrong magic; counts other than the
 *            registry's; non-zero reserved fields or flag bits; inconsistent lengths; a bad
 *            checksum; NaN, infinity or out-of-range metrics; a compact slot that differs from
 *            normal for a key without hasCompact (or exceeds it for a Px key); a translucent
 *            opaque key; an invalid id; a name that is empty, too long or not UTF-8.
 *   Unsupported  formatVersion != 1, or a registry hash other than kRegistryHash ("theme blob
 *            compiled against another key registry; rebuild").
 * Pure and thread-safe. O(size). Budget: B1.
 */
[[nodiscard]] Result<Theme> decodeThemeBlob(std::span<const std::byte> blob);

/*
 * Loads a compiled theme file: status() first (a file over kMaxThemeFileBytes is Corrupt), then
 * readFile and decodeThemeBlob. A blob whose embedded id differs from `expectedId` is Corrupt.
 * Every io error (NotFound, PermissionDenied, Busy, InvalidArgument, IoError) is propagated with
 * the path as context. Thread-safe as far as `fs` is. Budget: B1.
 */
[[nodiscard]] Result<Theme> loadThemeFile(io::IFileSystem& fs, std::string_view absolutePath,
                                          std::string_view expectedId);

/*
 * Lists the themes in `themesDir` (every regular "<id>.ftheme" file whose blob decodes and whose
 * embedded id matches its file name), sorted by id. Files that fail are skipped; when `skipped`
 * is given, their errors are appended to it. For the theme picker (m1/shell-nav), never for the
 * start-up path. Errors: those of listDirectory. O(files x blob size).
 */
[[nodiscard]] Result<std::vector<ThemeInfo>> listInstalledThemes(io::IFileSystem& fs, std::string_view themesDir,
                                                                 std::vector<Error>* skipped = nullptr);

} // namespace frost::ui
