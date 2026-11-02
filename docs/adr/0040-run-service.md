<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0040: The W1 run service

- Status: accepted (feature-first phase, ADR 0031)
- Date: 2026-11-02
- Plan reference: PLAN.md section 3.2 (src/build: run configurations), section 3.5 (trust, launching); ADR 0032 (W1); ADR 0035

## Context
W1 users need Visual Studio's Ctrl+F5: build the startup project, then start its program with the debugger
settings from the project and its `.vcxproj.user` file. The debugger itself is M4. What to start comes from
MSBuild properties that imported `.props` files can change, so a static read of the project is not enough.

## Decision
1. **MSBuild reports what to run.** `MSBuild.exe <project> -getProperty:TargetPath ... -p:Configuration=...
   -p:Platform=...` (MSBuild 17.8+) evaluates the project without running a target and prints JSON. One call
   asks for TargetPath, TargetDir, ProjectDir, SolutionDir, ConfigurationType and the five
   `LocalDebugger*` properties, with the `Solution*` properties Visual Studio passes. The answer is cached per
   project file and configuration until the `.vcxproj` or `.vcxproj.user` changes. An MSBuild without the
   switch (MSB1001) falls back to a static read of both files over MSBuild's C++ defaults, with a warning.
   SubSystem is link metadata, not a property, so the console mode reads the PE header of the built program.
2. **Expansion and defaults follow Visual Studio.** Arguments, working directory and environment get
   `$(Macro)` expansion (the reported properties, then environment variables; unknown names are empty), the
   environment values `%VAR%` expansion. The command defaults to `$(TargetPath)`, the working directory to
   `$(ProjectDir)`. `LocalDebuggerEnvironment` lines are merged over the IDE's environment unless
   `LocalDebuggerMergeEnvironment` is false. The argument string is split with the MSVC CRT's rules and
   re-quoted for CreateProcessW; shell redirections in it are not interpreted.
3. **Console modes.** GUI programs start with no console. Console programs start, by default, in an external
   console window (`useExternalConsole`, persisted per solution): `cmd.exe` (absolute, from the system
   directory) runs `/d /s /c ""<program>" <args> & ... echo Process finished with exit code N & pause & call
   exit N"`, so the window stays open and the exit code survives. frost::io gains `ProcessSpec::rawArguments`
   (a verbatim Windows command tail) and `ProcessSpec::newConsole` (CREATE_NEW_CONSOLE, all streams Inherit);
   both are Unsupported on POSIX. The other mode is FrostIDE's Run console: stdout and stderr piped and shown
   as UTF-8 chunks, an input line writing to stdin. Programs that buffer piped output show it late there, which
   is why the external console is the default.
4. **Runs.** Build first through BuildService (its trust prompt and Save All); a failed or cancelled build
   starts nothing. A project the configuration does not build runs its last build after the trust check. One
   run per project: a second Run is `Busy`, which the UI turns into "stop and rerun?". Stop ends the whole tree
   (Job Object or process group); after a natural exit, processes the program started live on. The exit code
   and run time are reported. Off Windows, MSBuild solutions are `Unsupported`.
5. **Startup project** defaults to the first Application project in solution order, else the first loaded C++
   project, and is persisted per solution in `run-settings.txt` beside the build service's state.
6. **Keys.** Run is Shift+F10 and Stop Ctrl+F2 (IntelliJ); Ctrl+F5 is bound to `run.runWithoutDebugging` as
   a Visual Studio alias in the same table (unbound on macOS, where Meta+F5 toggles VoiceOver).

## Consequences
- Each run costs one MSBuild evaluation (about a second) the first time per configuration.
- The Run console is not a terminal: no PTY, no colours, no carriage-return overwriting (the terminal is M5).
- Run configurations beyond the startup project, CMake targets and the debugger come with M2-M4.
