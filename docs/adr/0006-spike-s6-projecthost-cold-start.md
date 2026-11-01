# ADR 0006: Spike S6, ProjectHost cold start

- Status: accepted (fixture, R2a and R2b). Two parts are only proposed and wait for the user: decision 6 (the
  B23 preconditions) and the final B23 value (decision 3).
- Date: 2026-10-31; revised 2026-11-01 after the independent verification (next project, B23 method,
  preconditions, units, Windows SDK).
- Plan reference: PLAN.md section 2 ("MSBuild / .vcxproj", path (b)), budget B23, D3, M0 spike S6

## Context
`FrostIDE.ProjectHost` is the C# source of truth for MSBuild evaluation of C++ projects. It runs on net472 (the VC
targets and tasks need .NET Framework MSBuild), finds VS with MSBuildLocator and talks StreamJsonRpc over stdio.
D3 allows C#.

ProjectHost's cold start is part of B23's "first project streamed" variant: p50 3 s on R2b. `budgets.json`
still notes that target as "final value set by S6", and defines B23 like this:
- method: "Open request to the trees painted, and to the first project streamed from ProjectHost (ETW
  markers)";
- preconditions: measured after trust, the trust click excluded from the timing, ProjectHost cold start included.

The interval starts and ends in FrostIDE's own open path: the open request, the native solution reader, the
supervisor and the ETW markers. That path first exists in M2, so S6 can time only ProjectHost's part of it.

## Decision
1. **Keep path (b) as planned:** net472 + Microsoft.Build.Locator + StreamJsonRpc.
   - On R2b, ProjectHost's part of B23's first-project interval takes p50 1.90 s (2026-10-31) and 2.02 s
     (2026-11-01), 20 runs each. "What the spike timed" under Measurements defines that part.
   - It is a ProjectHost-side lower bound on B23 for the same machine and cache state, not a B23 measurement.
     The M2 measurement with ETW markers decides B23.
   - At these medians about 1 s of the 3 s would remain for FrostIDE's own part of the path. Medians of the
     parts do not add exactly, so M2 times the whole interval.
2. **Pre-warm ProjectHost after L1, before trust.** As soon as a root with a solution or vcxproj opens, FrostIDE
   starts ProjectHost and has it evaluate and design-time-build a *bundled* dummy C++ project. That JITs the
   evaluator and the VC targets without running any project-supplied code, so the trust model (PLAN.md section 3.5)
   is unaffected. The trust click then pays only for the real project: on R2b, Host.EXE takes 0.45 s
   (2026-10-31) and 0.51 s (2026-11-01) after the warm-up, instead of 1.90–2.02 s.
   - Later projects cost less, but how much less depends on their project references. In a cold host, after
     Host.EXE, R2b's second project under decision 5, `Host.FeatureTests` (2 references), takes 185 ms, and `hostlib`
     (1 reference) then takes 131 ms (p50, 20 runs). Host.EXE requested a second time in a pre-warmed host still
     takes 398 ms. The first estimate here, about 0.1 s per later project, holds only for projects with few
     references.
   - If trust is given before the warm-up finishes, the real request waits behind it. Launch to the first R2b
     project then takes 1.96 s against 1.90 s for a cold host (2026-10-31), or 2.13 s against 2.02 s
     (2026-11-01), p50. In that case pre-warming costs about 60–110 ms.
3. **Propose keeping B23 at p50 3 s for "first project streamed" on R2b. Whether that value is final is the
   user's decision.**
   - ProjectHost's part is p50 1.90–2.02 s, max 2.33 s over 40 runs, with a warm file cache.
   - The rest of the interval stays unmeasured until M2, and M2 also measures it with a cold cache (decision 6).
   - `budgets.json` still says "final value set by S6". Confirming 3 s and removing that note is up to the user,
     and this ADR does not edit `budgets.json`. Any change to the target needs an ADR and user approval.
4. **MSBuild assemblies load through MSBuildLocator's resolver.** Binding them by `codeBase` to the VS copies (in
   the hope of using VS's native images) made no measurable difference.
5. **Projects stream in solution-file order, limited to C++ projects that build in the active solution
   configuration. The first such project streams first.**
   - On a first-ever open no startup project is known. The startup project is per-user state that is not stored
     in the `.sln`/`.slnx`, and FrostIDE has no session state for a new root.
   - The rule needs only the native solution reader, so the request can go out the moment trust is given. Once
     the user sets a startup project, that project streams first.
   - For R2b in Debug\|x64, `OpenConsole.slnx` gives this order:
     1. `src/host/exe/Host.EXE.vcxproj`: an Application with 14 project references, one of which is hostlib.
     2. `Host.FuzzWrapper.vcxproj`: skipped, because it has `<Build Solution="Debug|x64" Project="false" />`.
     3. `src/host/ft_host/Host.FeatureTests.vcxproj`: a DLL with 2 project references (`types`,
        `winconptylib`). It is the second project streamed.
     4. `Host.Tests.UIA.csproj`: skipped, because it is C#.
     5. `src/host/lib/hostlib.vcxproj`: a static library with 1 project reference. That reference,
        `midi.vcxproj`, comes from the shared-items project `host-common.vcxitems`, which hostlib imports.
   - The startup-project reading would also pick Host.EXE, because it is the first project and an Application,
     so the rule does not change the number.
   - Host.EXE is a demanding first project: its design-time build evaluates its 14 references.
   - B23 is measured in Debug\|x64, the developer configuration on an x64 host. The general rule for the default
     configuration is an M2 item (configuration switcher).
6. **Proposed B23 preconditions (status: proposed; the user decides).** `budgets.json` does not state these yet,
   and this ADR does not edit it. The spike assumed (a) to (c). M2 needs (d) and (e) for a well-defined
   measurement.
   - **(a)** R2b's NuGet packages are restored with the repository's own `dep\nuget\nuget.exe` (ADR 0005,
     Restore). Without the restore, command lines silently lose the WIL include directory (ADR 0005 decision 6).
   - **(b)** The active solution configuration is Debug\|x64.
   - **(c)** The first project streamed is chosen by decision 5's rule. On R2b that is Host.EXE.
   - **(d)** Trust is granted as soon as the open request is made. ProjectHost's cold start, and a pre-warm that
     is still running (decision 2), then stay inside the timed interval. Once ProjectHost is pre-warmed, that is
     what "ProjectHost cold start is included" requires. With an instant grant, the excluded trust click takes
     no time.
   - **(e)** The gating sample is cold in PLAN.md section 4's sense (after a reboot or a standby-list purge), and
     a warm-cache sample is reported alongside. All of S6's numbers are warm-cache numbers.

   If the user approves, `budgets.json`'s B23 preconditions and PLAN.md's B23 row take these over, together
   with the final value (decision 3).

## Measurements
- **Machine and toolchain.**
  - REF-W1, VS 2022 Build Tools 17.14: MSBuild 17.14.60, and MSBuildLocator reports instance 17.14.37710.0.
  - MSVC 14.44.35207 and Windows SDK 10.0.26100.0, the only SDK installed. R2b's pinned commit defaults to that
    SDK (ADR 0005).
  - .NET Framework 4.8.1 or later (release key 534296).
- **Spike host.**
  - Built with .NET SDK 10.0.403 for net472. It uses Microsoft.Build.Locator 1.9.1, Microsoft.Build 17.14.28
    (reference only, `ExcludeAssets=runtime`) and StreamJsonRpc 2.22.11.
  - The R2a and R2b runs use the first-session host plus a `DesignTime` request. That request evaluates with the
    design-time global properties of ADR 0005 decision 5 (from microsoft/msbuild-extractor-sample, MIT). It then
    builds PLAN.md's target list, `ComputeReferenceCLInput;GetProjectDirectories;GetClCommandLines`, through
    the ADR 0005 wrapper.
  - The sample also does three things the spike leaves out: it sets the global properties `VCToolsInstallDir`,
    `VCInstallDir` and `NETFXKitsDir`; it imports a fix-up props file through `ForceImportBeforeCppTargets`;
    and after evaluation it sets `WindowsTargetPlatformVersion` on the project instance with `SetProperty`.
    The spike did not need any of them: MSBuildLocator's VS instance resolved the toolset, and the projects'
    own props set the SDK version. M2 revisits them.
- **What the spike timed, against B23.**
  - Start: the driver (`s6_drive.py`) reads `perf_counter()` immediately before it creates the `S6Host.exe`
    process.
  - End: the driver has read the complete JSON-RPC response to the first `DesignTime` request from the host's
    stdout.
  - Inside the interval: process creation, CLR start, MSBuildLocator, StreamJsonRpc, the request (written to the
    host's stdin right after launch), evaluation, the design-time build, serialisation of the result and the
    pipe.
  - Outside it, all in FrostIDE and first built in M2:
    - before the request: the open request, the native solution read with decision 5's project selection,
      trust, the supervisor launching ProjectHost through `IProcessLauncher`, and building the request;
    - after the response: parsing it, adding the project to the model, streaming it into the Solution tree, and
      the ETW marker that ends B23.

    The spike's request came from a precomputed plan file.
  - The figure is therefore only ProjectHost's side of B23. Under the same machine and cache state, B23 cannot
    be shorter.
- **Method.** Each run is a fresh process with a warm file cache. It is not a post-reboot cold start; M2 measures
  that (decision 6 (e)). The driver measures from process launch to each response. The host's marks are
  milliseconds since process start.
- **Units.** Memory figures are read through Win32 in binary units: host memory in MiB, and commit headroom and
  private memory in GiB.
- **Machine load, 2026-10-31.** `lghub_agent.exe` (Logitech G HUB) held 43.3 GiB of private memory, which left
  2.1–5.2 GiB of commit headroom during the host runs. Other agents' builds (cmake, ninja, clang-cl, lld-link,
  vcpkg) ran during four of the six batches. The CPU was 23–51 % busy, sampled for 0.5 s before each run.
- **Machine load, 2026-11-01 (s26, 01:30–01:32).** The runs held the overnight measurement lock "s26".
  - Lock record: free commit 26.4 of 64.1 GiB, 10.5 GiB physical memory free, system CPU 36.8 %, and no other
    agent's build running.
  - Before each run: the CPU was 27–44 % busy, commit headroom was 26.4–27.8 GiB, and no build-named process ran.
  - The user's own processes kept running: the `E:\r11` tree (FrostEditor, FrostRuntime, FrostServices, 7.2 GiB
    private in all), Discord and Wallpaper Engine. `lghub_agent.exe` was down to 0.17 GiB.
  - Host.EXE and the warm-up took 6–13 % longer than on 2026-10-31 (cold Host.EXE 2.02 against 1.90 s), despite
    the far larger commit headroom. In-host evaluation took 4 % longer and the build 7 %. The cause was not
    isolated. hostlib, now requested third, took about the same time as before: 124–131 against 129–131 ms.

  In both sessions all numbers are upper bounds.

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

**ProjectHost's side of B23 on R2b.** Every R2b request runs in Debug\|x64 with `SolutionDir=E:\corpus\R2b\`,
after the NuGet restore described in ADR 0005. Host.EXE returns 2 groups and 3 files.

2026-10-31: hostlib was requested second here, although decision 5 puts it third.

| Measurement | Runs | p50 | max | min |
|---|---|---|---|---|
| **Cold host: launch → Host.EXE response** | 20 | **1.90 s** | 2.33 s | 1.84 s |
| … batch 1 / batch 2 | 10 / 10 | 1.89 s / 1.95 s | 2.04 s / 2.33 s | 1.84 s / 1.86 s |
| Then `hostlib` (one of Host.EXE's references; 2 groups, 44 files) | 20 | 129 ms | 144 ms | 123 ms |
| Pre-warmed: warm-up on the fixture (stand-in for the bundled dummy) | 10 | 1.51 s | 1.69 s | 1.47 s |
| Pre-warmed: Host.EXE after the warm-up (post-trust latency when trust comes after the warm-up) | 10 | 448 ms | 499 ms | 431 ms |
| Pre-warmed: launch → Host.EXE response | 10 | 1.96 s | 2.12 s | 1.91 s |
| Pre-warmed: then `hostlib` | 10 | 131 ms | 141 ms | 118 ms |

2026-11-01 (s26), with the requests in decision 5's order:

| Measurement | Runs | p50 | max | min | In-host evaluation / build, p50 |
|---|---|---|---|---|---|
| **Cold host: launch → Host.EXE response** | 20 | **2.02 s** | 2.06 s | 1.99 s | 760 / 878 ms |
| … runs 1–10 / 11–20 | 10 / 10 | 2.03 s / 2.01 s | 2.06 s / 2.06 s | 2.00 s / 1.99 s | |
| **Then `Host.FeatureTests`** (the second project streamed; 2 groups, 21 files) | 20 | **185 ms** | 199 ms | 173 ms | 90 / 91 ms |
| Then `hostlib` (the third) | 20 | 131 ms | 141 ms | 124 ms | 77 / 53 ms |
| Pre-warmed: warm-up on the fixture | 10 | 1.62 s | 1.83 s | 1.61 s | 741 / 502 ms |
| Pre-warmed: Host.EXE after the warm-up | 10 | 505 ms | 532 ms | 497 ms | 105 / 397 ms |
| Pre-warmed: launch → Host.EXE response | 10 | 2.13 s | 2.36 s | 2.11 s | |
| Pre-warmed: then `Host.FeatureTests` | 10 | 170 ms | 183 ms | 145 ms | 85 / 85 ms |
| Pre-warmed: then `hostlib` | 10 | 124 ms | 138 ms | 120 ms | 69 / 55 ms |
| Pre-warmed: then Host.EXE again | 10 | 398 ms | 407 ms | 367 ms | 97 / 301 ms |

- **Cold-host phases.** p50 in ms since process start, 2026-10-31 / 2026-11-01:

  | Phase | 2026-10-31 | 2026-11-01 |
  |---|---|---|
  | `Main` | 41 | 42 |
  | Locator | 60 | 62 |
  | StreamJsonRpc attached | 154 | 159 |
  | Request dispatched | 273 | 287 |
  | Evaluated | 1019 | 1072 |
  | Design-time build done | 1843 | 1956 |

  - Inside the host, evaluation takes 729 / 760 ms and the build 822 / 878 ms.
  - The driver's launch-to-response time is about 60 ms longer than the host's `dt-end` mark for the first
    request. Later requests start about 1 ms after the previous one ends, so the first response pays a first-use
    cost on the response path.
- **References and build time.** An evaluation-only check (`-getItem:ProjectReference` with the design-time
  properties, s26) counts the references, including those that imported files add.

  | Project (R2b, Debug\|x64) | Type | `ClCompile` | Project references | Build p50 in the pre-warmed host (s26) |
  |---|---|---|---|---|
  | Host.EXE | Application | 3 | 14, including hostlib | 397 ms first, 301 ms again |
  | Host.FeatureTests | DynamicLibrary | 21 | 2: `types`, `winconptylib` | 85 ms |
  | hostlib | StaticLibrary | 44 | 1: `midi`, from `host-common.vcxitems` | 55 ms |
  | Fixture `s2.vcxproj` | Application | 4 | 0 | 502 ms, as the warm-up (first use) |

  - `MSBuild.exe -clp:PerformanceSummary` on Host.EXE shows the `MSBuild` task taking 304 ms.
    `GetTargetPath` and `GetPublicIncludeDirectories` run 14 times each.
  - When Host.EXE is requested again, the reference-resolution code is already JIT-compiled, yet its build still
    costs 301 ms. Projects with 1–2 references cost 55–85 ms.
  - So most of Host.EXE's cost recurs on every request and goes with its 14 references; only about 0.1 s
    (397 → 301 ms) is first use. The projects also differ in type and size, so these points do not give a
    per-reference cost.
  - The 2026-10-31 split of 354 ms (Host.EXE) against 69 ms (hostlib) compares 14 references with 1, not with 0.
  - hostlib is one of Host.EXE's references, so when hostlib is requested the first request has already run the
    reference protocol on it. Each request uses a new `ProjectCollection`, so no evaluation is reused, but
    hostlib's files are in the OS cache and the code paths are JIT-compiled.
- **Memory.** Host peak working set and peak private commit:
  - R2b: 89–98 MiB and 66–78 MiB, both sessions;
  - R2a: 92–98 MiB and 72–78 MiB;
  - fixture: 75 MiB and 54 MiB.

## Consequences
- ProjectHost needs a `warmUp` request and a bundled dummy project (M2), and the supervisor starts the host on
  root open. On a machine without VS the host is not started.
  - If the dummy references a second dummy project, the warm-up also JIT-compiles the reference protocol.
  - That saves at most the first-use part of Host.EXE's build after the warm-up, about 0.1 s (397 against
    301 ms). The other 0.3 s recurs on every request. M2 measures this.
- ProjectHost's part of B23 has four phases: about 0.27–0.29 s until the first request is dispatched,
  0.73–0.76 s of evaluation, 0.82–0.88 s of design-time build and about 0.06 s until the first response arrives.
  - The fixture's cold build has no references and takes 0.46–0.50 s (the warm-up rows' build p50). So roughly
    half of the build is first-use JIT, and most of the rest is Host.EXE's references.
  - The M2 perf test reports these phases separately, next to FrostIDE's own phases.
- The M2 B23 test puts ETW markers on the open request and on the first project streamed into the Solution tree.
  That result decides B23, under the preconditions the user approves (decision 6). The S6 figures do not.
- The spike creates a new `ProjectCollection` for each request. The planned shared `EvaluationContext` should let
  the references evaluated for the first project be reused when those projects stream next; hostlib is one of
  them. The next-project figures (Host.FeatureTests 170–185 ms, hostlib 124–131 ms) are therefore upper bounds.
- **Follow-ups once the user has decided** (not done here):
  - `budgets.json`'s B23 preconditions and its "final value set by S6" note;
  - PLAN.md's B23 row;
  - R2b's Windows SDK prerequisite in PLAN.md section 5 and `tests/corpus/manifest.json`, which should read
    10.0.26100.0, not 10.0.22621 (ADR 0005).
- R2b measurements assume restored NuGet packages, the state a developer of that repository has (ADR 0005
  decision 6). Its C++/WinRT projects need the UWP workload, which is not installed here. Host.EXE,
  Host.FeatureTests and hostlib do not need it.
- These three NuGet packages (all MIT) need the licence check and an ADR when ProjectHost lands in M2. The spike
  code stays outside the repository.

## Alternatives considered
- Path (c) only (`MSBuild.exe -getTargetResult` per request): 0.33–0.41 s per process with no warm reuse
  (ADR 0005), and it cannot stream results. It stays as the fallback and the consistency check.
- A net10.0 host for C++ projects: the VC targets require .NET Framework MSBuild, so it is not possible.
- NGEN of the host at install time: helps only our own assemblies, not Microsoft.Build loaded from VS. Revisit
  in M13 if pre-warming proves insufficient.
- Pre-warming with the solution's own first project instead of a bundled dummy: it would hide the remaining
  0.45–0.51 s, but it runs the repository's MSBuild logic before trust (PLAN.md section 3.5).
