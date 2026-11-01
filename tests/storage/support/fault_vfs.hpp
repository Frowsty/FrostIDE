// SPDX-License-Identifier: Apache-2.0
//
// FaultVfs: a shim SQLite VFS for the storage tests. It delegates every method to the platform
// VFS (win32-longpath on Windows, the default unix VFS elsewhere), records the names xOpen sees,
// counts xWrite, xSync and xTruncate calls, and fails the Nth of them with a chosen result code,
// once or from then on. Written from SQLite's VFS documentation (sqlite.org/c3ref/vfs.html and
// io_methods.html); SQLite's test_vfs.c (public domain) shaped the approach.
//
/*
 * Use: FaultVfs& vfs = FaultVfs::instance(); vfs.reset(); vfs.failAt(...);
 * then OpenOptions::vfs = FaultVfs::kName (or JournalOptions::vfs). The VFS is registered once,
 * never as the default. Counters and the plan are process-wide atomics: tests that use it must
 * not run in parallel threads of one process (gtest runs tests sequentially).
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace frost::storage::test {

class FaultVfs {
public:
    static constexpr const char* kName = "frost-fault";

    enum class Op : unsigned char { Write, Sync, Truncate };

    // Registers the shim on first use (after initialising frost's SQLite configuration).
    static FaultVfs& instance();

    // Clears the counters, the fault plan, the file-type filter and the recorded names.
    void reset();
    /*
     * Fails the call of `op` whose 0-based index (counted since reset, over files whose type is
     * in the filter) is `index` with `code`, and with `persistent` every later call of `op` too.
     * One plan per op; a new call replaces it.
     */
    void failAt(Op op, std::int64_t index, int code, bool persistent = false);
    // Restricts counting and faults to file types (SQLITE_OPEN_MAIN_DB, _WAL, _MAIN_JOURNAL, ...).
    void setTypeFilter(int typeMask) { m_typeMask.store(typeMask); }

    [[nodiscard]] std::int64_t count(Op op) const { return m_counts[index(op)].load(); }
    // Whether a planned fault was returned at least once since reset.
    [[nodiscard]] bool fired() const { return m_fired.load(); }
    // Every name xOpen received since reset (temporary files have none and are skipped).
    [[nodiscard]] std::vector<std::string> openedNames() const;

    // Called by the shim methods.
    [[nodiscard]] int check(Op op, int fileType);
    void recordOpen(const char* name);

private:
    FaultVfs() = default;
    static constexpr std::size_t index(Op op) { return static_cast<std::size_t>(op); }

    std::atomic<std::int64_t> m_counts[3] = {0, 0, 0};
    std::atomic<std::int64_t> m_failIndex[3] = {-1, -1, -1};
    std::atomic<int> m_failCode[3] = {0, 0, 0};
    std::atomic<bool> m_persistent[3] = {false, false, false};
    std::atomic<int> m_typeMask{0x7FFFFF00};
    std::atomic<bool> m_fired{false};
    mutable std::mutex m_namesMutex;
    std::vector<std::string> m_names;
};

} // namespace frost::storage::test
