# ADR 0091: Phased native and Lua engine launch

- Status: Accepted for implementation; source-only, runtime verification pending
- Date: 2026-11-11
- Plan: R7, RUN-11 Debug, FER-9, DBG-10 and DBG-11

## Context

Linux Yama permits the debugger parent to trace its launched child, while an independent debugger may be refused permission to attach to a sibling engine. The mixed engine workflow therefore launches the actual engine under the pinned native adapter, then authenticates and attaches the Lua debugger to that same process. A generic compound that prepares every endpoint before starting any member cannot satisfy this dependency.

The engine startup record and transient launch environment contain credentials. Generic debugger output, adapter diagnostic logging and protocol traces must not receive them. A native DAP host is not a RunManager host: manufacturing a RunId would grant incorrect process ownership.

## Decision

Typed engine configurations have an explicit EngineDebugMode: Lua or NativeAndLua. Lua remains the default. NativeAndLua is one host package containing two debugger sessions, not a relationship guessed between executable names. Its store format is version 4; existing Lua-only engine stores remain version 3 and ordinary stores retain versions 1 or 2. Run behavior remains the ordinary owned engine launch.

All members finish trusted build, before-task and canonical launch preparation before any host starts. PreparedDebugLaunch gains an optional asynchronous phased start; ordinary synchronous launches keep their existing path. A phased engine member launches its real executable through the built-in pinned native adapter with launch ownership. It waits for the actual DAP process identity, configuration acknowledgment, private engine endpoint and mutual HMAC authentication before preparing Lua. The authenticated processId must equal the native DAP PID. Project, role, instance, generation, controller generation and trust admission remain bound throughout.

Private output routing is installed before native launch. Raw engine streams, adapter stderr and launch protocol diagnostics containing the transient environment are never copied to ordinary consoles or traces. Authenticated, sanitized engine pushes continue to feed Engine Console. The Player uses transient FROST_SCRIPT_DEBUG_WAIT=1 and creates its VM only after actual authenticated Lua attach and configurationDone acknowledgment. No credential is placed in command-line arguments or saved configurations.

NativeEngineHostLease identifies the native registry session, controller generation, group and actual process identity separately from a RunManager RunId. Rollback or explicit owned-host Stop acts only on that current native launch. External engine/native attaches retain ordinary detach semantics and survive Stop. Lua attach ownership never silently becomes host ownership.

A user native stop during startup remains pending and is never automatically continued. An incomplete Lua authentication attempt is cancelled, its peer/factory is closed, and its late callbacks are rejected by attempt generation. A fresh bootstrap may begin after actual continued evidence under the same host lease. Frozen protocol deadlines are not extended. A mutation already written before a pause is treated as uncertain unless its actual response proves ownership; it is not blindly retried.

## Verification

Source contracts and refusal/lifecycle tests accompany implementation. Acceptance requires the actual pinned LLDB launch of FrostEditor on Linux, mutual authenticated PID equality, actual native and Lua frames for the same script pause, native all-stop beyond the negotiated heartbeat timeout, and actual native continued evidence before Lua continuation. Windows also retains a separately launched external-editor attach/detach survival case. Missing pinned tools, unsupported private plumbing, identity mismatch, cancellation and partial launch failures must fail without fabricated sessions, frames or counters. No runtime acceptance is claimed by this ADR.
