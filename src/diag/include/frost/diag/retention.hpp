// SPDX-License-Identifier: Apache-2.0
//
// Local retention of diagnostics (PLAN.md section 2, Privacy and network: at most 30 days and
// 1 GB in total, oldest deleted first; ADR 0022, D-J).
//
/*
 * What is managed (everything else is kept and not counted):
 *   crash reports   crashpad's database, through ICrashReportStore (CrashReportDatabase's
 *                   DeleteReport; CleanDatabase(3 days) afterwards removes orphans);
 *   freeze reports  <crashes>/freezes/freeze-*.json;
 *   logs            <logs>/frost-*.log; one still open by a running frost is protected
 *                   (Windows: its deletion fails with Busy; Linux: IActiveFileProbe);
 *   traces          regular files in <traces> and up to two directory levels below it.
 * Links (symlinks, junctions) are never followed or deleted.
 *
 * The plan (planPrune): first every unprotected item older than the age limit (a future
 * modification time counts as age 0); then, while the remaining total (protected items
 * included) exceeds the size limit, the oldest unprotected item, ties broken by path.
 */
#pragma once

#include "frost/core/clock.hpp"
#include "frost/core/error.hpp"
#include "frost/diag/paths.hpp"
#include "frost/io/file_system.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace frost::diag {

struct RetentionPolicy {
    IClock::Duration maxAge = std::chrono::hours(30 * 24);
    std::uint64_t maxTotalBytes = 1'000'000'000;
};

enum class RetentionKind : std::uint8_t { CrashReport, FreezeReport, Log, Trace };

struct RetentionItem {
    RetentionKind kind = RetentionKind::Trace;
    std::string path; // File path, or the crash report's id for CrashReport.
    std::uint64_t size = 0;
    IClock::SystemTime mtime{};
    bool isProtected = false; // Never deleted (an active log); still counted.
};

// The indices of `items` to delete, in deletion order (see the header comment). Pure;
// O(n log n).
[[nodiscard]] std::vector<std::size_t> planPrune(std::span<const RetentionItem> items, IClock::SystemTime now,
                                                 const RetentionPolicy& policy);

// crashpad's report database, or a test double.
class ICrashReportStore {
public:
    struct Report {
        std::string id; // UUID text.
        std::uint64_t size = 0;
        IClock::SystemTime created{};
    };
    [[nodiscard]] virtual Result<std::vector<Report>> list() = 0;
    [[nodiscard]] virtual Result<void> remove(const std::string& id) = 0;
    // Removes files the database no longer references, older than `olderThan`.
    virtual void clean(IClock::Duration olderThan) noexcept = 0;

protected:
    ~ICrashReportStore() = default;
};

// The crashpad database at `path` (created if missing). Errors: IoError.
[[nodiscard]] Result<std::unique_ptr<ICrashReportStore, void (*)(ICrashReportStore*)>>
openCrashpadReportStore(const std::string& path);

// Whether a file is in use by a running frost (Linux: flock(LOCK_EX | LOCK_NB) fails).
class IActiveFileProbe {
public:
    [[nodiscard]] virtual bool isActive(const std::string& path) noexcept = 0;

protected:
    ~IActiveFileProbe() = default;
};

// The real probe: Linux flock; Windows never reports a file active (deletion fails with Busy).
[[nodiscard]] IActiveFileProbe& realActiveFileProbe() noexcept;

struct PruneReport {
    std::size_t considered = 0;
    std::size_t deleted = 0;
    std::uint64_t bytesFreed = 0;
    std::size_t busy = 0;     // Deletions refused because the file is in use.
    std::size_t failures = 0; // Other deletion errors (left in place).
    // The managed bytes left afterwards; items that could not be deleted still count, so this
    // may stay above the limit (the pruner then logs a warning).
    std::uint64_t remainingBytes = 0;
};

/*
 * Lists the managed items, plans and deletes. Errors: only a failure to list the crash report
 * store; unreadable directories are skipped, failed deletions counted.
 */
[[nodiscard]] Result<PruneReport> pruneDiagnostics(const DiagnosticsDirectories& directories, io::IFileSystem& fs,
                                                   ICrashReportStore& store, IActiveFileProbe& probe,
                                                   const IClock& clock, const RetentionPolicy& policy);

/*
 * The "frost-diag" thread: runs `setup` once, then pruneDiagnostics with the store `setup`
 * returned, at once and then every `period` (24 h), until stopped or destroyed (a condition
 * variable with the thread's stop_token). `setup` may do slow work first (frost starts its
 * crash handler there); a null store ends the thread without pruning. The references, and the
 * store, must outlive the pruner.
 */
class PeriodicPruner {
public:
    using Setup = std::function<ICrashReportStore*()>;

    PeriodicPruner(Setup setup, DiagnosticsDirectories directories, io::IFileSystem& fs, IActiveFileProbe& probe,
                   const IClock& clock, RetentionPolicy policy, IClock::Duration period = std::chrono::hours(24));
    PeriodicPruner(const PeriodicPruner&) = delete ("owns a thread");
    PeriodicPruner& operator=(const PeriodicPruner&) = delete ("owns a thread");
    ~PeriodicPruner(); // Requests stop and joins.

    // Prunes completed so far (for tests).
    [[nodiscard]] std::size_t runs() const noexcept;

private:
    struct State;
    std::unique_ptr<State> m_state;
    std::jthread m_thread;
};

} // namespace frost::diag
