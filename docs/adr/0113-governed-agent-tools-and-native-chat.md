# ADR 0113: Governed C2 tools and native C3 session boundary

- Status: Implementation authored; independent source recheck and runtime acceptance pending
- Date: 2026-11-20

## Context

PLAN section 5.1 separates C1 end-user CLI terminals, C2 model tools/hooks, and native C3 chat.
Subscription authentication in native code requires recorded written provider approval (D10).
The implementation-first instruction currently prohibits builds, test/feature executions, provider
processes, data transfers, installs, downloads and WSL execution. Synthetic protocol fixtures cannot
establish compatibility with a real CLI release.

## Decision

Add the already-planned Qt-free agent layer for auth/launch policy, bounded NDJSON framing,
capability-gated controls, pending-control deadlines, MCP result limits and Stop-hook loop policy.
Production kSubscriptionApprovalRecorded remains false. Subscription, unknown and logged-out auth
fail closed. Each explicit native Connect probes auth status afresh, then the native process-start
API applies the same compiled auth gate. Accepted API-key/apiKeyHelper/Bedrock/Vertex routes carry
--bare, --ide, --settings and --mcp-config, with this window's IDE port and checkpoint environment.
There is no runtime subscription-approval setting, credential reader or unattended user turn.

Native built-in file/shell tools are intentionally absent from the launch tool allowlist. Only
AskUserQuestion and ExitPlanMode are built-ins; --strict-mcp-config selects the explicit frostide
server. Project reads, proposed edits, build/test/debug actions and language queries go through C2.
A host permission callback is not assumed to intercept commands that the CLI's merged permissions
might auto-allow. Broad native tools require a separately reviewed, real-CLI proof of per-call
admission. Agent Terminal remains the user's unmodified CLI mode with its existing C1 policy.

C2 is a separate frostide-mcp executable using stdio NDJSON and the existing current-user named pipe
or AF_UNIX transport, never TCP. Every IPC envelope binds token, instance and physical workspace.
Discovery, MCP config and hook settings are create-new private files: current-user DACL on Windows,
0600 files in an owned 0700 Linux directory. Reads verify the open handle's ownership/privacy and
reject reparse/symlink files and multiple links. External cwd discovery refuses ambiguous matching
windows. Explicit per-window configs select an exact instance. Dead owned runtime records are swept;
only files successfully created by the current operation are removed on failure or disconnect.

Tools query actual existing IDE services. LSP definitions/references/symbols/hierarchy preparation,
real diagnostics, loaded project models, build outcomes, debugger state and current CTest reports are
used; unavailable services return errors. Opaque LSP data and escaping resource locations are not
returned to the model. Diagnostic hooks compare refreshed edited-file T1 results with captured
pre-edit error baselines. Missing/stale evidence produces a warning rather than an invented error.
The Stop guard honors stop_hook_active, blocks at most three times for one new-error fingerprint,
then warns. Hook settings are additive inputs to the CLI; they do not rewrite user/project settings.

Mutations become explicit cancellable MCP tasks. Concrete edit before-images are displayed with
shared diff editors and Apply/Cancel before the existing multi-file transaction engine runs. Dirty
aliases retain their actual editor identity; final admission rechecks consent, task, physical target
identity and before-images. C++ rename retains its existing semantic receipt checks. Test tasks bind
actual CTest run generations so delayed polling/cancellation cannot affect later user-started runs.
Build tasks report actual build outcomes; debug commands report acceptance and actual observed state,
without asserting that a requested DAP action has completed. Standard debugger changes produce local
MCP list-changed notifications through bounded proxy polling.

Four tools are always-load. Results use conservative 12,000-byte pages, below the character limit
and a byte-fallback token bound; metadata for workspace diagnostics/project model permits at most
500,000 characters, but no large single response bypasses paging. Instructions are a fixed short
string. Input lines, control frames, event queues, retained pages/tasks, stderr and UI transcript
storage all have explicit finite bounds. No new external dependency/license is introduced.

The native panel requires explicit provider/cost consent and Connect, waits for actual compatible
CLI/IDE/MCP initialization, and transmits a user turn only when Send is pressed. It supports bounded
question cards, concrete plan review, capability-gated interrupt/model/permission/context controls,
actual provider cost display, own private session identity index and defensive read-only JSONL
viewing. CLI checkpoint rewind first requires dry-run evidence and scoped file review before write;
fork reconnects with fresh auth and submits no automatic prompt. CLI rewinds do not pretend to undo
MCP transactions: those use the IDE's shared Undo/recovery path. Native built-in file tools are
currently disabled, so fresh native file checkpoints are normally unavailable by design.

## Explicit current scope and acceptance gaps

Hierarchy adapters expose actual prepared roots and immediate incoming/outgoing or super/subtype edges through existing LSP services, with bounded flat records and workspace admission per reply. They do not claim transitive trees. inspection_info returns exact source-owned analyzer registry metadata and configured severity; unknown or external-tool ids remain unavailable. CTest is the
current C2 run_tests provider; other test providers are not inferred or relabeled as CTest results.
Other providers and full PLAN C2/C3 parity remain outside these bounded adapters.

The committed offline fixtures cover auth refusal/argument policy, framing, initialization/control
correlation and timeouts, pagination, Stop-hook loops, private create-new/hard-link admission and a
real local stdio proxy/current-user IPC token exchange. They are authored, not executed. The initial
Astra core findings (native stdin pipe and Json string pointer use) were corrected. The complete
adapter/UI/repair snapshot still requires independent source recheck.

Actual unmodified CLI 2.1.316+ auth/initialize/control/hook schemas, merged settings behavior,
zero-token IDE/MCP handshake, native provider variants, Windows ACL/desktop behavior, WSL mapping,
local resource-race acceptance, cancellation/teardown, budgets and all real-tool journeys remain
unverified. No credential file, keychain, real provider process, prompt or workspace data was read
or transmitted during implementation. No milestone exit, legal approval, successful native
connection, test pass, provider charge or release is claimed.
