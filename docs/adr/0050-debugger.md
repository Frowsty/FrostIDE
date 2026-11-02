<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0050: The U1 debugger over lldb-dap

- Status: accepted (feature-first phase, ADR 0031)
- Date: 2026-11-02
- Plan reference: PLAN.md M4 (debugger), section 3.5 (trust, launching); ADR 0040 (run service); ADR 0043 (QRhi editor)

## Context
U1 needs Rider's everyday debugging on the run configuration Run already resolves: breakpoints in the gutter,
a stop on them or on a crash, stepping, frames, variables, watches and Evaluate. The pinned LLVM release ships
lldb-dap, a Debug Adapter Protocol server that debugs MSVC (PDB) and Linux (DWARF) programs.

## Decision
1. **Client.** `frost::debug` speaks DAP 1.71 over the adapter's stdio: Content-Length framing and the JSON
   reader moved from `frost::lang` to a new `frost::rpc` module that both use. `IDebugAdapterClient` matches
   responses by seq on a reader thread, completes every pending request with an error when the stream ends
   (with the adapter's exit code), sends `cancel` when the adapter supports it, and declines reverse
   requests (`runInTerminal`, `startDebugging`) as Unsupported. The DAP types are hand-written for the
   requests used; TODO(M6) generates them from the schema.
2. **Session.** `DebugSession` runs initialize, launch, then on `initialized` setBreakpoints per source and
   configurationDone. It keeps the state (Starting, Running, Stopped, Ended), the stop's reason, the threads,
   frames loaded 20 at a time for the stopped thread only, and the selected frame; a crash (exception or
   signal) selects the first non-subtle frame with a source. Scopes, variables (paged) and evaluate are
   asynchronous requests. Run to Cursor adds a temporary breakpoint that is removed at the next stop. Stop
   sends disconnect with terminateDebuggee; the controller kills the adapter's tree after 3 s.
3. **The fake-dap double.** `FakeDebugAdapter` is an in-process transport with scripted replies, and
   `scriptProgram` scripts a whole debuggee (breakpoint lines, steps, a crash, paged arrays, evaluate), so
   every request and event is tested without lldb-dap. One integration test (label `lldb`) drives the real
   pinned lldb-dap against a fixture program and skips when it is not installed.
4. **Finding and starting lldb-dap.** The setting `tools.lldbDap.path` (absolute) wins; otherwise the SDK's
   `lldb-<pinned>/bin/lldb-dap` under `$FROST_SDK`, `E:\sdk` and `~/sdk`. PATH is never searched. It starts
   through `IProcessLauncher` with `--no-lldbinit`, stdio pipes and killOnClose.
5. **Environment rule.** `_NT_SYMBOL_PATH`, `_NT_ALT_SYMBOL_PATH` and `DEBUGINFOD_URLS` are removed from
   lldb-dap's environment (and NoDefaultCurrentDirectoryInExePath on Windows, as for Run), so a machine-wide
   symbol server never makes a launch download symbols for every system DLL. The program gets the run's
   arguments (split with the CRT rules), working directory and environment; an unmerged environment adds
   `settings set target.inherit-env false`.
6. **Trust.** `startDebugSession` refuses with Untrusted, before any adapter starts, when a trust root of the
   solution (or the program's folder) is not trusted.
7. **Debug from the run configuration.** `RunService::debug` saves, builds and resolves exactly like Run, then
   hands the command, subsystem and trust roots to the debugger instead of starting the program; the Run
   console ignores such runs. `run.debug` is Shift+F9, Stop is the shared `run.stop` (Ctrl+F2), and the
   debugger's actions use the IntelliJ ids and keymap rows (F7, F8, Shift+F8, F9, Alt+F8, Alt+F9, Ctrl+F8,
   Alt+5). There are no Visual Studio F5/F10/F11 aliases.
8. **Breakpoints.** `BreakpointManager` holds line breakpoints (one per line, Windows paths compared without
   case) with the adapter's verdict. They are persisted per root in `.frost/workspace.json` as
   `"breakpoints": [{"file": "rel/path", "line": N}]`, keeping the file's other bytes.
   `DocumentBreakpointTracker` keeps one anchor per breakpoint line, so breakpoints move with edits and merge
   when lines join. The gutter draws a filled red disc (verified, or no session), a hollow grey ring with the
   reason as tooltip (unverified), and the execution arrow with the stopped line highlighted. These are
   EditorDisplayList kinds, so the raster and QRhi backends draw the same thing. The colours derive from the
   marker colours; TODO(M6) adds theme keys.

## Consequences
- The debugger ships as libraries (`frost::debug`, `frost::ui_debug`) with the Debug tool window, the Evaluate
  dialog and the actions. The app wires them in the same slice that wires RunService into the window.
- M6 adds attach, conditional and function or exception breakpoints, the Breakpoints dialog, Set Value, inline
  values, data renderers, a symbol server setting with a cache, and the debugger console's command line.

## Workbench integration update

The MSBuild RunService hands its resolved command and trust roots to the debugger after saving and
building. CMake uses its own per-request build completion and the completed model; it refuses a
profile change or revoked trust before starting the adapter. The controller invalidates queued
launches and closes the adapter when the workbench shuts down, and an active debug session prevents
closing its project. Both backends use the same Debug window, editor overlays and Stop action.

On Windows, when the adapter advertises function breakpoints, the debugger installs one on `abort`
and recognizes its returned breakpoint id as an abort stop; selecting a non-subtle source frame
shows the user call site. The first `threads` request immediately follows `configurationDone` to
consume lldb-dap's cached launch-time list (ADR 0049).

Windows private-desktop tests cover the RPC and language transports, debugger unit and UI layers,
real lldb-dap, and Workbench MSBuild and CMake build/save, breakpoint, evaluate, step and stop.
They also cover a real MSBuild `abort()`, a delayed CMake profile switch and shutdown while the
build waits for Save All. The same focused debugger and CMake Workbench tests pass natively in
Linux with the pinned lldb-dap.

The Windows R2a debugger step was exercised against Notepad++ v8.9.8.1 at pinned commit
`dd40fe45598791e9aa38f3aeb200e10fb3d6338a`, built with MSVC Debug|x64. A private-desktop Workbench
run bound the breakpoint at `wWinMain` line 641, displayed its stack and `pCmdLine`, stepped into and
out of `parseCommandLine` and past `doPrintAndQuit`, read the `isMultiInst` watch and false Evaluate
result, continued to a visible Notepad++ window, paused with seven threads, and stopped both the
debuggee and its adapter. The run used isolated profile, settings, trust and session storage; it
removed the temporary project user settings and workspace files. This covers the debugger step, not
the other steps or exit criteria of the full U1 scenario.
