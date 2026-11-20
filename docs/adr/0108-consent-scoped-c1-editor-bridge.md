# ADR 0108: Consent-scoped C1 editor bridge and Agent Terminal

- Status: Implementation authored; runtime acceptance pending
- Date: 2026-11-20

## Context

PLAN section 5.1 C1 requires the user's unmodified installed CLI, with an IDE editor bridge.
This is distinct from native chat, autonomous prompts, provider APIs and C2 model tools/hooks.
No Claude process, provider connection, prompt or workspace transfer is executed during this slice.

## Decision

An explicit per-window action asks for workspace editor/selection/diagnostic consent after L1 and
project trust. Only then does AgentBridge bind an OS-assigned IPv4 loopback port. It implements
bounded WebSocket text framing over the existing QtNetwork dependency, the mcp subprotocol and
JSON-RPC/MCP discovery/initialization. The token is 128 random bits from the system generator;
Origin requests are rejected and token comparison accumulates differences across the full secret.
Discovery files have a current-user DACL on Windows or mode 0600 in an owned 0700 directory on Linux.
Only stale owned FrostIDE discovery records with demonstrably dead processes are cleaned.

Each window has its own token, roots, clients and diffs. Consent expires after eight hours, on project
switch, trust revision change, explicit disconnect or teardown. Admission is checked before every
request/notification and mutation. CLI probes bind a monotonic consent generation and cancellation
flag, and recheck the captured workspace's live trust immediately before either launch.

Agent Terminal asks the user to select their separately installed native CLI, probes --version and
auth status --json through IProcessLauncher, and launches the same executable in the existing PTY
with --ide and CLAUDE_CODE_SSE_PORT. No shell shims, login command, prompt, hooks, credentials file
or OAuth relay is used. A CLI below 2.1.316 requires a terminal-only confirmation; missing sign-in
requires a separate offer for the user to sign in interactively. Native C3 subscription auth remains
outside this implementation. No installer or dependency is added.

C1 tools admit physical existing regular files only within this window's consenting root, rejecting
credential/keychain names and escaping aliases. Windows accepts WSL /mnt/drive spellings only after
the same physical containment check; there is no all-interface listener or WSL installation action.
Read-only cached diagnostics expose their cache state; post-edit queries wait at most 1.5 seconds for
real engine snapshot versions. Open diffs are nonblocking editable shared-editor views. Only the same
physical source/target file is accepted. Apply checks local dirty state and source snapshots, then
atomically replaces bytes; it does not claim an exclusive cross-process writer lock. SaveDocument
asks separately. Completion is sent once as FILE_SAVED, DIFF_REJECTED or TAB_CLOSED. Closing tools
address only agent-owned diff windows, never arbitrary user tabs. Selection notifications are bounded
and sampled at 75 ms; at-mention uses the existing action id. Connected client count is shown locally.

## Validation and remaining acceptance

Offline fixtures cover no-consent listener refusal, discovery lifecycle, cross-window token rejection,
Origin rejection, initialization, masked-frame admission, revocation and physical resource boundaries.
These tests are authored, not run. Astra source review is in progress. Real CLI handshake/version/auth
schema compatibility, editor completion payloads, Windows ACLs, WSL mapping, negative listener tests,
Qt rendering and lifecycle behavior still require the resumed local acceptance phase. No milestone
exit, successful connection, portability or provider behavior is claimed from source review.
