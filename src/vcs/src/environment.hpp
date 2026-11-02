// SPDX-License-Identifier: Apache-2.0
//
// frost::vcs's private helpers: reading one environment variable as UTF-8 (GetEnvironmentVariableW
// on Windows) and ASCII case folding for path keys.
#pragma once

#include <string>
#include <string_view>

namespace frost::vcs::detail {

// The value of the environment variable `name` (UTF-8), or "" when it is unset or unreadable.
[[nodiscard]] std::string environmentVariable(const char* name);

// `text` with A-Z folded to a-z.
[[nodiscard]] std::string asciiLower(std::string_view text);

} // namespace frost::vcs::detail
