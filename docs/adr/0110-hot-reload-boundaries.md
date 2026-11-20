<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0110: Capability-bound hot reload and explicit restart boundaries

- Status: accepted decision; decision record only, no new reload implementation or runtime evidence
- Date: 2026-11-20
- Plan reference: R19 DBG-16, after DBG-14; PLAN.md HOTRELOAD-1 and later-tier managed debugging
- Related: ADRs 0073, 0087, 0090 and 0091

## Context and repository evidence

A debugger restart, a language-server configuration refresh and replacement of code in a running
process have different effects. Presenting all three as hot reload would promise preservation of
runtime state that the IDE cannot establish. DBG-16 requests a decision record, not an implementation
of Edit and Continue, a replacement compiler, or an engine patching mechanism.

The current `src/debug/include/frost/debug/dap_types.hpp` capability model contains
`supportsRestartRequest`; it contains no code-update/delta capability. `DebugSession::restart` in
`src/debug/src/debug_session.cpp` checks actual adapter capabilities, refuses attached/read-only
sessions and private engine sessions, clears frame/process references and sends the existing launch
arguments with `restart`. That operation provides no evidence of in-process code replacement or
state preservation. The native LLDB, GDB and DbgEng paths therefore cannot advertise hot reload on
the strength of that flag, memory writes, expression evaluation, or a successful rebuild.

ADR 0073 and `tools/managed-debug/dap-only.patch` explicitly exclude the pinned netcoredbg startup
hot-reload hook. The reviewed local payload is a debugger, not a .NET code-update provider. Changing
that recipe would reopen its dependency, provenance, compatibility and licence review; substituting
vsdbg is forbidden. This decision adds no dependency and accepts no licence.

FrostEngine communication already uses authenticated project/instance/generation bindings and
schema command admission (`src/engine/include/frost/engine/session.hpp` and
`src/engine/src/command_policy.cpp`). A command must be present, available, opted into IDE use and
admitted in the actual observed state. PLAN.md HOTRELOAD-1 calls for surfacing Lua reload outcomes,
states that the engine has no C++ hot reload, and specifies rebuild/restart as its replacement.
Neither that requirement nor a generic command channel proves a delivered per-class reload result
or state-migration protocol. No engine source outside the FrostIDE repositories was inspected for
this decision, and no engine capability was probed.

## Decision

There is no universal Hot Reload action. A future action is specific to the selected execution
profile and its actual reload provider. Its availability and result come from the selected runtime
or engine, separately from DAP restart capability. Unknown provider capabilities are unsupported.
An ordinary Save does not apply executable code, and an adapter ACK does not establish that an edit
was applied. Applying code is an explicit mutation with normal trust, cancellation and ownership.

| Profile | Decision | State boundary |
|---|---|---|
| Native C++ with LLDB, GDB or Frost DbgEng | No in-process update provider is established. Offer the existing eligible restart/rerun path with its actual name. A rebuild and fresh debug launch is the supported design for new code. | Runtime heap, stacks, threads and process identity are replaced on a fresh launch. Recreate breakpoints from source identities; old frame/variable references expire. |
| FrostEngine Lua | Feasible through the selected authenticated engine's actual Lua reload facility when its negotiated schema and outcome protocol support it. Surface the engine's actual per-class success/failure and navigable errors. This ADR does not claim that UI or protocol is implemented. | Preserve only state the engine explicitly reports it retained. Active closures, coroutine stacks, class instances and migrated fields cannot be assumed to survive an edit. |
| FrostEngine C++ | No hot reload. Rebuild, stop/restart the owned host and restore the recorded editor session through a supported restore contract. A paired external host requires an explicit external restart decision. | Runtime state is lost. Editor scene/document selection may be restored only from actual saved state and a confirmed restore result; unsaved documents need their normal save/recovery handling. |
| CoreCLR with the reviewed netcoredbg payload | No hot reload with this payload. Launch/attach and DAP inspection remain debugger operations. A future managed update provider needs a separate ADR and per-runtime/module edit capability. | Restart loses process state. Future deltas must bind the exact loaded assembly, runtime and symbol generation; unsupported edits require a visible restart choice. |
| Unity Mono or IL2CPP | Do not infer code-update support from attach, compilation success, a reload event, Mono runtime type or a Unity version label. Future integration follows the actual selected editor/player's compilation/domain-reload contract. | Domain/module replacement invalidates previous symbol/frame/value identities. Reattach/rebind only after fresh endpoint/module validation; no promise to preserve managed/static state or a player process. |
| Unreal Live Coding or other later providers | No generic support is established here. An engine-specific provider may expose its own trigger/result after its independent integration and licence review. | Preserve only guarantees made and observed by that exact provider; a rejected edit must remain rejected rather than being silently converted into restart. |

Debug, Release and Dist are distinct profiles. Debug information, compilation optimization, selected
runtime, assembly load mode and provider configuration may change eligibility. The IDE must not
guess that Debug always works, or that Release/Dist never can. Eligibility requires provider evidence
for the exact configuration and requested edit. No profile is newly enabled by this ADR.

## Required provider contract

A future implementation captures physical project, configuration/TFM where applicable, build/source
revision, loaded module identity, process identity, selected authenticated endpoint and generation.
A restart, reconnect, source edit, trust revision or module replacement invalidates that captured
operation. Apply is serialized with build/reload and debugger run control. Native all-stop prohibits
engine mutations until actual matching Continued evidence, following ADR 0090; a toolbar's Running
presentation or a Continue ACK is insufficient.

The provider distinguishes: unsupported edit with reason, preparation failure, accepted-but-pending,
applied with resulting code/symbol generation, failed without application, partially applied, and
uncertain outcome after transport loss/cancellation. Only an actual applied outcome can update the
matching displayed code version. Partial/uncertain results cannot be described as rollback or success
and are never automatically retried. An explicit restart may recover the host when the user chooses
it and ownership permits it. No automatic restart hides loss of running state.

Successful code replacement invalidates the affected stack/source/variable caches before they are
read again. Source locations come from the resulting symbols/sequence points, not from guessed line
shifts. Breakpoints are resubmitted to the provider and displayed as verified only after its answer.
The UI distinguishes the current editor buffer, loaded code and any pending edits; immutable debug
source snapshots continue to describe their original module until a newly validated generation is
available. Restoring window layout, watches and source breakpoints is IDE session restoration, not
runtime-state preservation.

Fresh launches retain existing process-tree ownership. Stop on a plain attach detaches; it never
kills an externally owned host merely to apply an edit. Engine restarts require fresh startup
records/authentication and cannot reuse ADR 0090 pause leases or ADR 0091 host/session credentials.
Reconnection does not replay a reload whose outcome was uncertain.

## Consequences and later acceptance

This closes the DBG-16 decision slice only. It implements no new command, action, protocol field,
compiler delta path or session-restore mechanism. No supported-platform or state-preservation claim
is expanded. Missing functionality in HOTRELOAD-1, managed updates, Unity reload survival or Unreal
integration remains its own implementation work; it is not relabelled as hardening.

Any provider implementation needs source regressions and real-tool acceptance for its exact supported
profiles: apply one permitted edit with an externally observable behavior change; reject a prohibited
edit with its real reason; prove the advertised state-preservation boundary; revalidate symbols and
breakpoints; refuse stale identity/trust; cover cancellation/disconnect and partial/uncertain outcomes;
and prove attached-host survival and owned-host restart cleanup. Mixed native/Lua acceptance also
covers native all-stop and fresh authenticated generations after restart. Missing providers fail the
required case instead of being counted as a pass.

This ADR was authored from the cited repository contracts only. No builds, configuration, tests,
feature executions, WSL, installations, network dependency inspection or licence acceptance occurred.
Independent source review is required before integration; runtime acceptance remains deferred under
the user's implementation-first instruction.
