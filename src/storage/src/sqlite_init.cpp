// SPDX-License-Identifier: Apache-2.0
//
// Private: one-time SQLite configuration and the linked-build check (see sqlite_init.hpp).

#include "sqlite_init.hpp"

#include "sqlite_error.hpp"

#include <format>
#include <mutex>
#include <utility>

#include <sqlite3.h>

namespace frost::storage::detail {

namespace {

// The outcome of the one-time initialisation. Written once inside call_once, which orders the
// write before every later reader; read-only afterwards. constinit: no global constructor.
constinit std::once_flag g_initOnce;
constinit ErrorCode g_initError = ErrorCode::Unknown; // Unknown = success.
constinit const char* g_initMessage = nullptr;        // Static text describing a failure.
constinit int g_initCode = SQLITE_OK;

void initializeOnce() noexcept {
    if (!checkLinkedSqlite()) {
        g_initError = ErrorCode::Unsupported;
        g_initMessage = "the linked SQLite is not the pinned build";
        return;
    }
    // Must precede sqlite3_initialize. SQLITE_MISUSE here would mean someone initialised
    // SQLite already; that is harmless (only the global allocation counter stays on).
    static_cast<void>(sqlite3_config(SQLITE_CONFIG_MEMSTATUS, 0));
    if (const int rc = sqlite3_initialize(); rc != SQLITE_OK) {
        g_initError = errorCodeFromSqlite(rc, 0);
        g_initMessage = "sqlite3_initialize failed";
        g_initCode = rc;
    }
}

} // namespace

Result<void> checkLinkedSqlite() {
    if (sqlite3_libversion_number() != kPinnedSqliteVersionNumber) {
        return fail(ErrorCode::Unsupported,
                    std::format("SQLite {} is linked; FrostIDE pins 3.54.1 (ADR 0014)", sqlite3_libversion()));
    }
    if (sqlite3_threadsafe() == 0) {
        return fail(ErrorCode::Unsupported, "the linked SQLite was built with SQLITE_THREADSAFE=0");
    }
    if (sqlite3_compileoption_used("OMIT_LOAD_EXTENSION") == 0) {
        return fail(ErrorCode::Unsupported,
                    "the linked SQLite allows extension loading (no SQLITE_OMIT_LOAD_EXTENSION)");
    }
    if (sqlite3_compileoption_used("OMIT_JSON") == 0) {
        return fail(ErrorCode::Unsupported, "the linked SQLite includes JSON support (no SQLITE_OMIT_JSON)");
    }
    return {};
}

Result<void> initializeSqlite() {
    std::call_once(g_initOnce, initializeOnce);
    if (g_initError == ErrorCode::Unknown) {
        return {};
    }
    if (g_initError == ErrorCode::Unsupported) {
        // Recompute the detailed message (cheap, and it names the linked version).
        auto check = checkLinkedSqlite();
        if (!check) {
            return std::unexpected(std::move(check.error()));
        }
    }
    return fail(g_initError, std::format("{} (sqlite {} {})", g_initMessage, g_initCode, sqliteCodeName(g_initCode)));
}

} // namespace frost::storage::detail
