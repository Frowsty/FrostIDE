// SPDX-License-Identifier: Apache-2.0
//
// Reads one variable of this process's environment (wide API on Windows), for the CMake presets'
// $env{} and $penv{} macros. Private to frost::project.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace frost::project::detail {

// The variable's UTF-8 value, or nullopt when it is unset (or not representable). Thread-safe as
// long as nobody changes the environment concurrently, which FrostIDE never does after start-up.
[[nodiscard]] std::optional<std::string> processEnvironmentVariable(std::string_view name);

} // namespace frost::project::detail
