// SPDX-License-Identifier: Apache-2.0
//
// Private: white-box test hooks. Production never sets them; tests (which include this private
// header) install a function to run at a fixed point of an operation, to observe or interfere
// with the state there (for example, a second connection recording while a prune is between its
// snapshot and its deletes).
#pragma once

#include <atomic>

namespace frost::storage::detail {

/*
 * A function and its context, read with acquire on every pass. constinit, so there is no global
 * constructor. Thread-safe to install and clear; installing while the operation runs on another
 * thread is the test's own race to avoid.
 */
struct TestHook {
    std::atomic<void (*)(void*)> function{nullptr};
    std::atomic<void*> context{nullptr};

    void run() const noexcept {
        if (void (*hook)(void*) = function.load(std::memory_order_acquire); hook != nullptr) {
            hook(context.load(std::memory_order_acquire));
        }
    }
    void install(void (*hook)(void*), void* hookContext) noexcept {
        context.store(hookContext, std::memory_order_release);
        function.store(hook, std::memory_order_release);
    }
    void clear() noexcept {
        function.store(nullptr, std::memory_order_release);
        context.store(nullptr, std::memory_order_release);
    }
};

// Database::open: after sqlite3_open_v2 created or opened the file, before any lock is taken.
inline constinit TestHook g_afterSqliteOpen;
// openStore: after open() returned Corrupt, before the quarantine takes the lock.
inline constinit TestHook g_openStoreBeforeQuarantine;
// writeChunks: after each chunk row was inserted (cancellation tests request a stop here).
inline constinit TestHook g_afterChunkWritten;
// RecoveredJournal::discard: after the delete of every entry committed, before the close.
inline constinit TestHook g_discardAfterCommit;
// LocalHistory::prune: after the snapshot was loaded and its plan computed, before any delete.
inline constinit TestHook g_pruneAfterSnapshot;
// Quarantine and removal: after a VFS lock attempt found the file locked, before the sleep.
inline constinit TestHook g_vfsLockBusy;
// removeFiles: the main file is missing and a companion exists, before the placeholder is made.
inline constinit TestHook g_removeFilesMainMissing;
// Database::integrityCheck: after integrity_check passed, before foreign_key_check runs.
inline constinit TestHook g_integrityBeforeForeignKeys;

} // namespace frost::storage::detail
