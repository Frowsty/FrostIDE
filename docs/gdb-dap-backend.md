# GDB DAP backend preparation (DBG-13)

The Qt-free `frost::debug` backend starts a user-configured absolute GDB executable
out of process via its native DAP interpreter. It reuses the production DAP client
and DebugSession for initialization/configuration, breakpoint replacement, thread
and stack loading, scopes/variables, evaluation, stepping, disconnect and teardown.
No MI translation or scripted production responses are involved.

GDB must be version 16 or newer and built with Python/DAP. The factory probes
`--version` with a five-second deadline before opening the DAP transport; the DAP
initialize handshake then detects unavailable Python/DAP support. All init files,
auto-loaded object/repository scripts and debuginfod downloads are disabled.
GDB is user-installed, not redistributed or discovered through PATH.

Integration seams are `findGdbDap`, `gdbDapAdapterFactory`,
`startGdbDebugSession` and `attachGdbDebugSession`. The launch caller supplies an
inherited environment snapshot; GDB's launch `env` replaces the inferior block,
so the backend merges explicit overrides before serializing an object. Attach
uses the shared trust gate and detach ownership, with the Linux ptrace hint.

GDB 16's main-subprogram entry breakpoint does not meet DebugTarget's
first-instruction stop contract. Such launches fail explicitly. Source remapping
also fails explicitly because the GDB DAP launch schema has no sourceMap field.

The Linux `debug_gdb_tests` target requires `FROST_GDB_PATH` and drives the actual
compiled `debuggee.cpp` fixture through a breakpoint, threads/stack, local value,
step and normal exit. Missing GDB fails this target. Protocol/trust/version tests
are included in `debug_unit_tests`. Tests were authored but not executed during
implementation-first preparation; no build, WSL or debugger invocation was run.
The existing Settings dialog exposes **Native debugger** under Build, Execution,
Deployment / Debugger. LLDB remains the default; Linux offers GDB. Set the absolute
GDB executable in Toolchains / GDB. Both settings are application-only execution
settings, so an opened repository cannot select a different debugger. Ordinary
native launch, native attach configurations, Attach to Process and child debug
sessions follow this selection. Managed and explicitly supplied adapters retain
their own selection. Private engine launches require pinned LLDB and refuse GDB.

The production controller snapshots the actual process environment immediately
before calling the GDB session starter, then merges the resolved launch overrides.
Function and exception breakpoints enter the initial options before initialization.
Capabilities come from the actual GDB initialize response; GDB is never given LLDB's
verified native OS-thread identity provenance. The UI/settings admission tests are
authored only and runtime validation remains deferred.

Protocol references:
- https://sourceware.org/gdb/current/onlinedocs/gdb.html/Debug-Adapter-Protocol.html
- https://sourceware.org/pipermail/gdb-cvs/2024-December/059187.html

This is DBG-13 backend preparation, not a claim that R9 or Linux acceptance is complete.
