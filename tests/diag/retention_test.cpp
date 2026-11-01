// SPDX-License-Identifier: Apache-2.0
//
// Retention (frost/diag/retention.hpp) without the disk: planPrune's invariants over random
// item sets (no kept removable item over the age limit; the total at most the cap unless only
// protected items remain; size deletions strictly oldest first; minimality: keeping any one
// item deleted for size would break the cap) and golden tables; pruneDiagnostics over MemFs
// with a crash-report store double and an active-file probe double: aged and oversized
// fixtures through the virtual clock, links never followed (or even passed to status()),
// unknown files kept, active files protected, missing roots tolerated; and PeriodicPruner's
// thread.

#include "frost/core/clock.hpp"
#include "frost/diag/retention.hpp"
#include "frost/io/mem_fs.hpp"
#include "frost/io/path.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using namespace std::chrono_literals;
using frost::diag::RetentionItem;
using frost::diag::RetentionKind;
using frost::diag::RetentionPolicy;
using SystemTime = frost::IClock::SystemTime;

const SystemTime kNow = SystemTime(std::chrono::sys_days(std::chrono::year(2026) / 11 / 1).time_since_epoch());

RetentionItem item(std::string path, std::uint64_t size, std::chrono::hours age, bool isProtected = false) {
    return {RetentionKind::Log, std::move(path), size, kNow - age, isProtected};
}

std::vector<std::string> names(const std::vector<RetentionItem>& items, const std::vector<std::size_t>& plan) {
    std::vector<std::string> out;
    for (const std::size_t index : plan) {
        out.push_back(items[index].path);
    }
    return out;
}

TEST(PlanPrune, GoldenTables) {
    RetentionPolicy policy;
    policy.maxTotalBytes = 60;
    // Age first (in item order), then oldest first for size, ties by path. 145 bytes in all;
    // 135 after the age step, 95 after c, 55 after b.
    const std::vector<RetentionItem> items{
        item("a", 10, 31 * 24h),       // Aged.
        item("d", 40, 24h),            // Same age as b; b sorts first, and after b the total fits.
        item("b", 40, 24h),            // Size, second oldest.
        item("c", 40, 48h),            // Size, oldest.
        item("e", 10, 40 * 24h, true), // Protected: never deleted, still counted.
        item("f", 5, -24h),            // From the future: age 0, so the youngest.
    };
    EXPECT_EQ(names(items, frost::diag::planPrune(items, kNow, policy)), (std::vector<std::string>{"a", "c", "b"}));
    policy.maxTotalBytes = 100; // 95 after c already fits.
    EXPECT_EQ(names(items, frost::diag::planPrune(items, kNow, policy)), (std::vector<std::string>{"a", "c"}));
    // Exactly the age limit is kept; one second over is deleted.
    const std::vector<RetentionItem> edge{item("x", 1, 30 * 24h),
                                          {RetentionKind::Trace, "y", 1, kNow - 30 * 24h - 1s, false}};
    EXPECT_EQ(names(edge, frost::diag::planPrune(edge, kNow, RetentionPolicy{})), (std::vector<std::string>{"y"}));
    // Only protected items over the cap: nothing to do.
    const std::vector<RetentionItem> stuck{item("p", 500, 24h, true)};
    EXPECT_TRUE(frost::diag::planPrune(stuck, kNow, policy).empty());
    // The default cap is 10^9 bytes (Q6: the stricter reading of "1 GB").
    EXPECT_EQ(RetentionPolicy{}.maxTotalBytes, 1'000'000'000u);
    EXPECT_EQ(RetentionPolicy{}.maxAge, 30 * 24h);
}

TEST(PlanPrune, InvariantsHoldOnRandomItemSets) {
    std::mt19937_64 random(1101);
    for (int round = 0; round < 2000; ++round) {
        RetentionPolicy policy;
        policy.maxAge = std::chrono::hours(1 + random() % 1000);
        policy.maxTotalBytes = random() % 5000;
        std::vector<RetentionItem> items;
        const std::size_t count = random() % 40;
        for (std::size_t i = 0; i < count; ++i) {
            items.push_back(item(std::format("i{:02}", random() % 30), random() % 600,
                                 std::chrono::hours(static_cast<long>(random() % 1500) - 100), random() % 7 == 0));
        }
        const std::vector<std::size_t> plan = frost::diag::planPrune(items, kNow, policy);
        std::set<std::size_t> deleted(plan.begin(), plan.end());
        ASSERT_EQ(deleted.size(), plan.size()) << "an item planned twice";
        const auto ageOf = [&](std::size_t i) {
            return items[i].mtime > kNow ? frost::IClock::Duration::zero() : kNow - items[i].mtime;
        };
        std::uint64_t kept = 0;
        bool removableKept = false;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (deleted.contains(i)) {
                ASSERT_FALSE(items[i].isProtected) << "a protected item was planned";
                continue;
            }
            kept += items[i].size;
            if (!items[i].isProtected) {
                removableKept = true;
                ASSERT_LE(ageOf(i), policy.maxAge) << "a kept removable item is over the age limit";
            }
        }
        ASSERT_TRUE(kept <= policy.maxTotalBytes || !removableKept) << "over the cap with removable items kept";
        /* Size deletions: those within the age limit. Each is older than (or as old as, and
           before by path) every kept removable item, so they are the oldest removable items;
           and that prefix is the shortest that fits: keeping its youngest member would break
           the cap (oldest-first deletion is minimal among oldest-first prefixes). */
        std::optional<std::size_t> youngest;
        for (const std::size_t d : deleted) {
            if (ageOf(d) > policy.maxAge) {
                continue;
            }
            for (std::size_t k = 0; k < items.size(); ++k) {
                if (deleted.contains(k) || items[k].isProtected) {
                    continue;
                }
                ASSERT_TRUE(items[d].mtime < items[k].mtime ||
                            (items[d].mtime == items[k].mtime && items[d].path <= items[k].path))
                    << "a younger item was deleted for size before an older one was";
            }
            // The planner breaks full ties by index, so the last one deleted is the highest
            // index among equals (`deleted` iterates by increasing index).
            if (!youngest ||
                std::tie(items[d].mtime, items[d].path) >= std::tie(items[*youngest].mtime, items[*youngest].path)) {
                youngest = d;
            }
        }
        if (youngest) {
            ASSERT_GT(kept + items[*youngest].size, policy.maxTotalBytes) << "a size deletion that was not needed";
        }
    }
}

// A crash-report store double.
class FakeStore final : public frost::diag::ICrashReportStore {
public:
    std::map<std::string, Report> reports;
    std::vector<std::string> removed;
    int cleans = 0;

    frost::Result<std::vector<Report>> list() override {
        std::vector<Report> out;
        for (const auto& [id, report] : reports) {
            out.push_back(report);
        }
        return out;
    }
    frost::Result<void> remove(const std::string& id) override {
        removed.push_back(id);
        if (reports.erase(id) == 0) {
            return frost::fail(frost::ErrorCode::NotFound, id);
        }
        return {};
    }
    void clean(frost::IClock::Duration) noexcept override { ++cleans; }
};

class FakeProbe final : public frost::diag::IActiveFileProbe {
public:
    std::set<std::string> active;
    bool isActive(const std::string& path) noexcept override { return active.contains(path); }
};

/*
 * MemFs plus link entries: listDirectory() also reports each entry of `links` as a Symlink,
 * and any status() or removeFile() on a link fails the test (links are never followed or
 * deleted); a removeFile of a path in `busy` fails with Busy (a Windows log still open).
 */
class LinkingFs final : public frost::io::IFileSystem {
public:
    explicit LinkingFs(frost::io::MemFs& inner) : m_inner(inner) {}
    std::set<std::string> links;
    std::set<std::string> busy;

    frost::Result<std::string> readFile(std::string_view path) override { return m_inner.readFile(path); }
    frost::Result<void> writeFileAtomic(std::string_view path, std::string_view data) override {
        return m_inner.writeFileAtomic(path, data);
    }
    frost::Result<frost::io::FileStatus> status(std::string_view path) override {
        EXPECT_FALSE(links.contains(std::string(path))) << "status() followed the link " << path;
        return m_inner.status(path);
    }
    frost::Result<std::vector<frost::io::DirEntry>> listDirectory(std::string_view path) override {
        auto entries = m_inner.listDirectory(path);
        if (entries) {
            addLinks(path, *entries);
        }
        return entries;
    }
    // The same entries as listDirectory(), with MemFs's issues (ADR 0020).
    frost::Result<frost::io::DirListing> listDirectoryWithDiagnostics(std::string_view path) override {
        auto listing = m_inner.listDirectoryWithDiagnostics(path);
        if (listing) {
            addLinks(path, listing->entries);
        }
        return listing;
    }
    frost::Result<void> createDirectories(std::string_view path) override { return m_inner.createDirectories(path); }
    frost::Result<void> removeFile(std::string_view path) override {
        EXPECT_FALSE(links.contains(std::string(path))) << "removeFile() on the link " << path;
        if (busy.contains(std::string(path))) {
            return frost::fail(frost::ErrorCode::Busy, std::string(path));
        }
        return m_inner.removeFile(path);
    }
    frost::Result<void> removeDirectory(std::string_view path) override { return m_inner.removeDirectory(path); }
    frost::Result<bool> exists(std::string_view path) override { return m_inner.exists(path); }

private:
    // Appends each entry of `links` whose parent is `path`, as a Symlink.
    void addLinks(std::string_view path, std::vector<frost::io::DirEntry>& entries) const {
        for (const std::string& link : links) {
            if (frost::io::parentPath(link) == path) {
                entries.push_back({std::string(frost::io::fileName(link)), frost::io::FileType::Symlink});
            }
        }
    }

    frost::io::MemFs& m_inner;
};

constexpr bool kWindows = frost::io::kNativePathStyle == frost::io::PathStyle::Windows;

struct PruneRig {
    frost::ManualClock clock{frost::IClock::SteadyTime{}, kNow};
    frost::io::MemFs mem{clock};
    LinkingFs fs{mem};
    FakeStore store;
    FakeProbe probe;
    frost::diag::DiagnosticsDirectories dirs =
        frost::diag::diagnosticsDirectories(kWindows ? "C:\\cache" : "/cache").value();

    PruneRig() { mem.addRoot(kWindows ? "C:\\" : "/"); }

    std::string file(const std::string& dir, std::string_view name, std::size_t bytes) {
        const std::string path = frost::io::joinPath(dir, name).value();
        EXPECT_TRUE(mem.createDirectories(frost::io::parentPath(path)).has_value());
        EXPECT_TRUE(mem.writeFileAtomic(path, std::string(bytes, 'x')).has_value());
        return path;
    }
    bool has(const std::string& path) { return mem.exists(path).value_or(false); }
};

TEST(PruneDiagnostics, PrunesAgedAndOversizedFixtures) {
    PruneRig rig;
    // Written 40 days before "now" (the virtual clock then moves on).
    const std::string oldFreeze = rig.file(rig.dirs.freezes, "freeze-1.json", 100);
    const std::string oldLog = rig.file(rig.dirs.logs, "frost-1.log", 100);
    const std::string activeLog = rig.file(rig.dirs.logs, "frost-2.log", 100);
    const std::string busyLog = rig.file(rig.dirs.logs, "frost-3.log", 100);
    const std::string unknown = rig.file(rig.dirs.logs, "readme.txt", 100);
    const std::string almostLog = rig.file(rig.dirs.logs, "frost-.log", 100); // Empty middle: not ours.
    const std::string trace = rig.file(rig.dirs.traces, "a/b/t.etl", 100);
    const std::string deepTrace = rig.file(rig.dirs.traces, "a/b/c/t.etl", 100);
    rig.store.reports["old-report"] = {"old-report", 100, kNow};
    rig.clock.advance(38 * 24h);
    const std::string midFreeze = rig.file(rig.dirs.freezes, "freeze-2.json", 400);
    rig.clock.advance(24h);
    const std::string newFreeze = rig.file(rig.dirs.freezes, "freeze-3.json", 400);
    rig.store.reports["new-report"] = {"new-report", 400, rig.clock.systemNow()};
    rig.clock.advance(24h);
    rig.probe.active.insert(activeLog);
    rig.fs.busy.insert(busyLog);
    rig.fs.links.insert(frost::io::joinPath(rig.dirs.logs, "frost-9.log").value());
    rig.fs.links.insert(frost::io::joinPath(rig.dirs.traces, "a").value() + (kWindows ? "\\junction" : "/junction"));

    RetentionPolicy policy;
    policy.maxTotalBytes = 1000;
    auto report = frost::diag::pruneDiagnostics(rig.dirs, rig.fs, rig.store, rig.probe, rig.clock, policy);
    ASSERT_TRUE(report.has_value()) << report.error().describe();
    /* Aged: the old report, freeze-1, frost-1, frost-3 (busy: left in place, still counted) and
       the trace. Then 400 (freeze-2) + 400 (freeze-3) + 400 (new report) + 100 (active) = 1300 >
       1000 (the planned frost-3 no longer counts in the plan): the oldest removable item,
       freeze-2, goes, and 900 fit. On disk 1000 remain with the busy log. */
    EXPECT_FALSE(rig.has(oldFreeze));
    EXPECT_FALSE(rig.has(oldLog));
    EXPECT_FALSE(rig.has(trace));
    EXPECT_FALSE(rig.has(midFreeze));
    EXPECT_TRUE(rig.has(busyLog));
    EXPECT_TRUE(rig.has(activeLog));
    EXPECT_TRUE(rig.has(unknown));
    EXPECT_TRUE(rig.has(almostLog));
    EXPECT_TRUE(rig.has(deepTrace));
    EXPECT_TRUE(rig.has(newFreeze));
    EXPECT_FALSE(rig.store.reports.contains("old-report"));
    EXPECT_TRUE(rig.store.reports.contains("new-report"));
    EXPECT_EQ(report->deleted, 5u);
    EXPECT_EQ(report->bytesFreed, 800u);
    EXPECT_EQ(report->busy, 1u);
    EXPECT_EQ(report->failures, 0u);
    EXPECT_EQ(rig.store.cleans, 1);
    EXPECT_EQ(report->remainingBytes, 1000u); // The busy log still counts.
    EXPECT_EQ(report->considered, 9u);
}

TEST(PruneDiagnostics, ToleratesMissingRoots) {
    PruneRig rig;
    auto report = frost::diag::pruneDiagnostics(rig.dirs, rig.fs, rig.store, rig.probe, rig.clock, RetentionPolicy{});
    ASSERT_TRUE(report.has_value()) << report.error().describe();
    EXPECT_EQ(report->considered, 0u);
    EXPECT_EQ(report->deleted, 0u);
}

TEST(PeriodicPruner, RunsSetupThenPrunesAndStopsPromptly) {
    PruneRig rig;
    rig.file(rig.dirs.logs, "frost-1.log", 100);
    rig.clock.advance(31 * 24h);
    std::atomic<int> setups{0};
    {
        const frost::diag::PeriodicPruner pruner(
            [&]() -> frost::diag::ICrashReportStore* {
                ++setups;
                return &rig.store;
            },
            rig.dirs, rig.fs, rig.probe, rig.clock, RetentionPolicy{}, 24h);
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (pruner.runs() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_EQ(pruner.runs(), 1u);
        // The destructor stops the 24 h wait at once.
    }
    EXPECT_EQ(setups.load(), 1);
    EXPECT_TRUE(rig.mem.listDirectory(rig.dirs.logs)->empty());
    // A setup that returns no store ends the thread without pruning.
    const frost::diag::PeriodicPruner idle([] { return nullptr; }, rig.dirs, rig.fs, rig.probe, rig.clock,
                                           RetentionPolicy{});
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(idle.runs(), 0u);
}

} // namespace
