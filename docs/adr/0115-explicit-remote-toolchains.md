# ADR 0115: Explicit remote toolchains and named Linux run targets

- Status: Accepted for implementation; runtime acceptance deferred
- Date: 2026-11-20
- Scope: PLAN R19 CMK16 and RUN15

## Decision

The Windows IDE may configure, build, clean and list targets through an explicitly selected WSL distribution, using named CMake/Ninja profiles. Named Linux Run targets may use explicitly selected WSL or SSH transports. This narrow R19 scope supersedes earlier remote-execution non-goals; remote workspaces, provisioning, remote Debug and remote language services remain outside this decision.

Endpoints are application-owned records, separate from project files, with explicit execution approval. Editing execution identity or mappings revokes approval. Project trust remains independently required. Executables, distribution or SSH identity, Linux interpreter, source mappings and remote tool paths are explicit; opening a project performs no discovery or remote execution. SSH uses strict host verification and batch mode, with forwarding and connection sharing disabled. Credentials and host provisioning remain external.

Mappings are nonoverlapping component boundaries with exact reverse checks. Dot traversal and Windows-incompatible remote suffixes refuse. Diagnostics navigate only through current validated mappings; relative remote paths do not acquire an invented working directory. Registry bytes, physical identities, project trust revision and cancellation are retained and rechecked during admission.

WSL profiles construct structured CMake/Ninja arguments from mapped sources and a separate Linux build directory. Actual selected tool version output must match the SDK pins before work starts. Profiles request compile_commands.json generation; they do not claim to project a remote FileAPI model into the native IDE model. A BuildService external-launch reservation and the heavy-build resource lease span probes and execution, and are released before native retry notification.

Remote Run is a distinct configuration type with Linux program and working-directory semantics and an exact endpoint binding. It has no native fallback, native before-launch tasks or remote Debug path. Unsupported native process policies refuse explicitly.

A static Linux Python guardian receives structured workload JSON over a dedicated control stream. It requires an actual kernel pidfd probe and child-subreaper support before launching the workload, keeps child input separate from control, emits framed actual child results and uses identity-bound pidfds for descendant cleanup. Stop, control EOF and heartbeat expiry request cleanup. Success requires the remote child result and cleanup acknowledgement as well as successful transport completion. Transport loss or timeout without acknowledgement reports unconfirmed cleanup; it is not proof that every remote process exited.

Output is bounded at protocol, process queues, retained console and GUI relay boundaries. The GUI relay coalesces output with one scheduled drain; overflow requests Stop and reports a visible failure. Profile persistence validates the complete bounded registry before replacing the previous image.

## Verification and consequences

Implementation and independent source review are the current evidence. Mapping and guardian fixes through 052cd2b9 received focused Astra source clearance. Later application integration, bounded-relay changes, actual WSL oracle source and this ADR received final independent Astra source clearance through 86ff4d17. No builds, tests, feature execution, WSL or SSH commands were run for this slice.

Authored deferred fixtures exercise actual guardian execution and independent descendant identity checks, explicit real transport execution, and actual WSL configure/build/run with generated compilation-database mapping. They require deliberately supplied endpoints and tool paths; unavailable endpoints skip and do not establish remote acceptance. Normal UI, shutdown, cancellation, connection-loss cleanup, tool-version behavior and cross-platform compilation remain runtime acceptance work. Source implementation is not a deployment or process-lifecycle acceptance claim.
