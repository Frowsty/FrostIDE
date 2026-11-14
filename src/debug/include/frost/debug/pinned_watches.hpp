// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "frost/core/error.hpp"
#include "frost/io/file_system.hpp"

#include <string>
#include <string_view>
#include <vector>
namespace frost::debug {
// Personal workspace expressions only; no adapter references, evaluated values or data IDs persist.
[[nodiscard]] Result<std::vector<std::string>> loadPinnedWatches(io::IFileSystem& fs, std::string_view root);
[[nodiscard]] Result<void> savePinnedWatches(io::IFileSystem& fs, std::string_view root,
                                             const std::vector<std::string>& expressions);
} // namespace frost::debug
