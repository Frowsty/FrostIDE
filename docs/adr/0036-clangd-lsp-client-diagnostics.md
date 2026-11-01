<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0036: The LSP client, clangd supervision and compile-command sources (W2)

- Status: accepted for `w2/clangd-diagnostics` (feature-first phase, ADR 0031). No budget, test or warning is
  weakened.
- Date: 2026-11-02
- Plan reference: PLAN.md section 2 ("Semantic engine": clangd, compile commands, VS solutions), 3.2 (`src/lang`,
  frost::lang, "LSP client, diagnostics store"), 3.3 (threads), 3.4 ("Diagnostics", "Completion"); ADRs 0017
  (ChangeSets, D8 TextChanges), 0021 (yyjson), 0032 (W1: the build error list's record), 0033 (project readers);
  feature row 98.

## Context
Work use needs live C++ diagnostics in the editor before M3's full language service. clangd is the semantic
engine (PLAN.md section 2). FrostIDE has no LSP client yet, and Visual Studio solutions have no
compile_commands.json, so clangd needs a database from somewhere.

## Decision
1. **Module.** `src/lang` (frost::lang, layer `lang`), Qt-free, on project, settings, text, io, sched and core.
   yyjson (MIT, already allowed by ADR 0021) is linked PRIVATE for JSON, so no new dependency is added.
2. **Protocol scope, written from the LSP 3.17 specification.**
   - The base protocol: Content-Length framing; the decoder resynchronises after malformed headers and skips
     oversized frames.
   - JSON-RPC 2.0: requests with ids, futures (`Task<Result<Json>>`) or callbacks, `$/cancelRequest` driven by
     stop tokens, notifications and batches.
   - Answers to server requests: workspace/configuration, window/workDoneProgress/create, capability
     (un)registration, the refresh requests, showMessageRequest, and applyEdit (refused for now).
   - The lifecycle: initialize, initialized, shutdown and exit, the shutdown within a time limit.
   - Capabilities: positionEncoding with "utf-8" preferred, clangd's offsetEncoding as the fallback, and UTF-16
     as the default.
   - Documents: didOpen, versioned incremental didChange built from frost::text ChangeSets (ADR 0017's
     extraction rule; full text when a batch's shape does not allow it), didSave and didClose.
   - Diagnostics: publishDiagnostics with versionSupport and relatedInformation, mapped to the snapshot of the
     version clangd saw (older versions are marked stale). The record carries the build error list's fields
     (file, line, column, severity, code, message).
   - Every member is thread-safe. One reader coroutine and at most one writer run per connection on the
     scheduler, and handlers run on the reader. Completion, definition and hover use the same `request()`.
3. **Process policy.**
   - Discovery: clangd is the `tools.clangd.path` setting (it must exist; there is no silent fallback), else
     the pinned LLVM from sdk/sdk.lock.json (FROST_LLVM_ROOT, FROST_SDK, then the bootstrap's default root),
     else a manual PATH walk. The result is always an absolute path started through IProcessLauncher, in its
     container (Job Object or process group).
   - Arguments: `--background-index --header-insertion=never --log=error --pch-storage=memory`, plus
     `-j=max(1, min(cores/2, 8))` (a first index build is commit-heavy, ADR 0008 decision 4; the M3
     ResourceGovernor will size it by free commit) and `--compile-commands-dir`.
   - stderr goes to frost::log (category "clangd"), and the last 64 lines are kept.
   - An exit nobody asked for is a crash. The restart waits 500 ms, doubling up to 30 s. The count resets after
     60 s of stable running, and the service gives up (Failed) after 5 consecutive crashes. Open documents are
     re-sent after each restart.
   - stop() sends shutdown and exit within 5 s, waits for the exit within the same limit, and kills clangd
     otherwise.
4. **Compile-command sources (the ICompileCommandsProvider seam, asked before every start).**
   - (a) An existing compile_commands.json in the root, build/, out/build/*/ or cmake-build-*/, in that order and
     by name order within each.
   - (b) A database synthesized for a .sln/.slnx from the static readers:
     - Sources are the SolutionModel's ClCompile items in the active configuration.
     - Flags come from each .vcxproj's folded ClCompile ItemDefinitionGroups: include directories, defines,
       language standard, runtime library, exception handling, conformance mode, forced includes and
       CharacterSet. References that stay unresolved are dropped.
     - The MSVC and Windows SDK include directories come from vswhere and the registry, passed as `/imsvc`.
     - The commands are clang-cl `--driver-mode=cl` commands, written to a cache directory only when they
       change.
   - (c) `GeneratorCompileCommandsProvider`, the hook for a command list from the user's own generator.
     TODO(M4): wire it to a configuration.
   - A chain provider takes the first source that has a database.

## Consequences
- clangd can diagnose CMake trees and Visual Studio solutions today. Synthesized flags approximate MSBuild:
  per-file metadata, property sheets, AdditionalOptions and PCH settings are ignored until MSBuild evaluation
  (M4).
- Deferred to the hardening phase (ADR 0031): fuzzing the frame decoder and JSON parser, sanitizers, stress
  tests of restart and cancellation, a ResourceGovernor-driven `-j`, and perf gates on didChange latency.
