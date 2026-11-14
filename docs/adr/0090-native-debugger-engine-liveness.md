<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0090: Exact native debugger evidence and engine heartbeat liveness

- Status: accepted design; source implementation and independent review in progress; runtime acceptance pending
- Date: 2026-11-11
- Plan reference: R7 FER-9, DBG-11; builds on ADR 0087

## Context

Native all-stop suspends the engine's heartbeat thread as well as its Lua VM. Treating that known
native pause as an unexpected heartbeat loss revokes the authenticated Lua DAP peer and resumes its
Lua pause on detach. Merely increasing a timeout, fabricating a heartbeat, or equating a selected
native tab or PID with ownership would hide actual failures and allow stale debugger authority.

The GUI presentation state is also insufficient evidence. Sending Continue or Step can optimistically
show Running before the adapter reports an actual continued event.

## Decision

The engine session can mint one move-only `NativePauseLease` for its current authenticated connection.
Registration pins the full engine project/instance/generation/connection generation and positive OS
PID, together with one unique native registry session ID and controller generation. The app proves
that the native adapter uniquely matches that authenticated target, physical project, session epoch
and captured trust revision. Tab selection grants no authority. Reconnect cannot inherit the lease.

The publisher uses only actual adapter Stopped, Continued and Ended observations with a monotonic
adapter evidence sequence. Outgoing commands, successful request acknowledgments and GUI-only state
restoration do not create or clear pause evidence. Foreign bindings refuse; stale or out-of-order
observations have no effect. Controller deletion, generation replacement, binding ambiguity, project
or trust revocation, actual transport loss, and explicit release revoke the lease.

While the immutable binding has actual Stopped evidence, the session suspends **heartbeat expiry
only**. The last real heartbeat timestamp remains unchanged. Actual Continued starts one grace period
of the original negotiated heartbeat timeout on the unchanged live connection. A validated real
heartbeat clears that grace. Duplicate Continued events cannot extend it. Ended or explicit
revocation grants no continuation grace. Physical EOF, protocol errors, authentication failures,
trust revocation, write deadlines and per-command deadlines remain effective throughout a pause.

The Lua connection retains its independently authenticated R6 owner and bootstrap session ID. Inbound
DAP delivery can remain open during native all-stop; outgoing Lua DAP operations and engine mutations
refuse while the matching native adapter is actually stopped. Mixed Continue must first observe
actual native Continued and then admit the Lua operation. It never automatically resumes a separately
paused native debugger as a side effect of Lua Stop.

## Mixed pause provenance

A combined view uses actual Lua frames captured before the native pause and the actual native stack
from that same pause relationship. Lua stopped metadata supplies positive `vmGeneration`,
`stopGeneration`, and `osThreadId`; the Lua DAP coroutine/thread ID is a separate identity. Native
thread correlation requires a verified adapter mapping to actual OS thread IDs, plus the complete
registry/controller/engine/Lua bootstrap binding. Missing, partial, unsupported or stale evidence
refuses combined correlation. Cached frames are observations, never fabricated cross-language frames
or a permission to issue Lua requests while native execution is suspended.

## Private startup and ordering

The private Lua bootstrap and mutual HMAC DAP preauthentication retain their absolute 30-second lease.
A native stop during unfinished bootstrap/configuration cancels that attempt. A fresh bootstrap is
required after actual native continuation under the same admitted host; authentication deadlines are
not extended and no mutation is replayed when its outcome is uncertain.

A Lua launch waits for actual Attach success and actual configurationDone acknowledgment. Engine
command observations carry the serialized worker's real event-envelope cursor at the actual reply.
The app uses that cursor to reject delayed publication of a pre-start Edit event. Remote play.start
returns an accepted pending operation receipt with an exact operationId before dispatching actual
scene startup. This grants pending-operation cleanup only; a matching actual Play push promotes it
to the real play generation. Already-playing attach grants neither form of ownership. Guarded Stop
waits for real DAP detach acknowledgment, then sends expectedOperationId and the actual generation
when known. Completion requires the exact operation's actual terminal Edit phase (cancelled, failed
or completed), and actual failures are reported. Old Edit events or a replacement operation cannot
complete or inherit this lease. Replies never invent engine state or process ownership.

## Verification

Real loopback protocol tests cover heartbeat silence while stopped, unchanged command deadlines,
actual continuation grace, real heartbeat recovery, stale/foreign evidence, duplicate bindings,
revocation, trust loss, physical EOF and reconnect without inherited authority. App protocol tests
hold actual filesystem publication to exercise the reply/event ordering race and verify zero command
transmissions while admission is refused. These are protocol tests, not native-process or VM oracles.

Mandatory real-engine acceptance must hold an actually verified native all-stop for more than 60
seconds and beyond the negotiated heartbeat window, preserve the exact Lua pause/VM/thread identities,
prove zero successful Lua DAP writes during native stop, and observe actual native continuation before
Lua continuation. Runtime evidence is required before this feature is accepted.
