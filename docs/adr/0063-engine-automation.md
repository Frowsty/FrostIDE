# ADR 0063: FrostEngine automation

Status: bounded automation and experimental Lua integration; broader acceptance remains open.

FrostEngine publishes per-project discovery records containing loopback endpoints and, when enabled,
credentials. FrostIDE uses the existing project trust decision before reading a record or issuing a
command. A contained, absolute-path FrostIDE helper performs discovery, the complete engine
conversation and physical path validation only after that check. Credentials remain in the helper;
its private stdout contains at most 16 MiB of result JSON and never enters logs. The async process has a
deadline and a stop token. Cancellation destroys it without waiting for its exit; timeout requests
termination and waits at most the async process's five-second grace. The reactor reaps a POSIX child
after it exits; a kernel-stalled child may remain until the kernel releases it or the app exits.
Each explicitly requested operation opens a fresh IPv4 loopback-only connection in the helper.
It authenticates, checks the server contract and the live `project.info` directory, and
only then sends one allowlisted command. The transport uses newline-delimited JSON with a shared
deadline, cancellation and a 16 MiB line limit. Credentials and server-supplied error text do not
enter the UI or logs. The user can run play controls, validation, tests, log retrieval and editor
focus from a FrostEngine tool window. Validation sends an immutable snapshot of the current buffer.
Log and test locations are resolved to Lua files inside the project, and resolved again in a helper
when activated. The workbench retains request handles, cancels them and shuts down its reactor before
draining the handles and destroying the scheduler. Output retains at most 10,000 rows and 4 MiB
of UTF-8 display text; this excludes widget, QString and navigation-metadata overhead. Older entries
are evicted with an explicit notice and log pagination uses the engine's `nextSince`.

Cancellation and timeout close the client connection; neither guarantees that an engine operation
already received has stopped. The UI reports that uncertainty and never retries commands implicitly.
The engine protocol has no atomic project-identity precondition on a command, so the live identity
check immediately precedes the command but cannot prevent a project switch in the engine between
those separate requests.

The engine protocol does not make the identity check and a later action atomic. A filesystem or
kernel that does not complete cancellation can also delay OS-level process cleanup after the IDE has
stopped waiting; the IDE does not claim the engine action was rolled back.

The experimental Lua path reserves launch admission before asynchronous discovery and authentication,
then rechecks the reservation, project identity, trust and debugger state after reentrant callbacks.
Build, CMake configuration, project edits and managed debug preparation share that admission state.
The adapter exposes captured stack/source data and pause/continue. Breakpoints, variable inspection,
evaluation and stepping remain unsupported. Disconnecting an attached debugger does not terminate
the engine process.

Owned play Stop carries the exact engine operation identity and only completes the matching session.
An uncertain transport outcome retains cleanup ownership until reconciled; an explicit refusal is
reported without pretending that play stopped. The accepted real-host scenario is a paused main Lua
stack: the workbench stops its owned play session, reaches Ended, completes cleanup and leaves the
host usable in Edit state. The tested FrostEditor then exits cleanly through its own window close.
Running and coroutine Stop are not accepted by this slice. Source and platform evidence is recorded
in the 2026-11-03 Lua entry in `docs/progress.md`; this is not complete Lua debugger acceptance.
