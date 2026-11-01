// SPDX-License-Identifier: Apache-2.0
//
// The interface every fuzz harness implements (cmake/FrostFuzz.cmake): <name>_harness.cpp defines
// these two functions; <name>_fuzzer.cpp wraps them in libFuzzer's entry points (fuzz trees) and
// replay_main.cpp runs them over the committed corpus (normal test trees, label unit).
#pragma once

#include "frost/core/error.hpp"

#include <cstdint>
#include <span>

namespace frost::fuzz {

// One-time process setup (for example disabling crash dialogs). Called before the first input.
void initialize();

// Runs one input. Returns ErrorCode::Internal (or another error) describing a bug; a harness
// keeps no state between inputs, so each input reproduces on its own.
[[nodiscard]] Result<void> runInput(std::span<const std::uint8_t> data);

} // namespace frost::fuzz
