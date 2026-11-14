<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0087: Persistent FrostEngine sessions and schema command admission

- Status: accepted design; implementation and runtime acceptance pending
- Date: 2026-11-10
- Plan reference: PLAN.md R6, FER-1b through FER-7, FEC-3 and RUN-11
- Builds on: ADR 0078 (definition of done), ADR 0080 (test automation endpoint)

## Context

The one-shot engine helper cannot retain push events, identify a restarted engine, or distinguish a
long main-thread command from a dead process. R6 requires a persistent authenticated connection,
schema-driven command admission, real state on the toolbar, logs and diagnostics, project content,
and editor/player run configurations. The engine contract and all production clients must migrate
together. R5 verification remains isolated while R6 sources are developed in separate worktrees.

## Decision

`frost::engine::EngineSession` owns the Qt-free connection. The app owns selection, trust, process
lifetime, GUI dispatch and policy context. The engine's AutomationServer owns the protocol; editor
and player adapters publish actual host observations without introducing an Automation dependency
into the lower Runtime layer. The player adapter belongs to the FrostRuntime application.

### Authentication and identity

Contract v2 uses mutual HMAC proofs with fresh cryptographic nonces and distinct client/server
roles. Proofs bind the canonical project, instance, generation, protocol capabilities and heartbeat
parameters. The engine documentation specifies the exact transcript. The IDE uses the existing
pinned libsodium dependency. Authentication secrets may be delivered through protected discovery
or private startup channels; they never enter ordinary console output, run configuration storage,
diagnostics or journey evidence. Player startup metadata contains no secret.

V2 is the default. Legacy bearer authentication requires explicit engine compatibility opt-in;
clients never retry a failed v2 challenge using a bearer token. `frostctl`, screenshot tooling and
the production IDE helper migrate with the engine. The separate Lua DAP protocol is not changed
by this decision. Authenticated host role is reported separately from protocol capabilities.

Every post-authentication request and pushed observation is bound to the selected project,
instance and generation. A project change or server restart invalidates the old binding. Reconnect
re-reads the selected trusted discovery record and authenticates again; it never replays a command
whose outcome is uncertain or silently chooses another instance.

Automatic reconnect is limited to the surviving selected instance. A full external process restart
creates a new instance ID: Refresh, select the replacement, and Pair explicitly establish its new
binding. An IDE-owned run restart binds the new private startup record. The Workbench acceptance
must distinguish the chosen replacement from a second instance of the same project.

### Liveness, ordering and cancellation

Commands continue to run on the engine main thread. Heartbeats are produced by the network thread
and use a separate envelope from sequenced state/log events. The client measures receipt using its
own monotonic clock. A subscription snapshot establishes the event baseline. Queue and frame
budgets bound both sides; overflow or malformed identity fails the connection rather than making
up state. Requests retain their actual success or failure independently of heartbeat progress.

The session has separate receive/watchdog work and serialized callbacks. Callbacks marshal to the
GUI and cannot destroy their own session. Shutdown cancels and joins workers. Cancellation after
transmission reports an uncertain engine outcome; it cannot promise rollback. Cancellable project
CLI work terminates its contained child and discards stale results. Trust revision, project and
generation are checked before process launch, command transmission and result publication.

### Command policy and user interface

An immutable `CommandPolicy` owns the authenticated schema and its project/session binding.
Commands must explicitly opt into IDE use through schema metadata. Missing metadata, unknown
parameters, unavailable capabilities or commands, unsuitable state, revoked trust, or mismatched
identity deny transmission. Mutation permission is a separate required observation. Admission is
rechecked immediately before each send; a previous successful check grants no lasting permission.

Toolbar state comes from authenticated engine observations. Button clicks do not synthesize play
or pause state. An unresponsive engine main thread does not imply a native debugger pause. The
console retains bounded actual logs and source frames; engine diagnostics have their own Problems
source. Paired processes remain externally owned. IDE-owned runs use the existing RunManager and
contained process Stop. When a native DAP session matches the authenticated engine PID, the app
overlays its actual stopped/continued state without modifying engine push observations. Engine
Stop uses DAP terminate where supported, or an explicitly terminating disconnect for a launched
debuggee; an attached target without a supported termination path is refused. Ordinary debugger
detach semantics remain unchanged. R7 adds the dedicated engine debug configurations.

The Engine view derives content from the actual project and asset CLI results, preserving exact
64-bit handles. Scene assets come from `assets.list`; `scene.list` describes open scenes only.
Format-1 `.frostproj` does not declare project modules, so the view reports that limitation rather
than substituting Lua API namespaces. Editor and player configurations share the existing run
model. Debug variants remain R7 scope.

## Verification and consequences

Engine changes must pass the engine's Windows Debug tests, warning-free Release build/tests,
WSL tests, formatting and independent review before landing on engine main. Only then may the
IDE corpus lock move to that main SHA. Existing pinned corpus and live user checkouts are not
modified to make acceptance pass.

IDE acceptance includes real-engine authentication rejection and mutual proof validation,
twenty pause/resume cycles plus stepping, two-instance selection and restart, zero transmitted
frames for a policy-denied command, actual logs and script error locations, exact CLI content
comparison, heartbeat survival during measured main-thread work, bounded disconnect after process
death, and owned editor/player lifecycle. The Debug-only `j4_engine_session` journey exercises
the real IDE. Missing tools or contract fields fail these mandatory cases. Targeted tests,
independent Astra reruns, the full Windows/WSL corpus gates and orchestrator UAT remain required;
authored tests and static review alone do not complete R6.

R6 work uses separate IDE and engine worktrees while R5 verification runs. Heavy verification
remains serialized to avoid resource contention. Verified R5 changes must be integrated before
R6's combined gate. ADR 0086 belongs to the incoming R5 inspection work.
