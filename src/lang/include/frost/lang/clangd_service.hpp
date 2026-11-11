// SPDX-License-Identifier: Apache-2.0
//
// ClangdService resource-governor integration (FND-18) and background-index metrics (CPP-4, ADR
// 0084): governed clangd launch flags (-j, platform background-index priority,
// --pch-storage=disk), backgroundIndexProgress lifecycle tracking, shard counting in
// .cache/clangd/index, and peak clangd RSS sampling via frost::sched::ResourceGovernor.
#pragma once

#include "frost/lang/clangd.hpp"
#include "frost/sched/resource_governor.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace frost::lang {

/*
 * Snapshot of clangd background indexing metrics (CPP-4, ADR 0084): whether at least one
 * backgroundIndexProgress token reached its "end" notification, elapsed indexing duration in
 * milliseconds, peak clangd process working set / RSS sampled via ResourceGovernor, number of
 * `.idx` shard files written under `.cache/clangd/index`, and the configured governor RSS cap.
 */
struct IndexMetrics {
    bool completed = false;
    std::uint64_t elapsedMs = 0;
    std::uint64_t peakRssBytes = 0;
    // Whether this snapshot obtained the running process's OS lifetime peak successfully.
    // A failed latest observation preserves the historical peak and exposes its error.
    bool peakRssSampleValid = false;
    std::string peakRssSampleError;
    std::size_t shardCount = 0;
    std::uint64_t governorRssCapBytes = 0;
    // Ceiling verified by the successful launcher contract for the current process round.
    // Independent of private/committed allocations and later governor changes; empty without a process.
    std::optional<std::uint64_t> residentMemoryLimitBytes;
};

// Attaches `governor` to `service` (call before `service.start()`). When attached, `ClangdService`
// sizes `-j=<N>` via `governor->clangdJobs()` when `ClangdLaunchOptions::jobs == 0`, passes
// the platform's background-index priority and `--pch-storage=disk`, registers a
// `HeavyLegKind::BackgroundIndex` activity scope during `backgroundIndexProgress`, and samples
// `clangd` process RSS into `governor` and `indexMetrics(service)`.
void setResourceGovernor(ClangdService& service, std::shared_ptr<sched::ResourceGovernor> governor) noexcept;

// Returns the `ResourceGovernor` attached to `service`, or nullptr if none was attached.
[[nodiscard]] std::shared_ptr<sched::ResourceGovernor> resourceGovernor(const ClangdService& service) noexcept;

// Returns a thread-safe snapshot after synchronously querying current/OS peak memory (CPP-4, ADR 0084).
[[nodiscard]] IndexMetrics indexMetrics(const ClangdService& service) noexcept;

} // namespace frost::lang
