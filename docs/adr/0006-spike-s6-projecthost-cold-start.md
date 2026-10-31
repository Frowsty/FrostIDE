# ADR 0006: Spike S6, ProjectHost cold start

- Status: accepted (fixture). The corpora are fetched; the R2a/R2b design-time builds are pending the user's approval to execute their MSBuild logic on this machine.
- Date: 2026-10-31
- Plan reference: PLAN.md section 2 ("MSBuild / .vcxproj", path (b)), budget B23, D3, M0 spike S6

## Context
`FrostIDE.ProjectHost` is the C# source of truth for MSBuild evaluation of C++ projects. It runs on net472 (the VC
targets and tasks need .NET Framework MSBuild), finds VS with MSBuildLocator and talks StreamJsonRpc over stdio.
Its cold start is part of B23 ("first project streamed", p50 3 s on R2b, final value set by S6). D3 allows C#.

## Decision
1. **Keep path (b) as planned:** net472 + Microsoft.Build.Locator + StreamJsonRpc. The numbers below fit inside
   B23.
2. **Pre-warm ProjectHost after L1, before trust.** As soon as a root with a solution or vcxproj opens, FrostIDE
   starts ProjectHost and has it evaluate and design-time-build a *bundled* dummy C++ project. That JITs the
   evaluator and the VC targets without running any project-supplied code, so the trust model (PLAN.md section 3.5)
   is unaffected. The trust click then pays only the real project's evaluation, about 0.1 s for a small project
   once warm, instead of about 1.5 s.
3. **Keep B23 at p50 3 s for "first project streamed" on R2b.** It is re-checked when R2b is measured; any change
   to the budget needs an ADR and user approval.
4. **MSBuild assemblies load through MSBuildLocator's resolver.** Binding them by `codeBase` to the VS copies (in
   the hope of using VS's native images) made no measurable difference.

## Measurements
REF-W1, VS 2022 Build Tools 17.14 (MSBuild 17.14.60), .NET Framework 4.8.1 runtime. The spike host was built with
.NET SDK 10.0.403 for net472: Microsoft.Build.Locator 1.9.1, Microsoft.Build 17.14.28 (reference only,
`ExcludeAssets=runtime`), StreamJsonRpc 2.22.11. Each run is a fresh process with a warm file cache: not a
post-reboot cold start, which is measured under B23's cold conditions in M2. Three agents were building in
parallel. Fixture: the 4-TU v143 project from ADR 0005; the request evaluates it and runs the wrapper target
`FrostGetClCommandLines`.

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

## Consequences
- ProjectHost needs a `warmUp` request and a bundled dummy project (M2), and the supervisor starts the host on
  root open. On a machine without VS the host is not started.
- B23 decomposes into roughly 0.3 s to ready, plus the evaluation and design-time build of the first project. The
  M2 perf test reports both phases separately.
- These three NuGet packages (all MIT) need the licence check and an ADR when ProjectHost lands in M2. The spike
  code stays outside the repository.

## Alternatives considered
- Path (c) only (`MSBuild.exe -getTargetResult` per request): 0.36 s per process with no warm reuse, and it
  cannot stream results. It stays as the fallback and the consistency check.
- A net10.0 host for C++ projects: the VC targets require .NET Framework MSBuild, so it is not possible.
- NGEN of the host at install time: helps only our own assemblies, not Microsoft.Build loaded from VS. Revisit
  in M13 if pre-warming proves insufficient.
