// SPDX-License-Identifier: Apache-2.0
//
// Path helpers of the navigation models (ADR 0042): the style of an absolute path (native, or the
// other style for a MemFs in tests, as scanTree decides), joining in that style, the display form
// relative to a project root, and the file name. Pure functions; any thread.
#pragma once

#include "frost/io/path.hpp"

#include <string>
#include <string_view>

namespace frost::ui::nav {

// The style under which `path` is absolute: the native one, else the other (a MemFs of the other
// style); the native style for a path absolute under neither. O(n).
[[nodiscard]] io::PathStyle styleOf(std::string_view path) noexcept;

// `base` + separator + `name` in base's style (io::joinPath), or "" when that fails. O(n).
[[nodiscard]] std::string joinIn(std::string_view base, std::string_view name);

// The last component of `path` ('/' or '\' separated). O(n).
[[nodiscard]] std::string_view baseName(std::string_view path) noexcept;

// `path` relative to `root` with '/' separators when it lies below `root` (ASCII case ignored on
// the Windows style), else `path` unchanged. O(n).
[[nodiscard]] std::string relativeTo(std::string_view root, std::string_view path);

// The parent directory of `path` in its style ("" for a root). O(n).
[[nodiscard]] std::string_view parentOf(std::string_view path) noexcept;

} // namespace frost::ui::nav
