# ADR 0005: Spike S2, MSBuild `-getTargetResult:GetClCommandLines`

- Status: accepted (fixture, R2a and R2b).
- Date: 2026-10-31
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
   generator and a vcpkg manifest install for 26 s, then failed. It also wrote `obj\` and `bin\` into the source
   tree and 119 MB into `%LOCALAPPDATA%\vcpkg`. With the properties set, no run wrote to the corpus or to
   `%LOCALAPPDATA%\vcpkg`. On a plain project they change one switch: `/errorReport:queue` becomes
   `/errorReport:prompt`.
6. **Command lines extracted before a NuGet restore are not trusted.** Before the restore, R2b's `server.vcxproj`
   extracted successfully, but its command line lacked the WIL include directory
   (`/I…\packages\Microsoft.Windows.ImplementationLibrary.1.0.250325.1\build\native\..\..\include\`). The import
   that adds it is guarded by `Exists()`, so nothing fails. FrostIDE therefore has to detect the missing restore
   itself (see Consequences).

## Measurements
REF-W1, MSBuild 17.14.60.43110 (amd64). Each run is a fresh MSBuild process (`-nodeReuse:false`) with a warm file
cache, timed by the driver from process launch to exit.

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
| `src/host/lib/hostlib.vcxproj` (conhost core; its sources come from the shared-items project `host-common.vcxitems`) | 44 | 10 | 405 ms | 670 ms | 368 ms | 2: 43 `/Yu`, 1 `/Yc` |
| `src/host/exe/Host.EXE.vcxproj` (the solution's first project, 14 project references) | 3 | 10 | 331 ms | 730 ms | 293 ms | 2: 2 `/Yu`, 1 `/Yc` |
| `src/cascadia/TerminalApp/TerminalAppLib.vcxproj` (C++/WinRT) | 44 | – | not measured | | | Evaluation fails with MSB8036: the project sets `ApplicationType=Windows Store`, which needs the UWP workload. That workload is not installed (only the `Linux` application type is), so C++/WinRT's SDK check fails. Before the restore it failed with MSB4096 instead. |

Controls, under the same conditions unless noted:

| Variant | Runs | p50 | max | min | Notes |
|---|---|---|---|---|---|
| R2b `server.vcxproj` after the restore, first-session command line (no design-time properties) | 10 | 344 ms | 450 ms | 332 ms | Same flags as the design-time run apart from `/errorReport` |
| R2b `server.vcxproj` before the restore, first-session command line | 10 | 346 ms | 588 ms | 328 ms | Same groups and files; the WIL include directory is missing (decision 6) |
| R2a `notepadPlus.vcxproj` (Release\|x64) with the design-time properties | 10 | 400 ms | 440 ms | 340 ms | 1 group, 97 files |

- **Restore.** The repository's own Microsoft-signed NuGet 7.6.0 and feed were used:
  `dep\nuget\nuget.exe restore dep\nuget\packages.config -PackagesDirectory packages -ConfigFile NuGet.Config`.
  It installed 17 packages (876 MB) in 7.4 s into the git-ignored `packages\` folder. After the restore, the
  corpus's `git status --ignored` shows only `packages/`. The `obj\` and `bin\` from the unintended build in
  decision 5 were moved out of the tree.
- **Machine load.** `lghub_agent.exe` (Logitech G HUB) held 43.3 GB of private memory, which left 0.4–4.3 GB of
  commit headroom during these batches. Other agents were building (cmake, ninja, clang-cl, lld-link or vcpkg in
  five of the six batches), and the CPU was 26–78 % busy, sampled for 0.5 s before each run. These numbers are
  upper bounds: the max column follows the load spikes, and the R2a run with design-time properties (400 ms)
  cannot be compared with the first-session R2a run (342 ms), which ran under different load.

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
- Grouping by option set means FrostIDE expands groups into per-file compile commands itself.

## Alternatives considered
- `-getItem` after `-t:GetClCommandLines`: needs the internal item type name, which would mean reading Visual
  Studio's proprietary targets (excluded by the clean-room rule). The wrapper only depends on the target's
  documented outputs.
- `WriteLinesToFile` from a custom target: works, but loses the structured metadata that `-getTargetResult`
  provides.
- Leaving out the design-time properties on path (c): 21 ms faster on `server.vcxproj` (344 against 365 ms), which
  is within the run-to-run spread. It is unsafe on projects with references (decision 5).
