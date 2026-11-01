<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0035: The W1 build service

- Status: accepted (feature-first phase, ADR 0031)
- Date: 2026-11-02
- Plan reference: PLAN.md section 3.2 (src/build), section 3.5 (trust, launching); ADR 0032 (W1); ADR 0034
- Supersedes: ADR 0034 decisions 3 (language) and 4 (what ends after MSBuild exits)

## Context
The Build menu, the configuration combo, the output window, the error list and the editor's marks need one
Qt-free owner of "the build": the open solution, its active configuration, one MSBuild run at a time with the
trust gate and Save All before it, and the diagnostics afterwards (`frost/build/build_service.hpp`). Building
it showed three problems in the ADR 0034 driver: localized output, processes a build step started on purpose
being killed, and a cancel that races MSBuild's exit being reported as a cancelled build.

## Decision
1. **One build at a time, no queue.** `build()` while a build is anywhere in the pipeline (NeedsTrust,
   SavingDocuments, Starting, Running, Cancelling) fails with `Busy`; so do switching the configuration or the
   solution. Visual Studio behaves the same, and a queue would need its own UI and cancel semantics.
2. **The service has one thread.** Pipeline steps and every callback run on it, in event order, one at a time;
   the UI marshals to its thread. The Save-All hook runs there too and completes from any thread, once; a
   failure ends the build before MSBuild starts.
3. **The active configuration** defaults to the first solution configuration named `Debug...`, else the first,
   and is persisted per solution path in `active-configurations.txt` beside the trust store (a header line and
   `<solution>\t<Configuration|Platform>` lines). It is user state, so it stays out of the solution's directory.
4. **One project builds through the solution's own target** (`Folder\Sub\Name`, cleansed as MSBuild does), so
   the solution's configuration mapping and build order apply.
5. **Output is English.** An empty `MsBuildRequest::uiLanguage` now means `en-US`, which also sets
   `VSLANG=1033` so cl, link and the other Visual Studio tools print English category words for the
   diagnostic parser. `DOTNET_CLI_UI_LANGUAGE` still switches MSBuild's console to UTF-8 (ADR 0034
   measurement). A caller that asks for another language gets it for MSBuild and keeps the user's `VSLANG`.
6. **After MSBuild exits only build infrastructure ends.** frost::io's container gains
   `containedProcesses`, `terminateContained`, `releaseContainer` and `abandonOutput`. startMsBuild's session
   ends `MSBuild.exe`, `MSBuildTaskHost.exe`, `mspdbsrv.exe` and `VBCSCompiler.exe` left in the job, releases
   the container when anything else survives (a server or tool a build step started keeps running, as in Visual
   Studio), and, if a survivor still holds the output pipe after a 1 s grace, stops reading it. Cancelling
   still ends the whole tree. Other BuildSession users keep ADR 0034's end-everything default.
7. **A cancel that loses the race against the exit is not a cancellation.** The session reports `cancelled`
   only when the exit status is the termination status (`io::kTerminatedExitCode`); a build that finished on its
   own keeps its own result.
8. **`$(VAR)` in `.vcxproj` Include paths** falls back to the process environment after the project's own
   properties (as MSBuild does); `VcxprojOptions::environment` injects another lookup. Unknown names stay in
   `unresolvedIncludes` and the warnings.

## Consequences
- Progress counts the `Project.vcxproj -> output` lines of minimal verbosity only; projects without an output
  (Clean, utility projects) are not counted and project starts are not reported. TODO(M4): exact events
  through FrostIDE.MSBuildLogger.
- A process a build step started outlives the build in a released job; it ends with its own lifetime, not
  FrostIDE's.
- When a caller picks a language other than English, localized category words are still not parsed (I12, M4).

## Alternatives considered
- Queueing builds: needs a visible queue and per-entry cancel for little gain; VS refuses as well.
- Persisting the configuration in a file next to the solution: writes into the user's tree, which may be
  read-only or under version control.
- Ending everything after MSBuild exits (ADR 0034): kills servers and tools a post-build step starts on purpose.
- Leaving the infrastructure running too: a PDB server or compiler server holding the output pipe would keep
  the build from finishing and would outlive it.
