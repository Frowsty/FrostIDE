// SPDX-License-Identifier: Apache-2.0
//
// Diagnostics retention (frost/diag/retention.hpp): the pure plan, the walk over the managed
// directories and the periodic pruner thread.

#include "frost/diag/retention.hpp"

#include "frost/core/log.hpp"
#include "frost/io/path.hpp"

#include "internal.hpp"

#include <algorithm>
#include <numeric>
#include <utility>

namespace frost::diag {

std::vector<std::size_t> planPrune(std::span<const RetentionItem> items, IClock::SystemTime now,
                                   const RetentionPolicy& policy) {
    std::vector<std::size_t> plan;
    std::vector<bool> gone(items.size(), false);
    std::uint64_t total = 0;
    for (const RetentionItem& item : items) {
        total += item.size;
    }
    // Age: a future modification time counts as age 0.
    for (std::size_t i = 0; i < items.size(); ++i) {
        const RetentionItem& item = items[i];
        const IClock::Duration age = item.mtime > now ? IClock::Duration::zero() : now - item.mtime;
        if (!item.isProtected && age > policy.maxAge) {
            plan.push_back(i);
            gone[i] = true;
            total -= item.size;
        }
    }
    if (total <= policy.maxTotalBytes) {
        return plan;
    }
    // Size: oldest first, ties by path (then index, so the order is total).
    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!gone[i] && !items[i].isProtected) {
            candidates.push_back(i);
        }
    }
    std::ranges::sort(candidates, [&](std::size_t a, std::size_t b) {
        if (items[a].mtime != items[b].mtime) {
            return items[a].mtime < items[b].mtime;
        }
        if (items[a].path != items[b].path) {
            return items[a].path < items[b].path;
        }
        return a < b;
    });
    for (const std::size_t index : candidates) {
        if (total <= policy.maxTotalBytes) {
            break;
        }
        plan.push_back(index);
        total -= items[index].size;
    }
    return plan;
}

namespace {

bool matches(std::string_view name, std::string_view prefix, std::string_view suffix) noexcept {
    return name.size() > prefix.size() + suffix.size() && name.starts_with(prefix) && name.ends_with(suffix);
}

// Appends the regular files of `directory` named prefix*suffix (any name when both are empty).
void collectFiles(io::IFileSystem& fs, const std::string& directory, RetentionKind kind, std::string_view prefix,
                  std::string_view suffix, int depth, std::vector<RetentionItem>& out) {
    auto entries = fs.listDirectory(directory);
    if (!entries) {
        return; // Missing or unreadable: nothing to manage there.
    }
    for (const io::DirEntry& entry : *entries) {
        auto path = io::joinPath(directory, entry.name);
        if (!path) {
            continue;
        }
        if (entry.type == io::FileType::Directory && depth > 0) {
            collectFiles(fs, *path, kind, prefix, suffix, depth - 1, out);
            continue;
        }
        // Links, devices and anything else are never touched.
        if (entry.type != io::FileType::Regular) {
            continue;
        }
        if ((!prefix.empty() || !suffix.empty()) && !matches(entry.name, prefix, suffix)) {
            continue;
        }
        auto status = fs.status(*path);
        if (!status || status->type != io::FileType::Regular) {
            continue;
        }
        out.push_back({kind, std::move(*path), status->size, status->mtime, false});
    }
}

} // namespace

Result<PruneReport> pruneDiagnostics(const DiagnosticsDirectories& directories, io::IFileSystem& fs,
                                     ICrashReportStore& store, IActiveFileProbe& probe, const IClock& clock,
                                     const RetentionPolicy& policy) {
    std::vector<RetentionItem> items;
    auto reports = store.list();
    if (!reports) {
        return std::unexpected(std::move(reports.error()).withContext("crash report database"));
    }
    for (ICrashReportStore::Report& report : *reports) {
        items.push_back({RetentionKind::CrashReport, std::move(report.id), report.size, report.created, false});
    }
    collectFiles(fs, directories.freezes, RetentionKind::FreezeReport, "freeze-", ".json", 0, items);
    const std::size_t firstLog = items.size();
    collectFiles(fs, directories.logs, RetentionKind::Log, "frost-", ".log", 0, items);
    for (std::size_t i = firstLog; i < items.size(); ++i) {
        items[i].isProtected = probe.isActive(items[i].path);
    }
    collectFiles(fs, directories.traces, RetentionKind::Trace, "", "", 2, items);

    PruneReport report;
    report.considered = items.size();
    for (const RetentionItem& item : items) {
        report.remainingBytes += item.size;
    }
    for (const std::size_t index : planPrune(items, clock.systemNow(), policy)) {
        const RetentionItem& item = items[index];
        Result<void> removed =
            item.kind == RetentionKind::CrashReport ? store.remove(item.path) : fs.removeFile(item.path);
        if (removed || removed.error().code() == ErrorCode::NotFound) {
            // NotFound: gone already (another frost pruned it), so it no longer counts.
            report.remainingBytes -= item.size;
        }
        if (removed) {
            ++report.deleted;
            report.bytesFreed += item.size;
        } else if (removed.error().code() == ErrorCode::Busy) {
            ++report.busy;
        } else if (removed.error().code() != ErrorCode::NotFound) {
            ++report.failures;
        }
    }
    // Stale entries of crashpad's new/ directory (reports never finished) and orphaned files.
    store.clean(std::chrono::hours(72));
    if (report.remainingBytes > policy.maxTotalBytes) {
        log::warn("diag", "retention: {} bytes of diagnostics remain, above the {} byte limit ({} busy, {} failed)",
                  report.remainingBytes, policy.maxTotalBytes, report.busy, report.failures);
    }
    return report;
}

struct PeriodicPruner::State {
    Setup setup;
    DiagnosticsDirectories directories;
    io::IFileSystem* fs;
    IActiveFileProbe* probe;
    const IClock* clock;
    RetentionPolicy policy;
    IClock::Duration period;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::atomic<std::size_t> runs{0};
};

PeriodicPruner::PeriodicPruner(Setup setup, DiagnosticsDirectories directories, io::IFileSystem& fs,
                               IActiveFileProbe& probe, const IClock& clock, RetentionPolicy policy,
                               IClock::Duration period)
    : m_state(new State{std::move(setup), std::move(directories), &fs, &probe, &clock, policy, period, {}, {}, {}}) {
    m_thread = std::jthread([state = m_state.get()](const std::stop_token& stop) {
        nameAndGuardThisThread("frost-diag");
        ICrashReportStore* store = state->setup ? state->setup() : nullptr;
        if (store == nullptr) {
            return;
        }
        while (!stop.stop_requested()) {
            auto pruned =
                pruneDiagnostics(state->directories, *state->fs, *store, *state->probe, *state->clock, state->policy);
            if (pruned) {
                log::info("diag", "retention: {} of {} items deleted ({} bytes), {} busy, {} failed", pruned->deleted,
                          pruned->considered, pruned->bytesFreed, pruned->busy, pruned->failures);
            } else {
                log::warn("diag", "retention: {}", pruned.error().describe());
            }
            state->runs.fetch_add(1, std::memory_order_release);
            std::unique_lock lock(state->mutex);
            (void)state->wake.wait_for(lock, stop, state->period, [] { return false; });
        }
    });
}

PeriodicPruner::~PeriodicPruner() {
    m_thread.request_stop();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

std::size_t PeriodicPruner::runs() const noexcept { return m_state->runs.load(std::memory_order_acquire); }

} // namespace frost::diag
