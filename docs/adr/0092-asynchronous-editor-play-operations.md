<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0092 — Asynchronous editor Play operations

- Accepted for source implementation on 2026-11-11
- Runtime verification pending
- Covers R7 FER-9 and DBG-11 with ADRs 0087, 0090 and 0091

## Context

The editor starts its runtime scene synchronously inside `play.start`. Lua `OnCreate` executes before that call returns. A real debugger entry pause can therefore hold the RPC handler beyond the existing five-second play-command deadline. The response cannot grant committed Play ownership while scene initialization can still fail.

## Decision

Remote `play.start` accepts one deferred operation while the editor has an open project and scene. Its receipt contains the actual current state together with `accepted=true`, `pending=true`, `startedNow=false`, an opaque 32-character lowercase hexadecimal `operationId`, and `phase=pending`. Its Play generation remains empty. A second pending start refuses. An already-running Play returns `accepted=false`, `pending=false` and `startedNow=false` and grants no new ownership.

The editor queues the RPC response before its normal `Update` safe point dispatches the real transition. Its network writer can send that queued response and real heartbeats while Lua pauses. The hook continues to pump its restricted debugger and OS work. Direct in-engine `StartPlay` remains synchronous. A local start supersedes a queued remote operation and cannot inherit its cancellation guard.

Authoritative `play.state` carries `operationId`, `phase` and the actual `playGeneration`. The phase vocabulary is `pending`, `playing`, `cancelled`, `failed` and `completed`. A committed scene start publishes `playing` with the fresh real generation. Cancellation before dispatch publishes actual Edit with `cancelled`. A failed transition publishes actual Edit with `failed` and retains its `operationError`. Stopping committed Play publishes actual Edit with `completed`. Terminal identity and failure text remain available until replacement. Before dispatch, the host revalidates the project engine generation, unique scene-opening identity, actual scene-start counter and automation identity. Committed ownership continues to validate the captured project, engine, scene opening and automation identity together with its actual runtime scene and VM generation. Token rotation, server restart and replacement revoke that operation's cancellation authority.

The IDE may own cancellation of an accepted pending operation bound to its exact authenticated engine, physical project and captured trust revision. It promotes that ownership only after an actual matching Play push supplies the committed generation. Cleanup first receives the real Lua detach acknowledgment, then sends `play.stop` with `expectedOperationId` and, when known, `expectedPlayGeneration`. Both guards must match the current operation. A foreign or replaced operation refuses without stopping the replacement. Completion requires the matching operation's terminal Edit push.

The five-second RPC deadline remains in force. The direct local toolbar Play and Stop APIs retain their existing execution behavior. The remote workflow uses the accepted operation and authoritative pushes.

## Verification

Host regression sources cover acceptance before scene startup, duplicate refusal, exact queued cancellation, committed-generation ownership, direct local replacement, runtime VM replacement, and retention of a real settings-validation failure. A real Lua-hook harness holds `OnCreate` beyond five seconds after the receipt has returned, verifies pending ownership during that pause, detaches, and performs the exact guarded stop after user-call unwinding.

The real pinned-engine acceptance must additionally exercise mutual authentication, a returned pending RPC receipt, an initial `OnCreate` pause beyond the request and heartbeat deadlines, acknowledged Lua detach, exact cancellation, and the actual matching terminal Edit push. Source and formatting checks provide no runtime acceptance.
