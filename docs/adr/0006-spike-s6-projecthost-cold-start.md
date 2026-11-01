# ADR 0006: Spike S6, ProjectHost cold start

- Status: accepted (fixture, R2a and R2b), the three proposed parts included. **Decided 2026-11-01 (ADR 0011,
  B1–B4)**, on the user's instruction of that day to follow the recommendation: decision 3 takes option (i), with
  the cold samples collected passively, one per ordinary reboot (B1), and its trees proposal is accepted (B2);
  decision 6's five preconditions are accepted (B3); decision 2's question takes answer 2 as M2's target, with
  answer 3 as the fallback and answer 1 rejected (B4). D3 stays not final until the ten cold samples are in.
  Decision 2's options against first-use JIT stay open M2 options. Until then the status read:
  accepted (fixture, R2a and R2b), apart from three parts that are only proposed and wait for the user:
  how B23's final values are set, which also decides when D3 becomes final (decision 3); the B23 preconditions
  (decision 6); and whether a trusted root launches ProjectHost before L1 (decision 2). Decision 1 keeps path (b)
  as M2's plan but does not make D3 final. Decision 2's options against first-use JIT are open M2 options, not
  decisions. S6 has no cold sample, so it cannot set the first-project value on its own, and it does not cover
  the trees value.
- Date: 2026-10-31; revised 2026-11-01 after the independent verification (next project, B23 method,
  preconditions, units, Windows SDK), after its re-verification (the open path under an instant trust grant, the
  missing cold sample, the trees target, wording), after the confirmation (the start rule's scope, first-use
  JIT with no warm-up, D3, the path to L1, figures), and after the third verification (connection tracing, the
  wait for L1, the tree fill, the start rule's title, NGEN, the trees proposal, path (c) caveats, wording; raw
  data moved out of the session scratchpad into `E:\FrostBuilds\perf\m0\s2-s6\`).
- Plan reference: PLAN.md section 2 ("MSBuild / .vcxproj", paths (b) and (c)), budget B23, D3, M0 spike S6

## Context
`FrostIDE.ProjectHost` is the C# source of truth for MSBuild evaluation of C++ projects. It runs on net472 (the VC
targets and tasks need .NET Framework MSBuild), finds VS with MSBuildLocator and talks StreamJsonRpc over stdio.
D3 allows C# ("use C# if necessary"). PLAN.md's M0 spike list orders the two: S6 "fixes B23 before D3 is final"
(decision 3).

ProjectHost's cold start is part of B23's "first project streamed" variant: p50 3 s on R2b. `budgets.json`
defines B23 like this:
- method: "Open request to the trees painted, and to the first project streamed from ProjectHost (ETW
  markers)";
- preconditions: measured after trust, the trust click excluded from the timing, ProjectHost cold start included.

`budgets.json` puts the note "final value set by S6" on both B23 targets: the trees on R3 (p50 1.5 s) and the
first project streamed on R2b (p50 3 s). PLAN.md's row reads "p50 1.5 s / 3 s (final value set by S6)". S6
covers only the second. The trees use the native readers and no ProjectHost, so S6 has no evidence for 1.5 s
(decision 3).

The interval starts and ends in FrostIDE's own open path: the open request, the native solution reader, the
supervisor and the ETW markers. That path first exists in M2, so S6 can time only ProjectHost's part of it.

## Decision
1. **Keep path (b) as planned:** net472 + Microsoft.Build.Locator + StreamJsonRpc.
   - This keeps path (b) as M2's plan. It does not make D3 final: PLAN.md has S6 fix B23 before D3 is final, and
     B23's final values are still open (decision 3).
   - On R2b, ProjectHost's part of B23's first-project interval takes p50 1.90 s (2026-10-31) and 2.02 s
     (2026-11-01), 20 runs each, max 2.33 s over the 40 runs, all with a warm file cache. "What the spike timed"
     under Measurements defines that part.
   - It is an expected ProjectHost-side lower bound on B23 for the same machine and cache state. It is neither a
     B23 measurement nor a guaranteed bound; "What the spike timed" gives the conditions it rests on. The M2
     measurement with ETW markers decides B23.
   - This cold-host figure, not the pre-warmed one, is the one that applies under the proposed gating condition.
     Under decision 6 (d) trust is granted at the open request, so decision 2 launches ProjectHost without a
     warm-up and, as proposed, without waiting for L1 (that part is the user's question in decision 2).
   - At the warm-cache medians, 0.98 s (2026-11-01) to 1.10 s (2026-10-31) of the 3 s remain; after the slowest
     run (2.33 s), 0.67 s. That remainder has to cover all of FrostIDE's part ("Outside it" under Measurements):
     the open request, the native solution read and project selection, the launch, the slowdown that the tree
     fill causes the host by running beside it, and the ingest after the response. The fill runs in parallel, so
     its own duration does not count as long as the fill finishes before the host's response. B23 ends when the
     first project is streamed into the Solution tree, and the Solution view is part of the fill, so a
     Solution-view fill that outlasts the response can add up to its remaining time (parsing the response and
     adding the project to the model can overlap with it). A File System scan still running then only slows
     the path down. Nothing measured R2b's tree fill. If the launch waits for L1, the wait counts too. A cold
     cache shrinks the remainder further (decision 3). Medians of the parts do not add exactly, so M2 times the
     whole interval.
2. **On an open whose first project has no valid cached record: pre-warm ProjectHost while trust is pending in a
   root that FrostIDE has no session state for (a first-ever open); skip the warm-up once trust is granted.**
   - **Scope.** This start rule applies only to an open in which the first project (decision 5) has no valid
     ProjectModelCache record, for example a first-ever open of a new root, the case B23 measures.
     - A warm reopen of an unchanged tree follows PLAN.md section 3.4 instead: the fingerprints are validated
       natively, L1 is reached with zero .NET processes, and ProjectHost is not started in either trust state,
       not even for the warm-up (B3, T14).
     - A root that stays restricted never gets a record, because ProjectHost evaluates none of its projects. So
       the warm-up also needs a root that FrostIDE has no session state for, as on a first-ever open. Reopening a
       root that is still restricted starts no .NET process, and a trust click on such a reopen pays the
       cold-host figure; that case is outside B23.
     - When a reopen finds only later projects' records invalid, PLAN.md section 3.4 re-evaluates just those.
       When that host starts is outside B23, and M2 sets it.
   - **Trust pending.** On a first-ever open of a restricted root with a solution or vcxproj, FrostIDE starts
     ProjectHost once L1 is reached, which keeps the host's work off the path to L1. It has the host evaluate and
     design-time-build a *bundled* dummy C++ project. That JITs the evaluator and the VC targets without running
     any project-supplied code, so the trust model (PLAN.md section 3.5) is unaffected. A trust click after the
     warm-up then pays only for the real project: on R2b, Host.EXE takes 0.45 s (2026-10-31) and 0.50 s
     (2026-11-01) after the warm-up, instead of 1.90–2.02 s.
   - **Trust before the host has started.** Trust can come first through a trusted parent (PLAN.md section 3.5),
     a trust click before L1, or decision 6 (d)'s instant grant. FrostIDE then runs no warm-up. The supervisor
     launches ProjectHost once trust is granted and decision 5's rule has picked the first project, and that
     project is the host's first request. As proposed, the launch does not wait for L1; whether it may come
     before L1 is the user's question below. ProjectHost's part of B23 is then the cold-host figure of
     decision 1.
   - **Before L1 in a trusted root (proposed; the user decides).** Launching at once has costs that the B23
     figures do not show:
     - It puts about 2 s of ProjectHost work (process start, JIT, evaluation and the design-time build;
       decision 1) on the path to L1, beside the tree fill. The spike did not record the host's CPU time, and it
       did not measure the host's effect on L1 or on the tree fill.
     - It runs the project's MSBuild logic before L1, and evaluation can reach the network, for example when it
       resolves an MSBuild project SDK through NuGet. PLAN.md rules out network I/O before L1 (section 2.1) and
       tests it by recording connections (section 2.2). S6 did not trace connections, so it has no evidence
       either way for its own runs.
     - R2b's own MSBuild logic did reach the network during S2. Without the design-time properties, a
       `GetClCommandLines` run on `TerminalAppLib` built the referenced `types.vcxproj`. Its vcpkg manifest
       install downloaded tool archives and cloned vcpkg's registry: 552 MB in `%LOCALAPPDATA%\vcpkg`, together
       with the tools extracted from the archives (ADR 0005 decision 5 and Consequences). That was a build of a
       reference, not an evaluation. With the properties, the logged `MSBuild.exe` run on Host.EXE, which also
       references `types.vcxproj`, ran no vcpkg target.
     - PLAN.md section 3.4's cold-start flow lists ProjectHost as the step after the tree fill.

     There are three answers, and each needs the user:
     1. Launch before L1, with PLAN.md's network rule changed to exempt the trusted project's own MSBuild logic.
        That is a plan change.
     2. Launch before L1, but keep evaluation off the network until L1 and defer a project that needs it.
        Nobody has built or measured that.
     3. Wait for L1. That keeps PLAN.md's rule and order, but it puts the time to L1 on B23's first-project path,
        where it uses up some or all of decision 1's remainder (0.98–1.10 s at the warm medians). If the time to
        L1 on R2b is as long as the remainder, 3 s cannot be met even at those medians. No M0 spike measured that
        time on R2b.

     Whichever answer is chosen, M2 measures the host's effect on L1 and on the tree fill.

     **Decided 2026-11-01 (ADR 0011, B4):** answer 2 is M2's target; answer 3 is the fallback if answer 2 proves
     infeasible; answer 1 is rejected. PLAN.md does not change now.
   - **First-use JIT with no warm-up.** When trust comes before the host starts, which includes the gating case
     under 6 (d), no warm-up runs. All of the host's first-use JIT is then on B23's first-project path, and the
     warm-up does nothing against it. Binding Microsoft.Build to VS's copies made no difference either
     (decision 4). The JIT share was not isolated. The fixture comparison points to the host's first evaluation,
     which takes about 0.7 s ("Fixture" under Measurements), and to roughly half of Host.EXE's build
     (Consequences). Two options for M2, neither decided here:
     - **(A) Path (c) for the first project of a trusted root while ProjectHost starts.** A fresh `MSBuild.exe`
       with the ADR 0005 wrapper and design-time properties returned Host.EXE's command lines in p50 331 ms
       (10 runs, max 730 ms, min 293 ms;
       `E:\FrostBuilds\perf\m0\s2-s6\run2-corpora\s2-r2b-hostexe-dt.jsonl`, 2026-10-31). That is about 1.6 s
       less than the cold host's 1.90 s on the same day. `MSBuild.exe` loads VS's Microsoft.Build, for which
       this machine's native image cache holds a native image
       (`C:\Windows\assembly\NativeImages_v4.0.30319_64\Microsoft.Build`, dated 2026-10-14); whether the runs
       used it was not checked. What this number does not cover:
       - The runs were loaded: other agents were building during all 10, and the CPU was 31–65 % busy. Commit
         headroom was 0.42–1.08 GiB, far less than the 2.1–5.2 GiB during the cold-host runs of 2026-10-31.
       - They ran only the wrapper target, not PLAN.md's three-target list that the spike host runs. One logged
         run of the full list in `MSBuild.exe` (`-clp:PerformanceSummary`,
         `E:\FrostBuilds\perf\m0\s2-s6\run2-corpora\perfsummary-r2b-hostexe-dt.txt`, 2026-10-31) shows 104 ms
         for Host.EXE's evaluation and 400 ms for its targets. The spike host's in-host evaluation and build
         took 729 ms and 822 ms that day (p50; 760 ms and 878 ms in s26). The full list was not timed end to end
         in a fresh process.
       - It would run beside ProjectHost's own start and the tree fill, which was not measured. It does not
         stream, and its output was not compared with the host's for the same project.
       - Launched before L1 in a trusted root, path (c) also runs the project's MSBuild logic before L1, so it
         raises the same network question as launching ProjectHost before L1 (the question above).
       - B23's method ends at "the first project streamed from ProjectHost", and its preconditions include
         ProjectHost cold start. So this option changes B23's method and preconditions, which needs an ADR and
         the user's approval.
     - **(B) Native images for the host.** NGEN of ProjectHost's own assemblies at install time covers only our
       code. Covering Microsoft.Build as well may mean compiling VS's copies again for the host, which would then
       have to be redone whenever VS updates them. It may not: VS's own Microsoft.Build already has a native
       image here (option (A)), and a host that used that image would need no second NGEN. Whether a host that
       loads Microsoft.Build through MSBuildLocator uses such images at all is open: decision 4's binding to
       VS's copies made no difference. `ngen install` needs administrator rights, while PLAN.md's
       Windows installer is a per-user MSI (section 2.2). ReadyToRun is a .NET Core format that .NET Framework
       does not use. It applies only to the net10.0 host for SDK-style projects (PLAN.md section 2, path (b)),
       not to the net472 host for C++.
     - **What the spike measured.** Only (A)'s 331 ms, taken in S2 with no host beside it, and decision 4's
       binding variant. Neither NGEN of the host nor any native-image build of Microsoft.Build for it was
       measured, so (B) has no number.
   - **Trust given during the warm-up.** The real request waits behind the warm-up. Launch to the first R2b
     project then takes 1.96 s against 1.90 s for a cold host (2026-10-31), or 2.13 s against 2.02 s
     (2026-11-01), p50; the maxima are 2.12 s and 2.36 s. That is about 50–110 ms more than a cold host (the
     medians differ by 54 ms and 112 ms), and the launch itself waited for L1. This case does not arise under
     decision 6 (d).
   - Later projects cost less, but not a flat 0.1 s or so each. After Host.EXE in a cold host (2026-11-01, p50
     over 20 runs), R2b's second project under decision 5, `Host.FeatureTests` (2 references), takes 185 ms, and
     `hostlib` (1 reference) then takes 131 ms. After the pre-warm they take 170 ms and 124 ms (10 runs). Only
     hostlib comes near 0.1 s. Host.EXE requested a second time in a pre-warmed host still takes 398 ms. These
     points do not give a per-reference cost ("References and build time" under Measurements).
   - Whether a restricted root should start the warm-up before L1 instead, so that an early trust click waits
     less, is an M2 measurement. The spike did not measure the host's effect on the tree fill.
3. **S6 does not set B23's final values. It proposes keeping p50 3 s for "first project streamed" on R2b as a
   provisional target. How the final values are set, and with that when D3 becomes final, is the user's
   decision.**
   - PLAN.md's M0 spike list (section 7) reads "S6: ProjectHost cold start ... fixes B23 before D3 is final". The
     order settles B23 before the decision on the C# ProjectHost is final. Proposed precondition 6 (e) makes the
     gating sample cold in PLAN.md section 4's sense, after a reboot or a standby-list purge. Every S6 sample has
     a warm file cache. The spike did not measure, bound or estimate what a cold cache adds.
   - With a cold cache, ProjectHost has to read from disk what the warm runs found in memory, unless another
     process holds it: the .NET Framework and Microsoft.Build assemblies, the VC props and targets, and
     Host.EXE's project with its 14 references and their imports. That could use up the 0.98–1.10 s left at the
     warm medians (decision 1).
   - Machine state alone moves the warm figure: 1.90 s against 2.02 s between the sessions, and 22.5–23.9 % less
     than s26's median in the later runs of the independent re-run (Measurements).
   - Confirming 3 s as final together with 6 (e) would therefore fix a final value for a condition nobody has
     measured. Under either way forward, 3 s stays the ceiling: a final value above it needs an ADR and the
     user's approval. Both ways need cold samples, so both need the user to take part, now or in M2. They are
     the user's to choose:
     - **(i) Cold samples for S6.** Each sample needs a reboot, or an elevated standby-list purge
       (`RAMMap -Et`; RAMMap is not installed here) right before it. S6 would take 10 such samples with the
       cold-host plan and record Defender and Dev Drive state per sample (PLAN.md section 4).
       - The samples bound only ProjectHost's part of B23 on a cold cache. FrostIDE's own part (the open request,
         the native solution read, the tree fill beside the host and the ingest) first exists in M2, so it stays
         unmeasured on a cold cache, as it is on a warm one. A final value proposed from these samples would
         rest on an allowance for that part.
       - D3 waits for the samples: it stays not final until they are in and the user has fixed B23's
         first-project value. PLAN.md's order is kept for that value. It is not kept for the trees value if
         the user accepts the trees proposal below: S6 would then fix only the first-project value before D3
         becomes final, so PLAN.md's B23 row and its S6 spike-list line ("fixes B23 before D3 is final")
         change under this option too.
     - **(ii) Final value from M2.** M2's B23 test, cold under 6 (e) with a warm sample alongside, sets the value.
       - Each cold M2 sample also needs a reboot or an elevated purge (PLAN.md section 4's "Cold"), so this moves
         the user's part to M2 rather than removing it.
       - It times the whole interval, FrostIDE's part included. But B23 gates M2, so the gate would be set by the
         measurement it gates.
       - D3: M2 builds the C# ProjectHost (about 3k lines, PLAN.md section 2, path (b)) before B23 is fixed,
         which inverts PLAN.md's gate. PLAN.md's S6 line and its D3 row change, so this is a plan change.

     **Decided 2026-11-01 (ADR 0011, B1):** option (i). The cold samples are collected passively: one per
     ordinary reboot, through the logon harness that M1 builds for cold B1. D3 stays not final until 10 samples
     exist and the first-project value is fixed from them.
   - **The trees target.** S6 gives no evidence for the trees value (R3, p50 1.5 s; see Context), and no M0
     spike measures the native readers. Removing that target's "final value set by S6" note on the strength of
     S6 would finalise 1.5 s with nothing behind it. Proposed: 1.5 s stays provisional until M2's first B23
     trees measurement on R3, and the note says so.
     - This proposal is a change to PLAN.md, under option (i) as well as option (ii). PLAN.md's B23 row carries
       one "(final value set by S6)" note for both values ("p50 1.5 s / 3 s"), and `budgets.json` puts the
       same note on this target. Under option (i) the S6 spike-list line also no longer holds as written,
       because S6 would fix only one of B23's two values. This ADR edits neither file.
     - 1.5 s stays the ceiling in the same way. The user decides.

     **Decided 2026-11-01 (ADR 0011, B2):** accepted. PLAN.md's B23 row note and S6 spike-list line, and
     `budgets.json`'s note on the trees target, now say so.
   - This ADR does not edit `budgets.json`. Any change to a target needs an ADR and user approval.
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
   - **(d)** Trust is granted as soon as the open request is made, so the excluded trust click takes no time.
     Under decision 2, FrostIDE then runs no warm-up and, as proposed, does not wait for L1, and ProjectHost's
     cold start lies inside the timed interval, as the current precondition requires. M2's open path must skip
     the warm-up. A path that put the warm-up first would time a longer interval than decision 1 assumes. If the
     user decides that the launch waits for L1 (decision 2), the interval also holds that wait.
   - **(e)** The gating sample is cold in PLAN.md section 4's sense (after a reboot or a standby-list purge), and
     a warm-cache sample is reported alongside. All of S6's numbers are warm-cache numbers, so S6 cannot say
     whether 3 s holds under (e) (decision 3).

   If the user approves, `budgets.json`'s B23 preconditions and PLAN.md's B23 row take these over, together
   with the user's choice of how the final values are set (decision 3).

   **Decided 2026-11-01 (ADR 0011, B3):** all five are accepted. `budgets.json`'s B23 preconditions state them;
   PLAN.md's B23 row already defers its preconditions to `budgets.json`.

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
  - Start: the driver (`s6_drive.py`; the copies in `run2-corpora\tools\` and `run3-s26\tools\` under "Raw
    data" below are identical) reads `perf_counter()` immediately before it creates the `S6Host.exe` process.
  - End: the driver has read the complete JSON-RPC response to the first `DesignTime` request from the host's
    stdout.
  - Inside the interval: process creation, CLR start, MSBuildLocator, StreamJsonRpc, the request (written to the
    host's stdin right after launch), evaluation, the design-time build, serialisation of the result and the
    pipe.
  - Outside it, all in FrostIDE and first built in M2:
    - before the request: the open request, the native solution read with decision 5's project selection,
      trust, the supervisor launching ProjectHost through `IProcessLauncher`, and building the request. Under
      decision 2, none of this waits for a warm-up once trust is granted, and as proposed none of it waits for
      L1. If M2's path did wait, the wait would be on the path too;
    - beside the host: the tree fill (native readers, File System and Solution views) runs during the interval
      and competes for CPU and disk. As long as it finishes before the host's response, it adds to the interval
      only through the slowdown it causes the host, not through its own duration. A Solution-view fill that
      outlasts the response can also add up to its remaining time, because B23 ends with the project streamed
      into the Solution tree; a File System scan still running then only competes with the ingest. R2b's tree
      fill was not measured. In the spike, no FrostIDE work ran beside the host;
    - after the response: parsing it, adding the project to the model, streaming it into the Solution tree, and
      the ETW marker that ends B23.

    The spike's request came from a precomputed plan file.
  - The figure is therefore only ProjectHost's side of B23. Under the same machine and cache state it is an
    expected lower bound on B23, not a guaranteed one.
    - It holds only if M2's ProjectHost does at least the spike host's work for the first request: the same
      target list, a first evaluation in a fresh process, and a request already waiting when the host attaches.
    - The spike host is a throwaway prototype that creates a new `ProjectCollection` for each request. M2's
      planned shared `EvaluationContext` and 2–4 workers (PLAN.md section 2, path (b)) could make the first
      request cheaper.
- **Method.** Each run is a fresh process with a warm file cache. It is not a post-reboot cold start, and the
  spike has no cold sample (decision 3). The driver measures from process launch to each response. The host's
  marks are milliseconds since process start.
- **Units.** Memory figures are read through Win32 in binary units: host memory in MiB, and commit headroom and
  private memory in GiB.
- **Raw data.** It is kept outside the repository in `E:\FrostBuilds\perf\m0\s2-s6\`. Its `README.txt` maps
  each folder to the sections it backs:
  - `run1-prototype\s6\`: the first session's driver and host sources. The first-session Fixture table's runs
    left no per-run log;
  - `run2-corpora\`: 2026-10-31, one `jsonl` per batch (plan, per-run times, phase marks, load and host
    memory) for the R2a, R2b and fixture rows, with `analysis.txt`, the plans, the driver, and the spike host's
    sources and build log;
  - `run3-s26\`: 2026-11-01. `raw\` holds the per-run `jsonl` of the runs under the lock "s26", the lock and
    condition records, the reference counts (`projectrefs.txt`) and the solution order
    (`slnx-order-debug-x64.txt`); `smoke\` holds the smoke runs;
  - `analysis\`: the fix rounds' recomputations (`s26-2\headroom.txt`, `fix3-s26\figures.txt`);
  - `verification\`: the independent verifiers' re-runs and recomputations; `verify2-s26\` holds the
    independent re-run below.

  The scripts still name the session scratchpad paths they ran with. Compiled hosts, `bin\` and `obj\` folders
  and third-party sources and build output are not kept.
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
  - Before the lock, one smoke run of each host plan ran at 01:29:12–01:29:21, after `build-gate` but outside
    any measurement lock. They should have run inside the s26 lock: from 01:29:14 they overlapped another
    agent's lock (verify-s1), whose first samples may carry their load. The smoke runs are in no table here.
- **Independent re-run, 2026-11-01.** The verification re-ran both host plans, 3 runs each, under its own lock
  (verify-s26, 01:50:12–01:50:34, CPU 10–37 % busy before each run).
  - Cold Host.EXE took 2049, 1564 and 1536 ms, then Host.FeatureTests 190, 126 and 138 ms.
  - Pre-warmed, launch to Host.EXE took 1.60–1.64 s.
  - Its first run matched s26. The later two took 22.5 % and 23.9 % less time than s26's median (1564 and 1536
    against 2019 ms). Over the three cold runs, the commit headroom before each run grew from 26.4 to 33.1 GiB;
    it was 33.2–34.1 GiB before the pre-warmed runs that followed. The cause was not isolated.

  In both sessions the machine was loaded, so the numbers are best read as upper bounds for a warm file cache.
  They are not upper bounds for a cold cache (decision 3).

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
points to JIT in our host's first evaluation, which takes about 0.7 s. The warm-up hides that cost only when
trust comes after the warm-up. When trust comes first, as in the gating case under decision 6 (d), no warm-up
runs and the cost lies on B23's first-project path. Decision 2 lists the M2 options against it.

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
- ProjectHost needs a `warmUp` request and a bundled dummy project (M2). On an open whose first project has no
  valid ProjectModelCache record, such as a first-ever open, the supervisor follows decision 2's start rule. It
  starts the host at L1 with the warm-up while trust is pending on a first-ever open. Once trust is granted, it
  starts the host without the warm-up: at once as proposed, or at L1 if the user decides so (decision 2). A
  warm reopen of an unchanged tree starts no ProjectHost and no warm-up (PLAN.md section 3.4, B3, T14). On a
  machine without VS the host is not started.
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
  them. The next-project figures (Host.FeatureTests 170–185 ms, hostlib 124–131 ms) are therefore expected to be
  upper bounds for a warm cache.
- **Follow-ups once the user has decided** (not done here):
  - `budgets.json`'s B23 preconditions, and the "final value set by S6" note on each of its two B23 targets.
    S6 has no cold sample for the first-project target and no evidence for the trees target (decision 3);
  - PLAN.md's B23 row, under either option of decision 3 if the user accepts the trees proposal, because its
    one "(final value set by S6)" note covers both values;
  - the M0 spike list's "fixes B23 before D3 is final" with the D3 row, if the first-project value moves to M2
    (decision 3, option (ii)). PLAN.md's B23 row changes under that option as well. The spike-list line alone
    also changes under option (i) if the user accepts the trees proposal;
  - PLAN.md's no-network-before-L1 rule (sections 2.1 and 2.2), if the user exempts the trusted project's own
    MSBuild logic before L1 (decision 2);
  - B23's method and preconditions, if M2 adopts path (c) for the first project (decision 2, option (A));
  - R2b's Windows SDK prerequisite in PLAN.md section 4 and `tests/corpus/manifest.json`, which should read
    10.0.26100.0, not 10.0.22621 (ADR 0005).
- R2b measurements assume restored NuGet packages, the state a developer of that repository has (ADR 0005
  decision 6). Its C++/WinRT projects need the UWP workload, which is not installed here. Host.EXE,
  Host.FeatureTests and hostlib do not need it.
- These three NuGet packages (all MIT) need the licence check and an ADR when ProjectHost lands in M2. The spike
  code stays outside the repository.

## Alternatives considered
- Path (c) only (`MSBuild.exe -getTargetResult` per request): 0.33–0.41 s per process with no warm reuse
  (ADR 0005), and it cannot stream results. It stays as the fallback and the consistency check. Path (c) for
  just the first project of a trusted root, while ProjectHost starts, is not rejected: it is decision 2's open
  M2 option (A).
- A net10.0 host for C++ projects: the VC targets require .NET Framework MSBuild, so it is not possible.
- NGEN of the host at install time: not rejected. It is decision 2's open M2 option (B), because no warm-up runs
  in the gating case.
- Pre-warming with the solution's own first project instead of a bundled dummy: it would hide the remaining
  0.45–0.50 s, but it runs the repository's MSBuild logic before trust (PLAN.md section 3.5).
- Running the warm-up even when the root is already trusted: the real request would wait behind it, about
  50–110 ms more than a cold host (decision 2), for no gain.
- Waiting for L1 before launching ProjectHost in a trusted root: not rejected; it is answer 3 of decision 2's
  question for the user. It keeps PLAN.md's no-network-before-L1 rule and puts the time to L1 on B23's
  first-project path. Launching at once does the reverse: it puts ProjectHost on the path to L1.
