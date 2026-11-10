// SPDX-License-Identifier: Apache-2.0
//
// ResourceGovernor (FND-18, PLAN.md Appendix C): hardware probing (logical cores, physical and
// commit RAM, per-process working set / RSS), governed clangd and build concurrency flags
// (-j=<N>, --background-index-priority=background, --pch-storage=disk), one-heavy-leg-at-a-time
// coordination with an in-process lock and cross-process file lock (compatible with the corpus
// build lock), and peak RSS tracking with cap enforcement.
#pragma once

#include "frost/core/error.hpp"
#include "frost/core/mutex.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace frost::sched {

struct HostResources {
    unsigned logicalCores = 1;
    std::uint64_t totalPhysicalBytes = 0;
    std::uint64_t availablePhysicalBytes = 0;
    std::uint64_t totalCommitBytes = 0;
    std::uint64_t availableCommitBytes = 0;
};

// Queries the host's logical CPU count and physical/commit memory limits.
[[nodiscard]] HostResources probeHostResources() noexcept;

// Queries the resident set size (working set on Windows, VmRSS on Linux) of this process.
[[nodiscard]] std::uint64_t currentProcessRssBytes() noexcept;

// Queries the resident set size (working set on Windows, VmRSS on Linux) of child/external `pid`.
// Errors: InvalidArgument (`pid <= 0`), NotFound (no such running process), PermissionDenied, IoError.
[[nodiscard]] Result<std::uint64_t> queryProcessRssBytes(std::int64_t pid) noexcept;

enum class HeavyLegKind : std::uint8_t {
    Build = 0,
    BackgroundIndex = 1,
    Inspection = 2,
};

[[nodiscard]] std::string_view toString(HeavyLegKind kind) noexcept;

struct ResourceGovernorOptions {
    // Default 4096 MiB RSS cap for clangd background indexing / semantic engine.
    std::uint64_t clangdRssCapBytes = 4096ULL * 1024ULL * 1024ULL;
    // Default 8192 MiB combined RSS cap across concurrent index + build processes.
    std::uint64_t combinedRssCapBytes = 8192ULL * 1024ULL * 1024ULL;
    // Upper bound on clangd -j when no heavy build leg is active.
    unsigned maxClangdJobs = 8;
    // Throttled clangd -j when a heavy build/inspection leg is active or memory is constrained.
    unsigned throttledClangdJobs = 2;
    // Upper bound on parallel build jobs (-j).
    unsigned maxBuildJobs = 8;
    // Optional cross-process lock file path (locks byte 0 with LockFileEx on Windows, flock on Linux).
    std::string lockFilePath;
    // Optional deterministic hardware snapshot for tests; nullopt probes the live host.
    std::optional<HostResources> hostOverride;
};

class ResourceGovernor;

// Move-only RAII lease for an exclusive heavy leg (in-process + optional cross-process lock file).
class [[nodiscard]] HeavyLegLease {
public:
    HeavyLegLease() noexcept = default;
    HeavyLegLease(HeavyLegLease&& other) noexcept;
    HeavyLegLease& operator=(HeavyLegLease&& other) noexcept;
    HeavyLegLease(const HeavyLegLease&) = delete ("owns an exclusive heavy-leg lock");
    HeavyLegLease& operator=(const HeavyLegLease&) = delete ("owns an exclusive heavy-leg lock");
    ~HeavyLegLease();

    [[nodiscard]] bool held() const noexcept { return m_governor != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return held(); }
    [[nodiscard]] HeavyLegKind kind() const noexcept { return m_kind; }
    void release() noexcept;

private:
    friend class ResourceGovernor;
    HeavyLegLease(ResourceGovernor* governor, HeavyLegKind kind, std::uintptr_t osHandle) noexcept;

    ResourceGovernor* m_governor = nullptr;
    HeavyLegKind m_kind = HeavyLegKind::Build;
    std::uintptr_t m_osHandle = ~static_cast<std::uintptr_t>(0);
};

// Move-only RAII token that records an active heavy leg (e.g., clangd $/progress indexing) without
// taking the exclusive cross-process build lock.
class [[nodiscard]] ScopedLegActivity {
public:
    ScopedLegActivity() noexcept = default;
    ScopedLegActivity(ScopedLegActivity&& other) noexcept;
    ScopedLegActivity& operator=(ScopedLegActivity&& other) noexcept;
    ScopedLegActivity(const ScopedLegActivity&) = delete ("tracks one active leg scope");
    ScopedLegActivity& operator=(const ScopedLegActivity&) = delete ("tracks one active leg scope");
    ~ScopedLegActivity();

    [[nodiscard]] bool active() const noexcept { return m_governor != nullptr; }
    void release() noexcept;

private:
    friend class ResourceGovernor;
    ScopedLegActivity(ResourceGovernor* governor, HeavyLegKind kind) noexcept;

    ResourceGovernor* m_governor = nullptr;
    HeavyLegKind m_kind = HeavyLegKind::BackgroundIndex;
};

class ResourceGovernor {
public:
    explicit ResourceGovernor(ResourceGovernorOptions options = {});
    ~ResourceGovernor() = default;
    ResourceGovernor(const ResourceGovernor&) = delete ("coordinates host-wide heavy legs");
    ResourceGovernor& operator=(const ResourceGovernor&) = delete ("coordinates host-wide heavy legs");

    [[nodiscard]] const ResourceGovernorOptions& options() const noexcept { return m_options; }

    // Current host resources (returns the test override when set, otherwise probes the live OS).
    [[nodiscard]] HostResources hostResources() const noexcept;
    void setHostResourcesForTesting(std::optional<HostResources> overrideResources);

    // Governed clangd launch flags and build concurrency.
    [[nodiscard]] unsigned clangdJobs() const noexcept;
    [[nodiscard]] std::string_view backgroundIndexPriority() const noexcept { return "background"; }
    [[nodiscard]] bool pchStorageDisk() const noexcept { return true; }
    [[nodiscard]] std::vector<std::string> clangdFlags() const;
    [[nodiscard]] unsigned buildJobs() const noexcept;

    // Exclusive heavy-leg coordination (one exclusive heavy leg at a time, plus cross-process lock
    // file when options().lockFilePath is configured).
    [[nodiscard]] Result<HeavyLegLease> tryAcquireHeavyLeg(HeavyLegKind kind);
    [[nodiscard]] Result<HeavyLegLease> acquireHeavyLeg(HeavyLegKind kind,
                                                        std::chrono::milliseconds timeout = std::chrono::minutes(30));

    // Non-blocking activity tracking (e.g. clangd background index progress running alongside a build).
    [[nodiscard]] ScopedLegActivity beginLegActivity(HeavyLegKind kind);
    void setLegActive(HeavyLegKind kind, bool active);

    [[nodiscard]] bool isLegActive(HeavyLegKind kind) const noexcept;
    [[nodiscard]] std::optional<HeavyLegKind> activeHeavyLeg() const noexcept;
    [[nodiscard]] unsigned activeLegCount() const noexcept;

    // Process RSS sampling and cap enforcement.
    void recordProcessRss(std::int64_t pid, std::uint64_t rssBytes, HeavyLegKind leg = HeavyLegKind::BackgroundIndex);
    Result<std::uint64_t> sampleProcessRss(std::int64_t pid, HeavyLegKind leg = HeavyLegKind::BackgroundIndex);
    void unregisterProcess(std::int64_t pid);
    void resetRssTracking();

    [[nodiscard]] std::uint64_t currentProcessRssBytes(std::int64_t pid) const;
    [[nodiscard]] std::uint64_t peakProcessRssBytes(std::int64_t pid) const;
    [[nodiscard]] std::uint64_t peakLegRssBytes(HeavyLegKind leg) const;
    [[nodiscard]] std::uint64_t currentCombinedRssBytes() const;
    [[nodiscard]] std::uint64_t peakCombinedRssBytes() const;

    [[nodiscard]] std::uint64_t clangdRssCapBytes() const noexcept { return m_options.clangdRssCapBytes; }
    [[nodiscard]] std::uint64_t combinedRssCapBytes() const noexcept { return m_options.combinedRssCapBytes; }

    [[nodiscard]] bool isProcessOverRssCap(std::int64_t pid) const;
    [[nodiscard]] bool isCombinedOverRssCap() const;

private:
    friend class HeavyLegLease;
    friend class ScopedLegActivity;

    void releaseHeavyLeg(HeavyLegKind kind, std::uintptr_t osHandle) noexcept;
    void endLegActivity(HeavyLegKind kind) noexcept;

    struct ProcessRssEntry {
        HeavyLegKind leg = HeavyLegKind::BackgroundIndex;
        std::uint64_t currentBytes = 0;
        std::uint64_t peakBytes = 0;
        bool active = true;
    };

    ResourceGovernorOptions m_options;
    mutable Mutex m_mutex;
    std::condition_variable m_cv;
    std::optional<HostResources> m_hostOverride FROST_GUARDED_BY(m_mutex);
    std::optional<HeavyLegKind> m_exclusiveLeg FROST_GUARDED_BY(m_mutex);
    std::array<unsigned, 3> m_legActivityCount FROST_GUARDED_BY(m_mutex){0, 0, 0};
    std::map<std::int64_t, ProcessRssEntry> m_processRss FROST_GUARDED_BY(m_mutex);
    std::array<std::uint64_t, 3> m_peakLegRssBytes FROST_GUARDED_BY(m_mutex){0, 0, 0};
    std::uint64_t m_peakCombinedRssBytes FROST_GUARDED_BY(m_mutex) = 0;
};

} // namespace frost::sched
