// SPDX-License-Identifier: Apache-2.0
//
// Private: one-time SQLite library configuration and the run-time check that the linked library
// is the pinned build (ADR 0014).
#pragma once

#include "frost/core/error.hpp"

namespace frost::storage::detail {

// The pinned build: SQLite 3.54.1 (overlay port third_party/vcpkg-ports/sqlite3).
inline constexpr int kPinnedSqliteVersionNumber = 3054001;

/*
 * Configures and initialises SQLite once per process (std::call_once; no global constructor):
 * SQLITE_CONFIG_MEMSTATUS off (otherwise every SQLite allocation takes a global mutex), then
 * sqlite3_initialize. Every later call returns the first call's result. It also checks the
 * linked library: version 3.54.1, thread-safe, compiled with SQLITE_OMIT_LOAD_EXTENSION and
 * SQLITE_OMIT_JSON; a mismatch is Unsupported (it catches a build that bypassed the overlay
 * port). Thread-safe. TODO(M1): route SQLITE_CONFIG_LOG to frost logging once core has it.
 */
[[nodiscard]] Result<void> initializeSqlite();

// The build check alone (no initialisation), for build_test. Errors: Unsupported.
[[nodiscard]] Result<void> checkLinkedSqlite();

} // namespace frost::storage::detail
