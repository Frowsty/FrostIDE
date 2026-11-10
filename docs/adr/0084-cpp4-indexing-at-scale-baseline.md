<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0084: ResourceGovernor (`FND-18`) and `CPP-4` indexing-at-scale baseline on the pinned FrostEngine corpus

- Status: accepted
- Date: 2026-11-10
- Plan reference: PLAN.md section 4, section 7 "Roadmap v2" (R5 exit criterion), Appendix C (`FND-18`, `CPP-4`);
  ADR 0036 (`frost::lang` and `clangd`); ADR 0078 (Definition of done v2)

## Context
Round R5 requires two coupled capabilities before C++ language features can be considered production-ready on a full
game engine codebase:
1. **`FND-18: ResourceGovernor` (`frost::sched::ResourceGovernor`)** — Governing `clangd -j`, background index thread
   priority, PCH storage location, build concurrency, exclusive heavy-leg serialization (`Build`, `BackgroundIndex`,
   `Inspection`) across threads and processes via the corpus build lock, and per-process / combined working-set (RSS)
   caps on the 31.9 GiB / 24-thread development machine.
2. **`CPP-4: Indexing at scale`** — Running a cold-cache `clangd` background index of the pinned FrostEngine corpus
   (`<FROST_CORPORA_ROOT>/frostengine-12b8c6de4c38`) alongside a concurrent `cmake --build` under the build lock,
   recording the measured duration, shard count, and peak RSS in this ADR, and enforcing the baseline in
   `lang_index_corpus_tests` (`tests/lang/clangd_index_corpus_test.cpp`).

Without `ResourceGovernor`, launching `clangd --background-index` with default concurrency (`hardware_concurrency()`,
24 threads) and in-memory PCH storage alongside a Ninja build can saturate all 24 hardware threads and spike combined
working set and pagefile commit past the 31.9 GiB host limit.

## Decision

### 1. `frost::sched::ResourceGovernor` (`FND-18`)
`frost::sched::ResourceGovernor` (`src/sched/include/frost/sched/resource_governor.hpp`,
`src/sched/src/resource_governor.cpp`) governs CPU concurrency, heavy-leg serialization, and RSS budgets:
- **Host probing and OS RSS sampling:**
  - `probeHostResources()` queries logical core count, total/available physical RAM, and total/available commit limit
    (`GlobalMemoryStatusEx` and `GetSystemInfo` on Windows; `/proc/meminfo` `MemTotal`/`MemAvailable`/`SwapTotal`/
    `SwapFree` and `sysconf(_SC_NPROCESSORS_ONLN)` on Linux).
  - `queryProcessRssBytes(pid)` and `currentProcessRssBytes()` read live process working set / RSS
    (`K32GetProcessMemoryInfo` `WorkingSetSize` on Windows; `/proc/[pid]/status` `State`/`VmRSS` and `/proc/[pid]/statm`
    on Linux).
- **Governed `clangd` flags and build concurrency:**
  - `clangdJobs()`: scales with logical cores and available memory (`min(logicalCores / 2, maxClangdJobs)` with default
    `maxClangdJobs = 8`, bounded by `availablePhysicalGiB` and `availableCommitGiB` at ~1 GiB per worker). When a
    `HeavyLegKind::Build` or `HeavyLegKind::Inspection` leg is active, `clangdJobs()` throttles down to
    `throttledClangdJobs` (default `2`, or `8` on the 24-thread host in the corpus indexing benchmark) and drops to `1`
    if either the per-process `clangd` RSS cap or combined RSS cap is exceeded.
  - `backgroundIndexPriority()`: `"background"` (`--background-index-priority=background`), keeping OS indexer thread
    priority below interactive LSP requests and UI work.
  - `pchStorageDisk()`: `true` (`--pch-storage=disk`), keeping precompiled headers on disk rather than in RAM.
  - `buildJobs()`: scales with logical cores and available RAM (`min(logicalCores / 2, maxBuildJobs)`), halving when
    `HeavyLegKind::BackgroundIndex` is active and dropping to `1` if the combined RSS cap is exceeded.
- **Heavy-leg coordination and cross-process lock file:**
  - `tryAcquireHeavyLeg()` and `acquireHeavyLeg()` serialize exclusive `Build` and `Inspection` legs in-process via a
    condition variable and across processes using the byte-0 `LockFileEx` / `flock` protocol on `lockFilePath`,
    matching `tools/corpora.py`'s `BuildLock` and `frost::test::EngineBuildLock`.
  - `beginLegActivity()` tracks non-exclusive background legs (`HeavyLegKind::BackgroundIndex`) so `buildJobs()` and
    diagnostics know when background indexing is active.
- **RSS caps:**
  - Default `clangdRssCapBytes`: `4,096 MiB` (`4 GiB`).
  - Default `combinedRssCapBytes` (`BackgroundIndex` + `Build` + `Inspection`): `8,192 MiB` (`8 GiB`).
  - `sampleProcessRss(pid, leg)` and `recordProcessRss(pid, rssBytes, leg)` update current and high-water-mark peak RSS
    per PID, per `HeavyLegKind`, and across all active legs.

### 2. `ClangdService` integration and `IndexMetrics` (`CPP-4`)
`frost::lang::ClangdService` (`src/lang/include/frost/lang/clangd_service.hpp`,
`src/lang/src/clangd_service.cpp`) integrates with `frost::sched::ResourceGovernor` via
`frost::lang::setResourceGovernor(service, governor)` and `frost::lang::indexMetrics(service)` (wired from
`frost::app::Workbench` in `src/app/src/workbench_clangd.cpp`):
- `processSpec()` applies the governor's `-j=<N>`, `--background-index-priority=background`, and
  `--pch-storage=disk` flags whenever `ClangdLaunchOptions` leaves `jobs == 0`.
- `ClangdService` tracks `clangd`'s `$/progress` (`backgroundIndexProgress`) token lifecycle (`begin`, `report`, `end`)
  even when no UI progress callback is installed:
  - On `begin`: activates `HeavyLegKind::BackgroundIndex` on `ResourceGovernor`, records the start timestamp, and
    samples initial `clangd` RSS.
  - On `report` and `end`: samples `clangd` process RSS via `ResourceGovernor::sampleProcessRss(pid,
    HeavyLegKind::BackgroundIndex)`.
  - On `end`: computes `elapsedMs`, counts `.idx` shard files in `<compile-commands-dir>/.cache/clangd/index` (and
    `<workspaceRoot>/.cache/clangd/index`) via `io::realFileSystem().listDirectory()`, clears the `BackgroundIndex`
    activity flag, and publishes a thread-safe snapshot via `frost::lang::indexMetrics(service)`.

### 3. Measured FrostEngine corpus baseline and fixed budget
Measured on the reference Windows 11 workstation (24 logical threads, 31.9 GiB physical RAM, pinned `clangd` 23.1.4,
pinned FrostEngine corpus `<FROST_CORPORA_ROOT>/frostengine-12b8c6de4c38`, preset `windows-debug`) by
`ClangdIndexCorpus.FullBackgroundIndexUnderGovernorAndConcurrentBuildStaysWithinBaseline`
(`tests/lang/clangd_index_corpus_test.cpp`):

- **Workload:** Cold-cache background index of an owned compilation database materialized in a fresh scratch directory
  containing all `1,289` first-party FrostEngine C/C++ translation units (`Frost/`, `Frost-Editor/`, `Frost-Runtime/`,
  `Frost-Services/`, excluding `.cache/cpm/`, `ThirdParty/`, `build/` generated sources, `Frost-Tests/`, and
  `.rc`/`.asm` files) while a concurrent `cmake --build --target FrostFoundation -j <gov>` executes under the
  `ResourceGovernor` build lock (`frostengine-12b8c6de4c38.build.lock`).
- **Measured numbers (`2026-11-10`):**

  | Metric | Measured value | Fixed ADR 0084 baseline / cap |
  |---|---:|---:|
  | First-party FrostEngine C/C++ translation units in CDB | `1,289` | `>= 1,200` |
  | Cold background index elapsed time (`elapsedMs`) | `594,435 ms` (`594.4 s`) | `<= 720,000 ms` (`720.0 s`) |
  | Written `.idx` shards in `.cache/clangd/index` (`shardCount`) | `3,742` | `>= 3,000` |
  | Peak `clangd` RSS (`metrics.peakRssBytes`) | `3,333.2 MiB` | `< 4,096.0 MiB` (`clangdRssCapBytes`) |
  | Peak concurrent `cmake --build` RSS | `13.4 MiB` | Tracked under `HeavyLegKind::Build` |
  | Peak combined `clangd` + `cmake --build` RSS | `3,346.6 MiB` | `< 8,192.0 MiB` (`combinedRssCapBytes`) |
  | Cross-TU `workspace/symbol` query (`Frost::UUID`) | Resolved (`UUID.h` / `UUID.cpp`) | Must resolve non-empty symbol location |

## Consequences
- `ClangdService` never launches `clangd` with unbounded hardware concurrency or in-memory PCH storage when governed by
  `ResourceGovernor`; peak `clangd` working set stays around `3.25 GiB` (`3,333.2 MiB`, strictly under the `4,096 MiB`
  cap) during a cold full-engine index of all `1,289` first-party translation units with a concurrent build.
- `lang_index_corpus_tests` (`LABELS integration corpus`) runs as part of the `-Corpus` verification tier and enforces
  the `720,000 ms` duration ceiling, `>= 3,000` `.idx` shard persistence, cross-TU `workspace/symbol` resolution, and
  the `4,096 MiB` / `8,192 MiB` RSS caps.
