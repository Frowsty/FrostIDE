# ADR 0006: Spike S6, ProjectHost cold start

- Status: accepted (fixture, R2a and R2b).
- Date: 2026-10-31
- Plan reference: PLAN.md section 2 ("MSBuild / .vcxproj", path (b)), budget B23, D3, M0 spike S6

## Context
`FrostIDE.ProjectHost` is the C# source of truth for MSBuild evaluation of C++ projects. It runs on net472 (the VC
targets and tasks need .NET Framework MSBuild), finds VS with MSBuildLocator and talks StreamJsonRpc over stdio.
Its cold start is part of B23 ("first project streamed", p50 3 s on R2b, final value set by S6). B23 measures that
variant after trust: the trust click is excluded from the timing, and ProjectHost cold start is included. D3 allows
C#.

## Decision
1. **Keep path (b) as planned:** net472 + Microsoft.Build.Locator + StreamJsonRpc. On R2b the first project
   streams at p50 1.90 s from host launch, about 1.1 s inside B23.
2. **Pre-warm ProjectHost after L1, before trust.** As soon as a root with a solution or vcxproj opens, FrostIDE
   starts ProjectHost and has it evaluate and design-time-build a *bundled* dummy C++ project. That JITs the
   evaluator and the VC targets without running any project-supplied code, so the trust model (PLAN.md section 3.5)
   is unaffected. The trust click then pays only for the real project. On R2b that is 0.45 s instead of 1.90 s.
   The first estimate here, about 0.1 s, holds only for later projects of a solution: R2b's second project takes
   0.13 s. If trust is given before the warm-up finishes, the real request waits behind it. Launch to the first R2b
   project then takes 1.96 s against 1.90 s for a cold host (p50), so in that case pre-warming costs about 60 ms.
3. **Keep B23 at p50 3 s for "first project streamed" on R2b.** S6 measured p50 1.90 s (max 2.33 s, 20 runs) on a
   heavily loaded machine. The budget stays at 3 s rather than tightening, because the M2 measurement adds a
   post-reboot cold cache and the IDE's own request path. Any change to the budget needs an ADR and user approval.
4. **MSBuild assemblies load through MSBuildLocator's resolver.** Binding them by `codeBase` to the VS copies (in
   the hope of using VS's native images) made no measurable difference.
5. **The first project streamed is the solution's first C++ project, in solution-file order, that builds in the
   active solution configuration.** On a first-ever open no startup project is known: the startup project is
   per-user state that is not stored in the `.sln`/`.slnx`, and FrostIDE has no session state for a new root. The
   rule needs only the native solution reader, so the request can go out the moment trust is given. Once the user
   sets a startup project, that project streams first. For R2b both readings select `src/host/exe/Host.EXE.vcxproj`:
   it is the first project in `OpenConsole.slnx`, it is an Application, and it builds in Debug\|x64. So the choice
   does not change the number. It is also a demanding first project, because its design-time build evaluates its
   14 project references. B23 is measured in Debug\|x64, the developer configuration on an x64 host. The general
   rule for the default configuration is an M2 item (configuration switcher).

## Measurements
- **Machine and toolchain.** REF-W1, VS 2022 Build Tools 17.14 (MSBuild 17.14.60; MSBuildLocator reports instance
  17.14.37710.0), .NET Framework 4.8.1 or later (release key 534296).
- **Spike host.** Built with .NET SDK 10.0.403 for net472, using Microsoft.Build.Locator 1.9.1, Microsoft.Build
  17.14.28 (reference only, `ExcludeAssets=runtime`) and StreamJsonRpc 2.22.11. The R2a and R2b runs use the
  first-session host plus a `DesignTime` request. That request evaluates with the design-time global properties of
  ADR 0005 decision 5 (from microsoft/msbuild-extractor-sample, MIT) and builds PLAN.md's target list:
  `ComputeReferenceCLInput;GetProjectDirectories;GetClCommandLines`, through the ADR 0005 wrapper. The sample also
  sets `VCToolsInstallDir`, `VCInstallDir`, `NETFXKitsDir` and a fix-up props file. The spike did not need them,
  because MSBuildLocator's VS instance resolved the toolset; M2 revisits them.
- **Method.** Each run is a fresh process with a warm file cache: not a post-reboot cold start, which is measured
  under B23's cold conditions in M2. The driver measures from process launch to each response. The host's marks
  are milliseconds since process start.
- **Machine load.** `lghub_agent.exe` (Logitech G HUB) held 43.3 GB of private memory, which left 2.1–5.2 GB of
  commit headroom during the host runs. Other agents' builds (cmake, ninja, clang-cl, lld-link, vcpkg) ran during
  four of the six batches, and the CPU was 23–51 % busy, sampled for 0.5 s before each run. All numbers are upper
  bounds.

**Fixture (first session).** The 4-TU v143 project from ADR 0005. The request evaluates it and runs the wrapper
target `FrostGetClCommandLines`. Three agents were building in parallel.

| Phase (ms since process start, a typical run of 14) | Time |
|---|---|
| `Main` entered | 40 |
| MSBuildLocator registered VS | 60 |
| StreamJsonRpc attached | 153 |
| First request dispatched | 274 |
| Project evaluated (first evaluation in the process) | 996 |
| Design-time build done (GetClCommandLines wrapper) | 1417 |
| **End-to-end, launch → response (driver-measured, 14 runs)** | **p50 1.49 s**, max 1.69 s |
| Second request in the same process (other configuration, 8 runs) | p50 95 ms |

For comparison, `MSBuild.exe -getTargetResult` on the same fixture takes 358 ms per process (ADR 0005). That
points to JIT in our host's first evaluation (about 0.7 s), which pre-warming hides.

**S6 on R2a.** `PowerEditor/visual.net/notepadPlus.vcxproj` (97 TUs, no project references), with
`SolutionDir=PowerEditor\visual.net\`.

| Request (fresh process per run) | Runs | p50 | max | min |
|---|---|---|---|---|
| **Launch → first design-time build (Release\|x64)** | 10 | **1.60 s** | 2.30 s | 1.53 s |
| Second request, same process (Debug\|x64) | 10 | 110 ms | 122 ms | 106 ms |
| Third request, same process (Release\|x64 again) | 10 | 115 ms | 129 ms | 105 ms |
| First-session request (wrapper target only, `DesignTimeBuild=true` only): launch → response | 10 | 1.61 s | 1.92 s | 1.53 s |
| … its second request (Debug\|x64) | 10 | 125 ms | 140 ms | 115 ms |
| Control: the fixture with the first-session request, repeated today | 10 | 1.53 s | 1.76 s | 1.49 s |

R2a's first design-time build, p50 over 10 runs, in ms since process start: `Main` 40, locator 59, StreamJsonRpc
attached 156, request dispatched 280, evaluated 1021, design-time build done 1542. Inside the host, evaluation
takes 722 ms and the build 520 ms. Under the same load, R2a costs about 0.07 s more than the fixture (1.60 against
1.53 s).

**B23 on R2b.** `src/host/exe/Host.EXE.vcxproj` in Debug\|x64 (decision 5), with `SolutionDir=E:\corpus\R2b\`,
after the NuGet restore described in ADR 0005. It returns 2 groups and 3 files.

| Measurement | Runs | p50 | max | min |
|---|---|---|---|---|
| **Cold host: launch → Host.EXE streamed** | 20 | **1.90 s** | 2.33 s | 1.84 s |
| … batch 1 / batch 2 | 10 / 10 | 1.89 s / 1.95 s | 2.04 s / 2.33 s | 1.84 s / 1.86 s |
| Next project in the same host (`hostlib.vcxproj`, 44 TUs, no references) | 20 | 129 ms | 144 ms | 123 ms |
| Pre-warmed: warm-up on the fixture (stand-in for the bundled dummy) | 10 | 1.51 s | 1.69 s | 1.47 s |
| Pre-warmed: Host.EXE request after the warm-up (post-trust latency when trust comes after the warm-up) | 10 | 448 ms | 499 ms | 431 ms |
| Pre-warmed: launch → Host.EXE streamed | 10 | 1.96 s | 2.12 s | 1.91 s |
| Pre-warmed: next project (`hostlib`) | 10 | 131 ms | 141 ms | 118 ms |

- **Cold-host phases**, p50 over 20 runs, in ms since process start: `Main` 41, locator 60, StreamJsonRpc attached
  154, request dispatched 273, evaluated 1019, design-time build done 1843. Inside the host, evaluation takes
  729 ms (max 920) and the build 822 ms (max 960).
- **Where the build time goes.** An `MSBuild.exe -clp:PerformanceSummary` run of the same three targets shows the
  `MSBuild` task taking 304 ms. Host.EXE's design-time build evaluates its referenced projects: `GetTargetPath`
  and `GetPublicIncludeDirectories` run 14 times each. After the warm-up the Host.EXE build still takes 354 ms
  (p50), against 69 ms for `hostlib`, which has no references.
- **Memory.** Host peak working set and peak private commit: R2b 89–91 and 69–70 MB; R2a 92–98 and 72–78 MB;
  fixture 75 and 54 MB.

## Consequences
- ProjectHost needs a `warmUp` request and a bundled dummy project (M2), and the supervisor starts the host on
  root open. On a machine without VS the host is not started. If the dummy references a second dummy project, part
  of the 354 ms reference work may move into the warm-up; M2 measures that.
- B23 decomposes into about 0.27 s until the first request is dispatched, about 0.75 s of evaluation and about
  0.8 s of design-time build. The fixture's cold build, which has no references, takes 0.46 s (the warm-up row's
  build p50), so roughly half of the 0.8 s is first-use JIT and most of the rest is Host.EXE's references. The M2
  perf test reports these phases separately.
- The spike creates a new `ProjectCollection` for each request. The planned shared `EvaluationContext` should let
  the references evaluated for the first project be reused when those projects stream next, so the 0.13 s
  next-project figure is an upper bound.
- R2b measurements assume restored NuGet packages, the state a developer of that repository has (ADR 0005
  decision 6). Its C++/WinRT projects need the UWP workload, which is not installed here; Host.EXE does not need
  it.
- These three NuGet packages (all MIT) need the licence check and an ADR when ProjectHost lands in M2. The spike
  code stays outside the repository.

## Alternatives considered
- Path (c) only (`MSBuild.exe -getTargetResult` per request): 0.33–0.41 s per process with no warm reuse
  (ADR 0005), and it cannot stream results. It stays as the fallback and the consistency check.
- A net10.0 host for C++ projects: the VC targets require .NET Framework MSBuild, so it is not possible.
- NGEN of the host at install time: helps only our own assemblies, not Microsoft.Build loaded from VS. Revisit
  in M13 if pre-warming proves insufficient.
- Pre-warming with the solution's own first project instead of a bundled dummy: it would hide the remaining
  0.45 s, but it runs the repository's MSBuild logic before trust (PLAN.md section 3.5).
