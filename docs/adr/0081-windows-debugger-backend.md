<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0081: The Windows debugger backend for natvis and large PDBs (DBG-0)

- Status: accepted for the direction; the DbgEng adapter itself is DBG-14 (R6). Redistribution of
  the Debugging Tools for Windows is an open question for the user (D36), and this ADR does not need it.
- Date: 2026-11-09
- Plan reference: DBG-0 (PLAN.md section 7, R3), D36, DBG-4a, DBG-4b, DBG-8, DBG-14.

## Context

FrostIDE debugs on Windows through the pinned lldb-dap 23.1.4 (ADR 0050). D36 left the Windows
backend for natvis and large PDBs open until a spike measured DbgEng against lldb and the
redistribution terms were reviewed. The measure is FrostEngine's Debug build in the pinned clone
`E:\FrostBuilds\corpora\frostengine-8237e1ecfeb9\build\windows-debug\bin`: FrostTests.pdb
(3,624 MB on disk; PLAN.md says 3.01 GB, which was an earlier build) and FrostEditor.pdb (2,350 MB).
FrostRuntime was not built in that clone. The R3 integration then built FrostTests and FrostRuntime in
the current pin's clone (`frostengine-499e734e0055` under `D:\FrostWork\corpora`) and measured all three
programs there (second table below).

## The spike

`tools/dbg0/dbg0_spike.cpp`, built and run by `tools/dbg0/run-dbg0.ps1` with the pinned clang-cl.
Each run starts the program, sets a function breakpoint on `main`, runs to it, records the time and
the debugger's peak memory, and then ends the debuggee.

- **DbgEng.** The engine runs in the spike's own process. It is loaded from
  `%SystemRoot%\System32\dbgeng.dll` (the operating system's copy, 6.2.26100.1) with
  `LoadLibraryExW(LOAD_LIBRARY_SEARCH_SYSTEM32)`, and nothing links `dbgeng.lib`. The symbol path is
  the program's folder only. Line loading is on, and the engine stops at the initial break. The
  breakpoint is `bp <module>!main`, and the peak memory figures are the spike process's.
- **lldb.** The pinned lldb-dap runs over DAP with the same sequence FrostIDE sends: initialize,
  launch, setFunctionBreakpoints, configurationDone, then the stopped event. The peak memory figures
  are lldb-dap's.

The runs are sequential, and each program alternates DbgEng and lldb. The first DbgEng run of a
program had the cold disk cache, so the order favours lldb.

Results (2026-11-09, this machine; `E:\FrostBuilds\r3-scratch\dbg0\dbg0-results.txt`):

| Program, run | Backend | First breakpoint | Breakpoint bound | Peak working set | Peak private |
|---|---|---|---|---|---|
| FrostTests 1 (cold) | DbgEng | 8.1 s | 3.2 s | 463 MB | 452 MB |
| FrostTests 1 | lldb-dap | 119.7 s | 105.4 s | 2,617 MB | 1,743 MB |
| FrostTests 2 | DbgEng | 1.8 s | 1.7 s | 463 MB | 452 MB |
| FrostTests 2 | lldb-dap | 41.1 s | 32.0 s | 2,618 MB | 1,741 MB |
| FrostEditor 1 (cold) | DbgEng | 4.0 s | 3.7 s | 278 MB | 265 MB |
| FrostEditor 1 | lldb-dap | 45.0 s | 36.7 s | 1,670 MB | 989 MB |
| FrostEditor 2 | DbgEng | 0.6 s | 0.5 s | 278 MB | 265 MB |
| FrostEditor 2 | lldb-dap | 22.7 s | 17.7 s | 1,670 MB | 988 MB |

Second run on the R3 pin (2026-11-09 18:54, this machine, the clone on the NVMe disk D:, after the
programs had just been built so every run is warm; `tools/dbg0/run-dbg0.ps1 -ProgramDir
D:\FrostWork\corpora\frostengine-499e734e0055\build\windows-debug\bin`, results in
`E:\FrostBuilds\r3-scratch\dbg0-499e734\dbg0-results.txt`). FrostTests.pdb is 3,229 MB, FrostEditor.pdb
1,755 MB and FrostRuntime.pdb 929 MB:

| Program, run | Backend | First breakpoint | Breakpoint bound | Peak working set | Peak private |
|---|---|---|---|---|---|
| FrostTests 1 | DbgEng | 1.5 s | 1.4 s | 463 MB | 452 MB |
| FrostTests 1 | lldb-dap | 46.7 s | 38.2 s | 2,591 MB | 1,715 MB |
| FrostTests 2 | DbgEng | 1.6 s | 1.4 s | 463 MB | 452 MB |
| FrostTests 2 | lldb-dap | 48.0 s | 38.6 s | 2,593 MB | 1,716 MB |
| FrostEditor 1 | DbgEng | 0.9 s | 0.8 s | 278 MB | 265 MB |
| FrostEditor 1 | lldb-dap | 29.5 s | 22.7 s | 1,670 MB | 989 MB |
| FrostEditor 2 | DbgEng | 0.8 s | 0.7 s | 277 MB | 265 MB |
| FrostEditor 2 | lldb-dap | 34.2 s | 28.4 s | 1,669 MB | 988 MB |
| FrostRuntime 1 | DbgEng | 0.8 s | 0.6 s | 221 MB | 209 MB |
| FrostRuntime 1 | lldb-dap | 27.7 s | 20.6 s | 1,330 MB | 912 MB |
| FrostRuntime 2 | DbgEng | 0.6 s | 0.5 s | 221 MB | 209 MB |
| FrostRuntime 2 | lldb-dap | 26.4 s | 19.3 s | 1,331 MB | 912 MB |

Every run stopped on the breakpoint. Warm, DbgEng reaches the first breakpoint 20 to 40 times faster
than lldb and uses about a sixth of the memory. Most of lldb's time goes into binding the breakpoint,
which reads the PDB through its own PDB reader. Two lldb-dap sessions on FrostTests use about 5 GB.

**Natvis.** The small cl.exe Debug fixture (`tests/debug/fixtures/msvc_debuggee.cpp`) stopped in
`callee`, and the commands were then run in `main`'s frame (`.frame 1`). Without natvis, `dx` showed
raw `_Mypair` members only. After `.nvload` of the installed Build Tools' `stl.natvis`
(`Common7\Packages\Debugger\Visualizers\stl.natvis`), `dx` showed what Visual Studio shows:
`numbers : { size=0x3 }` with `[0] 1, [1] 2, [2] 3` and `[capacity]`, `table : { size=0x2 }` with
`["1"] 1, "one"` and `["2"] 2, "two"`, and `text : "hello msvc"` with `[size] 0xa` and its
characters (`E:\FrostBuilds\r3-scratch\dbg0\fixture-dbgeng-natvis.log`). `.nvload` takes any
natvis file, so a project's own files load the same way.

**lldb's MSVC STL formatters (DBG-4a).** These are on by default in lldb 23.1.4 (the C++ category),
and FrostIDE sends no setting for them. They show the elements of vector, deque, list, set, map,
unordered_map, tuple, unique_ptr, shared_ptr, optional and variant, and the text of string, wstring
and string_view (`MsvcDebug.StlContainersShowTheirElements`). The gaps are:

- std::array has no formatter and shows a raw `_Elems` child.
- string, wstring and string_view show only raw members as children, with no size or capacity.
- map and unordered_map elements show the pair's type name until they are expanded.
- optional and variant summaries carry padding spaces (`" Has Value=true "`).
- lldb cannot read natvis, so a project's own visualisers (FrostEngine's Frost.natvis, FER-16) are
  not used.

## Licence review

- `dbgeng.dll` in System32 is part of Windows. FrostIDE loads the copy the user's Windows has, by
  absolute path, and never ships, copies or patches it. Using an operating-system component through
  its documented API needs no redistribution right. The headers (`dbgeng.h`) come from the Windows
  SDK that the build already uses.
- The Windows SDK licence (`sdk_license.rtf`, 10.0.26100.0) allows redistribution only of the
  "Distributable Code" named in the online REDIST.TXT list (fwlink 524842). It names the Debugging
  Tools for Windows (cdb, WinDbg and their newer `dbgeng.dll`, `dbghelp.dll`, `symsrv.dll` and
  `srcsrv.dll`) as "Utilities". Utilities may be copied to a third party's machine only to debug
  there, and must be deleted within 30 days. They are not a redistributable runtime.
- **Decision on redistribution:** FrostIDE bundles none of the Debugging Tools. Whether the online
  REDIST.TXT lists `dbghelp.dll` or `symsrv.dll` (for a newer symbol server client than the OS copy)
  is a legal question for the user (D36), and nothing below depends on the answer.

## Decision

1. **Natvis and large PDBs on Windows go through DbgEng, the System32 copy (DBG-14, R6).** FrostIDE
   gets a DbgEng-based debug adapter of its own, a separate process under `IProcessLauncher` that
   speaks DAP to `src/debug`. It loads `dbgeng.dll` only from System32, by absolute path, as the
   spike does. The adapter uses DbgEng's natvis support for the installed Visual Studio or Build
   Tools `stl.natvis`, for natvis files in the user's project and solution, and for FrostIDE's own
   files. DBG-4b ("Natvis engine completion") and DBG-8 ("Large PDBs and symbols") build on it
   instead of on a natvis engine of our own.
2. **lldb-dap stays the cross-platform backend and remains the Windows default until DBG-14 passes
   the DBG-1 suite.** It is the default on Linux and for clang-cl builds. Until the DbgEng adapter
   exists, R3's MSVC work stays on lldb-dap: DBG-2b's type-filtered throws (DebugSession, "Thrown
   types") and DBG-4a's STL display. A Debug build with an incremental link is handled as well (see
   7 below).
3. **Targets for R6 (PLAN.md, "FrostTests ... within the DBG-0 ADR targets"):** on FrostTests, the
   first breakpoint within 5 s warm and 15 s cold, and the adapter's peak private memory within
   1 GB. These are the spike's figures with headroom for the DAP layer, variable views and natvis.
4. **No redistribution.** Neither the Debugging Tools nor a DbgEng from a NuGet package or SDK are
   copied into FrostIDE's install. When System32's DbgEng lacks something FrostIDE needs, the
   product reports it as unavailable. It does not silently use another copy.
5. **No new dependency.** DbgEng and its SDK headers come with the operating system and the pinned
   Windows SDK, so `tools/licensecheck.py` gets no new entry.
6. **No macOS claim.** This ADR changes nothing outside Windows.
7. **Step Into with lldb on MSVC Debug builds (R2 follow-up).** The Windows launch turns off lldb's
   `step-in-avoid-nodebug`, and DebugSession steps out of functions without debug information
   itself. As a result, Step Into reaches the callee in a new Visual Studio project's Debug build
   (/ZI /JMC), where lldb's own step-in stepped over the call. It also reaches the callee in CMake's
   /Zi build. In both builds it stops on the callee's opening line, as Visual Studio does
   (`MsvcStepInto`; debug_session.hpp, "Step into a thunk").
8. **lldb cannot read a PDB that an incremental link rewrote (R3 integration, j2_msvc).** When link.exe
   relinks a Debug program in place (LinkIncremental, on by default for Debug), it leaves a PDB whose
   file size is not a whole number of MSF blocks: SLN-0's App.pdb was 1,677,312 bytes for 271 blocks of
   4096. lldb 23.1.4's PDB reader refuses such a file (llvm-pdbutil: "The PDB file is corrupt. File
   size is not a multiple of block size"), so lldb-dap reports "Symbols not found." for the program and
   every line breakpoint stays unverified. The same file padded with zeros to a block boundary reads
   fine, and the first, full link of the same project always reads fine. FrostIDE does not change the
   user's link settings or files to work around it. Instead, DebugSession prints a console note when
   the launched program loads without symbols, naming the incremental relink and the full link
   (LinkIncremental=false, or a Rebuild) that avoids it (`DebugSession.AProgramWithoutSymbolsIsNamedOnce`).
   j2_msvc's scratch copy links App with LinkIncremental=false (and /Od) so that its F5 steps exercise
   FrostIDE rather than this lldb limit. A fix (a reader that accepts the trailing bytes, or DbgEng
   under DBG-14) is a follow-up, and so is the user's choice of a workaround.

## Consequences

- R3 ships no DbgEng code in the product. Only the spike tool lives under `tools/dbg0`, and it is not
  built by CMake or staged.
- DBG-14 must pass the same DBG-1 scenarios on cl.exe builds. Where DbgEng differs from lldb
  (stepping, exception filters, the ThrowInfo type filter), it gets its own acceptance tests against
  the real engine.
- Until R6, a large FrostEngine session under lldb-dap takes 20 to 45 s warm to the first breakpoint
  and 1.7 to 2.6 GB. That is a known limit until R6, not a solved one.
