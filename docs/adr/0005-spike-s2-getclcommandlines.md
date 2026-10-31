# ADR 0005: Spike S2, MSBuild `-getTargetResult:GetClCommandLines`

- Status: accepted (fixture and R2a).
- Date: 2026-10-31
- Plan reference: PLAN.md section 2 ("MSBuild / .vcxproj", path (c)), M0 spike S2

## Context
Path (c) of the MSBuild design runs `MSBuild.exe -getTargetResult` JSON as a spike, a consistency check and a
pure-C++ fallback for extracting per-file `cl.exe` command lines. S2 measures its latency and checks that it
works with the installed MSBuild 17.14.60 (VS 2022 Build Tools).

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
   -p:ForceImportAfterCppTargets=<frost-extract.targets> -getTargetResult:FrostGetClCommandLines`.
3. **What the results contain.** Each item covers a *group* of files that share one option set, with metadata
   `CommandLine`, `Files` (`;`-separated absolute paths), `WorkingDirectory`, `ToolPath` and sometimes
   `ConfigurationOptions`. A per-file `PreprocessorDefinitions` override produces its own group.
4. **`ToolPath` is not trusted.** Outside a developer environment it reads `C:\WINDOWS\system32\CL.exe`, a path that
   does not exist. The compiler path always comes from toolchain detection (PLAN.md section 3.5,
   `IProcessLauncher` resolves absolute paths).

## Measurements
REF-W1, MSBuild 17.14.60.43110 (amd64), a fresh MSBuild process per run (`-nodeReuse:false`), warm file cache.
Three other agents were building at the same time, so these are upper bounds.

| Corpus | Runs | p50 | max | Notes |
|---|---|---|---|---|
| Fixture: one v143 Application project, 4 TUs, one per-file override | 10 | 358 ms | 576 ms | 2 groups returned: {main, src1, src2}, {src3 with ONLY_SRC3} |
| R2a (Notepad++ v8.9.8.1, `PowerEditor/visual.net/notepadPlus.vcxproj`, Release\|x64) | 10 | 342 ms | 498 ms | 1 group with all 97 `ClCompile` items; min 316 ms; one other agent building |

## Consequences
- Path (c) needs a targets file on disk. It is shipped with FrostIDE and resolved to an absolute path, never taken
  from the project directory.
- ProjectHost (path (b)) can call the same target in-process, so (b) and (c) still share one engine for the
  consistency check (PLAN.md section 6.1).
- About 0.35 s per fresh process is the floor for (c), and it barely grows with project size: the 97-TU R2a project
  costs the same as the 4-TU fixture, because evaluation and the VC targets dominate. (c) is still not on the
  warm-open path, which starts zero .NET processes (B3).
- An R2b (microsoft/terminal) data point needs `-p:SolutionDir=<solution dir>\` (its projects import
  `$(SolutionDir)src\common.build.pre.props`). The IDE sets it when it opens a project through its solution. The
  run itself is pending: evaluating R2b executes that repository's MSBuild logic, which needs the user's approval
  on this machine.
- Grouping by option set means FrostIDE expands groups into per-file compile commands itself.

## Alternatives considered
- `-getItem` after `-t:GetClCommandLines`: needs the internal item type name, which would mean reading Visual
  Studio's proprietary targets (excluded by the clean-room rule). The wrapper only depends on the target's
  documented outputs.
- `WriteLinesToFile` from a custom target: works, but loses the structured metadata that `-getTargetResult`
  provides.
