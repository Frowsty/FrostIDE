# ADR 0117: Governed local language server registry

Status: implementation authored; independent Astra source review clear; execution deferred.

LTR-17 adds generic stdio LSP onboarding through **Configure Language Servers**. The application
setting `languages.serverRegistry` is application-only and marked executing: checked-in or
personal project configuration cannot introduce executable servers. The product imports/edits
a version 1 JSON manifest for independently installed local tools. It does not download, install,
recommend or bundle a new dependency. Existing C++/Lua/C#/JSON/CMake/MSBuild services retain their
specialized routing and cannot be replaced with registry selectors.

Each descriptor declares a stable id/name, exact languageId, declared version, lowercase extension
selectors, absolute executable, literal argument vector, enabled flag, SHA256 artifact list,
exact approved SPDX expression and pinned local license evidence. `configuration` maps exact LSP
section names to JSON; `initializationOptions` passes its own initialize payload. Limits are
32 descriptors, 32 selectors, 128 arguments, 64 artifacts, 1 MiB manifest/license evidence,
128 MiB per artifact and 256 MiB total named artifacts. Example structure (paths/pins must be
replaced with actual installed files; this is configuration, never observed tool output):

```json
{"version":1,"servers":[{"id":"local-server","name":"Local language server",
"languageId":"example","version":"reviewed-installed-version","extensions":[".example"],
"executable":"/absolute/local/server","arguments":["--stdio"],"enabled":false,
"license":"MIT","licensePath":"/absolute/local/LICENSE","licenseSha256":"actual-64-lowercase-hex-digits",
"artifacts":[{"path":"/absolute/local/server","sha256":"actual-64-lowercase-hex-digits"}],
"configuration":{},"initializationOptions":{}}]}
```

The simple SPDX allowlist matches the existing repository license gate. Compound expressions or
exceptions outside that exact list refuse; local onboarding does not create a new ADR exception
or weaken `tools/licensecheck.py`. The declared version/SPDX and pinned license text are review
evidence supplied by the user, not an inferred package identity or automated legal conclusion.
Only named artifacts are pinned; this is not a hermetic runtime/dependency closure or an OS sandbox.
A repository-bundled server/dependency still needs its own existing license/ADR governance.

The registry reuses the existing supervised LSP transport and document synchronizer through
additive generic configuration/initialize options. A server starts only for an actual matching
open document in the trusted physical project. A dedicated launcher rechecks the captured trust
revision, physical project root, exact executable/argv and named artifact/license hashes at every
initial launch and supervised restart, then rechecks trust/root after hashing. It uses the existing
absolute-path `IProcessLauncher` process containment and shutdown. Hashing and launch run on workers;
no shell command interpolation, PATH discovery, install hook or network fetch is introduced.

Registered request routes are immutable snapshots of exact admitted open-document identities.
Delayed requests retain their original server and admission; they recheck project/document trust
and physical containment before sending and after the actual reply. External documents cannot
borrow a registered server through extension matching. Incoming server applyEdit checks the same
boundary on the GUI document owner immediately before the ordinary synchronous edit handler.
Physical project replacement retires the epoch and requires closing/reopening the project or an observed withdrawal and renewal of trust; an unrelated global trust revision cannot revive that epoch. Trust,
project or manifest changes cancel publication from old epochs and stop owned process trees.

The editor and Problems use actual versioned server diagnostics; stale/absent/failed observations
withdraw editor diagnostics and insight bindings. Capabilities and observed server name/version
come only from the initialize response. The status snapshot keeps declared version separate from
observed serverVersion. Missing executables, changed pins, disallowed licenses and untrusted or
replaced projects produce explicit unavailable/refused states, never sample diagnostics.

Authored tests cover malformed manifests, selector collisions/builtin protection, license/pin
refusal, trust revoke/regrant, external/delayed request refusal, supervised generic initialize and
didOpen wire behavior, retained routing across reload/tab changes, and product application-scope
onboarding. These are labelled scripted/native source fixtures, not evidence that any particular
new language server has passed real-tool/platform acceptance. No builds, tests, feature execution,
downloads or verification runners were executed under the implementation-first instruction.
