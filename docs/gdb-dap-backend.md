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
Production run-configuration/UI backend selection remains integration work.

Protocol references:
- https://sourceware.org/gdb/current/onlinedocs/gdb.html/Debug-Adapter-Protocol.html
- https://sourceware.org/pipermail/gdb-cvs/2024-December/059187.html

This is DBG-13 backend preparation, not a claim that R9 or Linux acceptance is complete.
