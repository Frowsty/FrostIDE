# ADR 0005: Spike S2, MSBuild `-getTargetResult:GetClCommandLines`

- Status: accepted (fixture, R2a and R2b).
- Date: 2026-10-31; revised 2026-11-01 after the independent verification (R2a control re-measured with
  `SolutionDir`, units, TerminalAppLib failure, side effects on C:, Windows SDK).
- Plan reference: PLAN.md section 2 ("MSBuild / .vcxproj", path (c)), M0 spike S2

## Context
Path (c) of the MSBuild design runs `MSBuild.exe -getTargetResult` JSON as a spike, a consistency check and a
pure-C++ fallback for extracting per-file `cl.exe` command lines. S2 measures its latency and checks that it
works with the installed MSBuild 17.14.60 (VS 2022 Build Tools). R2b (microsoft/terminal) adds a large solution
whose projects import props through `$(SolutionDir)`, need a NuGet restore and partly use C++/WinRT.

## Decision
1. **`-getTargetResult:GetClCommandLines` cannot be used directly.** MSBuild 17.14's JSON output formatter computes
   `%(FullPath)` for every returned item. `GetClCommandLines` returns items whose item spec is the command line
   itself, so the formatter throws `InvalidOperationException: The item metadata "%(FullPath)" cannot be applied
   to the path "/c /I…". Illegal characters in path` (MSB1025) and MSBuild exits with an internal failure.
2. **FrostIDE injects a wrapper target through the documented `ForceImportAfterCppTargets` property.** The wrapper
   moves the command line into metadata and gives every item a valid path as its item spec, keeping all other
   metadata:
   ```xml
   <Target Name="FrostGetClCommandLines" Returns="@(FrostClLine)">
     <CallTarget Targets="GetClCommandLines">
       <Output TaskParameter="TargetOutputs" ItemName="_FrostClRaw" />
     </CallTarget>
     <ItemGroup>
       <_FrostClRaw><CommandLine>%(Identity)</CommandLine></_FrostClRaw>
       <FrostClLine Include="@(_FrostClRaw->'$(MSBuildProjectFullPath)')" />
     </ItemGroup>
   </Target>
   ```
   It is invoked as `MSBuild.exe proj.vcxproj -nologo -nodeReuse:false -p:Configuration=… -p:Platform=…
   -p:SolutionDir=<solution dir>\ <the design-time properties of decision 5>
   -p:ForceImportAfterCppTargets=<frost-extract.targets> -getTargetResult:FrostGetClCommandLines`.
3. **What the results contain.** Each item covers a *group* of files that share one option set, with metadata
   `CommandLine`, `Files` (`;`-separated absolute paths), `WorkingDirectory`, `ToolPath` and sometimes
   `ConfigurationOptions`. A per-file `PreprocessorDefinitions` override produces its own group, and so does a
   precompiled header: each measured R2b project returns one `/Yc` group (`precomp.cpp`) and one `/Yu` group.
4. **`ToolPath` is not trusted.** Outside a developer environment it reads `C:\WINDOWS\system32\CL.exe`, a path that
   does not exist. The compiler path always comes from toolchain detection (PLAN.md section 3.5,
   `IProcessLauncher` resolves absolute paths).
5. **Path (c) passes the same design-time global properties as ProjectHost:** `DesignTimeBuild=true`,
   `BuildingInsideVisualStudio=true`, `BuildProjectReferences=false`, `LangID=1033`,
   `SkipInvalidConfigurations=true` and `SolutionDir`, the set that microsoft/msbuild-extractor-sample (MIT) uses in
   `InProcessExtractor.cs`. Without them, `GetClCommandLines` can build project references. On R2b's
   `TerminalAppLib.vcxproj` it built the referenced `types.vcxproj`: that ran the repository's feature-flag
   generator and a vcpkg manifest install for 26 s, then failed. It wrote `obj\` and `bin\` into the source tree
   (34 files, 0.86 MB, moved out afterwards) and 552 MB (526 MiB, 4,201 files) into `%LOCALAPPDATA%\vcpkg` on C:
   (see Consequences).
   - The TerminalAppLib run *with* the properties is no evidence for them: it stopped at MSB8036 (R2b table)
     before its references were reached.
   - The evidence is `Host.EXE.vcxproj`, which references the same `types.vcxproj` among its 14 references. With
     the properties, its `-clp:PerformanceSummary` run shows only targets that answer reference queries on those
     projects: `GetTargetPath`, `GetProjectInfoForReference` and `GetPublicIncludeDirectories` (14 calls each)
     and their dependencies, such as `PrepareForBuild` (15). No `Build`, `ClCompile`, feature-flag or vcpkg
     target ran.
   - After every run of both sessions, the corpora's `git status --ignored` shows only R2b's `packages/`, the
     corpora contain no new empty directories, and `%LOCALAPPDATA%\vcpkg` has no file newer than the unintended
     run (20:55:17 on 2026-10-31).
   - On a plain project the properties change one switch: `/errorReport:queue` becomes `/errorReport:prompt`.
6. **Command lines extracted before a NuGet restore are not trusted.** Before the restore, R2b's `server.vcxproj`
   extracted successfully, but its command line lacked the WIL include directory
   (`/I…\packages\Microsoft.Windows.ImplementationLibrary.1.0.250325.1\build\native\..\..\include\`). The import
   that adds it is guarded by `Exists()`, so nothing fails. FrostIDE therefore has to detect the missing restore
   itself (see Consequences).

## Measurements
REF-W1, MSBuild 17.14.60.43110 (amd64), MSVC 14.44.35207 and Windows SDK 10.0.26100.0, the only SDK installed.
Each run is a fresh MSBuild process (`-nodeReuse:false`) with a warm file cache, timed by the driver from process
launch to exit.

R2b's projects use that SDK: at the pinned commit, `src/common.build.pre.props` defaults
`WindowsTargetPlatformVersion` to 10.0.26100.0. PLAN.md section 5 and `tests/corpus/manifest.json` still give
"Windows SDK 10.0.22621" as R2b's prerequisite, which no longer matches the pin (see Consequences).

The first table is from the first session: three other agents were building at the same time, and no
design-time properties were passed.

| Corpus | Runs | p50 | max | Notes |
|---|---|---|---|---|
| Fixture: one v143 Application project, 4 TUs, one per-file override | 10 | 358 ms | 576 ms | 2 groups returned: {main, src1, src2}, {src3 with ONLY_SRC3} |
| R2a (Notepad++ v8.9.8.1, `PowerEditor/visual.net/notepadPlus.vcxproj`, Release\|x64) | 10 | 342 ms | 498 ms | 1 group with all 97 `ClCompile` items; min 316 ms; one other agent building |

R2b (microsoft/terminal v1.25.2733.0, `dc8ae36`, `OpenConsole.slnx`) is measured in Debug\|x64 after the NuGet
restore, with `-p:SolutionDir=E:\corpus\R2b\` and the design-time properties of decision 5:

| Project | `ClCompile` | Runs | p50 | max | min | Groups |
|---|---|---|---|---|---|---|
| `src/server/lib/server.vcxproj` (static library, plain C++) | 20 | 10 | 365 ms | 497 ms | 305 ms | 2: 19 files `/Yu`, 1 `/Yc` |
| `src/host/lib/hostlib.vcxproj` (conhost core; its sources and its one project reference, `midi.vcxproj`, come from the shared-items project `host-common.vcxitems`) | 44 | 10 | 405 ms | 670 ms | 368 ms | 2: 43 `/Yu`, 1 `/Yc` |
| `src/host/exe/Host.EXE.vcxproj` (the solution's first project, 14 project references) | 3 | 10 | 331 ms | 730 ms | 293 ms | 2: 2 `/Yu`, 1 `/Yc` |
| `src/cascadia/TerminalApp/TerminalAppLib.vcxproj` (C++/WinRT) | 44 | – | not measured | | | Evaluation succeeds, but the build fails. A target in the restored C++/WinRT package (`Microsoft.Windows.CppWinRT.targets(126,9)`) raises MSB8036, "The Windows SDK version 10.0.17134.0 (or later) was not found", and `-getTargetResult` prints `FrostGetClCommandLines` with `Result: Failure` and no items. The project sets `ApplicationType=Windows Store`, an application type that comes with the UWP workload. This install has only the `Linux` application type. An evaluation-only check (2026-11-01) gives `TargetPlatformVersion` 7.0, below the 10.0.17134.0 that the message asks for, while `WindowsTargetPlatformVersion` is 10.0.26100.0. Before the restore it failed with MSB4096 instead. |

Controls, under the same conditions unless noted:

| Variant | Runs | p50 | max | min | Notes |
|---|---|---|---|---|---|
| R2b `server.vcxproj` after the restore, first-session command line (no design-time properties) | 10 | 344 ms | 450 ms | 332 ms | Same flags as the design-time run apart from `/errorReport` |
| R2b `server.vcxproj` before the restore, first-session command line | 10 | 346 ms | 588 ms | 328 ms | Same groups and files; the WIL include directory is missing (decision 6) |
| R2a `notepadPlus.vcxproj` (Release\|x64) with decision 5's properties except `SolutionDir` | 10 | 400 ms | 440 ms | 340 ms | 1 group, 97 files |
| The same with the full set, `SolutionDir=E:\corpus\R2a\PowerEditor\visual.net\` (2026-11-01, s26) | 10 | 374 ms | 414 ms | 357 ms | 1 group, 97 files. Conditions: see the s26 note below |

- **Restore.** The repository's own Microsoft-signed NuGet 7.6.0 and feed were used:
  `dep\nuget\nuget.exe restore dep\nuget\packages.config -PackagesDirectory packages -ConfigFile NuGet.Config`.
  It installed 17 packages in 7.4 s into the git-ignored `packages\` folder: 918 MB (876 MiB) in 1,800 files.
  R2b's `NuGet.Config` points both the global packages folder and the repository path at `packages\`, so each
  package is there in both layouts (34 folders). The HTTP cache was redirected to `E:\sdk\nuget-http-cache`.
  After the restore, the corpus's `git status --ignored` shows only `packages/`. The `obj\` and `bin\` from the
  unintended build in decision 5 were moved out of the tree.
- **Units.** MB and GB are decimal. Commit headroom and private memory are read through Win32 and PowerShell in
  binary units, so they are given in GiB.
- **Machine load.** `lghub_agent.exe` (Logitech G HUB) held 43.3 GiB of private memory, which left 0.4–4.3 GiB of
  commit headroom during these batches. Other agents were building (cmake, ninja, clang-cl, lld-link or vcpkg in
  five of the six batches), and the CPU was 26–78 % busy, sampled for 0.5 s before each run. These numbers are
  upper bounds: the max column follows the load spikes, and the R2a run with design-time properties (400 ms)
  cannot be compared with the first-session R2a run (342 ms), which ran under different load.
- **s26 re-measurement (2026-11-01, 01:30–01:32).** Under the overnight measurement lock "s26":
  - The lock's conditions record: free commit 26.4 GiB of 64.1, 10.5 GiB physical free, system CPU 36.8 %, and no
    other agent's build running.
  - The user's own processes kept running, as recorded right after the runs: the `E:\r11` tree (4 FrostEditor,
    25 FrostRuntime, 2 FrostServices and 2 Python processes, 7.2 GiB private together), Discord (2.1 GiB) and
    Wallpaper Engine. `lghub_agent.exe` had dropped to 0.17 GiB private.
  - The CPU was 26–47 % busy before the R2a runs, and commit headroom was 27.7–27.8 GiB.

## Consequences
- Path (c) needs a targets file on disk. It is shipped with FrostIDE and resolved to an absolute path, never taken
  from the project directory.
- ProjectHost (path (b)) can call the same target in-process, so (b) and (c) still share one engine for the
  consistency check (PLAN.md section 6.1). They also use the same global properties (decision 5).
- About 0.3–0.4 s per fresh process is the floor for (c): p50 331–405 ms across three R2b projects and R2a. It
  barely grows with project size. The 44-TU hostlib costs 40 ms more than the 20-TU server, and the 97-TU R2a
  project about the same as the 4-TU fixture, because evaluation and the VC targets dominate. (c) is still not on
  the warm-open path, which starts zero .NET processes (B3).
- Path (c) runs the project's MSBuild logic, so like ProjectHost it runs only in a trusted root (PLAN.md section
  3.5). Without decision 5's properties that logic includes building project references, so the M2 tests assert
  the exact property set for both (b) and (c).
- A NuGet restore changes the extracted command lines, yet none of the files that the earlier evaluation imported
  changes: the restore creates files that its `Exists()` conditions probed and did not find. So the
  ProjectModelCache fingerprint must also cover those probed-but-missing paths, or a later restore leaves a stale
  record (M2, B4). One way is to evaluate through an `EvaluationContext` with a recording
  `MSBuildFileSystemBase`. The same data can drive a "packages not restored" hint in the Solution view (M2).
- R2b's projects open with `SolutionDir` set to the solution directory, as when the IDE opens a project through
  its solution. Its C++/WinRT projects (all under `src/cascadia`, including `TerminalAppLib`) need the UWP workload.
  Until the user installs it, R2b measurements use the OpenConsole host projects, as the corpus manifest already
  says.
- R2b's Windows SDK prerequisite in PLAN.md section 5 and `tests/corpus/manifest.json` (10.0.22621) should read
  10.0.26100.0, the version the pinned commit defaults to and the one `sdk/sdk.lock.json` pins. This ADR leaves
  both files unchanged, because PLAN.md changes need the user. That correction is listed for the user with the
  other open B23 questions (ADR 0006).
- **Third-party downloads left on C: (for the user to decide).** PLAN.md and `tools/env.ps1` keep tool caches on
  E:, but the unintended reference build of decision 5 ran the VS-bundled `vcpkg.exe`, which wrote 4,201 files,
  552 MB (526 MiB), to `%LOCALAPPDATA%\vcpkg` at 20:54:53–20:55:17 on 2026-10-31. They were left in place:
  - `downloads\`: PowerShell-7.5.3-win-x64.zip, 7z2501.7z and 7z2501.7z.exe, ninja-win-1.13.1.zip and the tools
    extracted from them under `downloads\tools\` (416 MB together);
  - `registries\`: vcpkg's registry `git` clone and `git-trees` (136 MB).

  The first count, 119 MB (3,144 files), does not reproduce. This count takes every file under the two folders,
  and both folders were created during the run; the extracted tools keep their archives' older timestamps.
  `%LOCALAPPDATA%\vcpkg\archives` (6.3 MB, 18:52) predates the run and is not from it.

  During the spike, two NuGet HTTP-cache index entries (0.76 MB) also landed in
  `%LOCALAPPDATA%\NuGet\v3-cache`: R2b's TerminalDependencies feed index at 20:56:26, and nuget.org's index and
  vulnerability data at 21:01:42, when the spike host's `dotnet` restore ran. The user decides whether to keep
  or delete these files. Both are caches: vcpkg and NuGet download them again when they need them.
- Grouping by option set means FrostIDE expands groups into per-file compile commands itself.

## Alternatives considered
- `-getItem` after `-t:GetClCommandLines`: needs the internal item type name, which would mean reading Visual
  Studio's proprietary targets (excluded by the clean-room rule). The wrapper only depends on the target's
  documented outputs.
- `WriteLinesToFile` from a custom target: works, but loses the structured metadata that `-getTargetResult`
  provides.
- Leaving out the design-time properties on path (c): 21 ms faster on `server.vcxproj` (344 against 365 ms), which
  is within the run-to-run spread. It is unsafe on projects with references (decision 5).
