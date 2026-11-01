// SPDX-License-Identifier: Apache-2.0
//
// Helpers shared by frost::diag's sources; not part of the module's interface.
#pragma once

#include "frost/core/clock.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace frost::diag {

// "yyyymmddThhmmssZ" of a wall-clock time, in UTC. Pure.
[[nodiscard]] std::string compactUtc(IClock::SystemTime time);

// "yyyy-mm-ddThh:mm:ss.mmmZ" of a wall-clock time, in UTC. Pure.
[[nodiscard]] std::string isoUtcMillis(IClock::SystemTime time);

// The value of environment variable `name` (UTF-8), or "" when unset.
[[nodiscard]] std::string environmentValue(std::string_view name);

// This process's id and the calling thread's OS id (Windows thread id, Linux tid).
[[nodiscard]] std::uint32_t currentProcessId() noexcept;
[[nodiscard]] std::uint64_t currentThreadId() noexcept;

/*
 * For threads frost::diag starts: names the calling thread (`name`, ASCII, at most 15
 * characters for Linux) and installs the per-thread fatal handlers (std::terminate's handler is
 * per thread in the MSVC runtime; frost/diag/fatal_handlers.hpp).
 */
void nameAndGuardThisThread(const char* name) noexcept;

} // namespace frost::diag
