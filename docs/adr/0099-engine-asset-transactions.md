# ADR 0099: Engine assets, actual usages and transaction receipts

Status: source implementation in progress; native/engine acceptance unexecuted.

## Asset observations

FEC-4 reuses project.info/assets.list rather than indexing file extensions or scanning folders.
An immutable AssetIndex owns the validated ProjectContent and maps exact unsigned handles and
native path identity keys to engine-reported assets. Refresh replaces the snapshot as a whole.
FEC-5 queries the selected authenticated session's actual assets.find-references command, using
its existing command policy and physical project/trust/session admission. Reply targets and source
identities must agree with the retained index. Counts distinguish reference rows (including separate settings in one document) from total
occurrences. Code Vision uses the reference-row count matching the CLI top-level count. Unsaved FrostEditor scenes and project/import settings remain explicit. Unregistered unsaved
scenes have no invented registry handle or disk-content navigation; the UI identifies their
live-editor-only state. Project settings open through the text-document navigation workbench,
with physical project containment checked, so the application cannot switch projects. Unreadable
documents make the query incomplete: no complete Code Vision count is published, including zero.

The Engine View opens referencing documents without inventing occurrence source lines. Engine
Code Vision uses a separate provider key alongside language metrics; it represents the actual last
engine query at asset-document scope, not a C++/Lua declaration estimate. Project/session/refresh
and IDE document changes invalidate those observations. Independent per-asset queries coexist;
one newly opened asset does not cancel another asset's lens. The current engine contract advertises
no asset/scene-content change push (only play.state/log/diagnostic/project.changed/server.shutdown).
Observed play-state changes and successful non-query engine commands invalidate locally retained
counts. External engine content edits therefore require explicit refresh; lenses are labelled
non-live query snapshots, not continuously current values. Stale asynchronous replies cannot publish.
The engine is the oracle; the authored future corpus case compares listing entries and a genuine
new reference against actual CLI listing/find-references. No runtime results are claimed.

## Remaining transaction boundary

Existing engine assets.rename moves .fmeta and keeps handles, with engine-local Undo. It is not
an IDE multi-file REF1 transaction and is not used as a substitute. Reference rewrite preparation
must be produced by the real engine with exact before/after images and sidecar/identity receipts;
unsaved engine documents require explicit admission and cannot be overwritten by disk guesses.
REF1 native participants, durability/recovery and global Undo remain the application write path.
A future planning/lease command must retain the authenticated source boundary and report partial
or uncertain outcomes truthfully; no externally atomic filesystem or universal same-user lock is
implied. This ADR will be extended with the concrete serialized-property and receipt profile as
those dependent source slices land.

Primary contract sources are the designated safe engine checkout: Automation/Commands/AssetCommands.cpp
(assets.list/info), AssetFileCommands.cpp (find-references and asset file operations), AssetOperations.cpp
(actual reference semantics including unsaved scenes/settings), ComponentCommands.cpp (reflected JSON
and validated component.set), and Core/Reflection metadata. No private backend or live developer
workspace is queried or executed during implementation.

## Serialized properties

The FEC-6 inspector chooses an actual engine-listed scene, an explicit entity name/ID, and a
component name from the authenticated runtime schema. component.get supplies the serialized
fields and exact entity identity; subsequent edits use that ID rather than resolving a name again.
Only changed fields are sent, leaving unchanged/read-only values out of the patch. The engine
still performs all reflection/range/read-only/invariant checks. No frontend serializer predicts
success or writes the scene file. A successful component.set is followed by a real component.get;
the editor shows the returned normalized values and reports concurrent changes honestly.

The additive optional component.set expected parameter contains the complete actual Before
fields. The engine compares it on the same main-thread command immediately before ApplyFieldsStrict;
a mismatch fails before mutations/Undo entries. Older engines without this described parameter
remain readable but read-only. Dialog requests retain exact session/project/trust/index lifetime,
cancel only their own request IDs, and cannot move a stale inspector to a replacement session.
The live scene's save and Undo remain engine-owned; this single-component inspector does not
claim an IDE REF1 disk transaction or global Undo. Native engine tests and real paired-engine UI
roundtrip/stale-Before tests are authored only. Actual command producer policies explicitly admit
read queries and descriptor reads, and restrict component writes to Edit plus mutation permission;
no wildcard or IDE-side policy bypass is introduced.

## Asset move proposal and reversible receipt profile

FEC-9b consumes actual session `assets.plan-move {path,target}` replies. Version 1 returns a
session lease, a retained reversible receipt, exact unsigned handle, old/new virtual paths,
asset/metadata move rows, exact before/after UTF8 reference document bytes, and bounded raw-byte
FNV1a64/size guards. The IDE validates and physically contains every path before converting it
to native REF1 authority. Missing metadata, malformed/overlapping participants, unsupported
rename-plus-rewrite source documents and uncertain identities refuse. Dirty IDE documents and
open reference-rewrite documents are explicitly refused rather than projected into disk guesses.

The normal file Rename/Move preparation composes these real engine edits with the asset/.fmeta
moves in one preview and native transaction. `assets.validate-move-plan` supplies the exact
verified lease/receipt/handle/source decision receipt at the final admission boundary.
After native settlement, `assets.finish-move-plan {lease,applied:true}` must verify the resulting
images, refresh the actual registry, and return verified/refreshed/handle/destination evidence.
The IDE also requests its real content refresh and invalidates cached usage observations.
Failure after native settlement is reported as an engine reconciliation warning, never as a
rolled-back disk operation; the retained lease is retried before history can proceed.

Undo/Redo reacquire `assets.reopen-move-plan {receipt,reverse}` from the same authenticated session.
The engine retains original raw pre/post images; the consumer accepts only the exact frozen
inverse. No client-supplied restorable JSON is trusted. Native history admission and the engine
lease are both rechecked before a decision. Pending native recovery retains its active lease;
Resume performs the frozen native decision then actual engine settlement. Remote receipts are
session-local and are not fabricated on application restart or replacement-session pairing.
Lost replies/session loss can therefore leave explicit reconciliation refusals, with no claimed
cross-process atomicity or universal same-user exclusion.

The producer admits persistent editor/headless sessions only when all scenes and worlds are closed.
A native project mutation gate holds the `.frost/asset-mutation.lock` process lock across the
lease, and rejects engine file mutations, registry rescans and asset watcher writes from other
owners. The producer records the owning RPC connection: disconnect refreshes/releases only when
all exact before images or all exact after images are observed; a mixed native journal state
retains the active lease/lock and reports recovery instead of admitting engine writes. One-shot CLI contexts cannot own a lease. The IDE exposes the actual refusal and never
falls back to engine-local Rename or an unguarded disk move. Directory asset moves (including
the Assets directory itself) are refused until native directory transactions exist. These are
implementation boundaries; the ordinary paired GUI acceptance journey has not run. Parser and
native external-guard history regression sources are authored; paired-engine/platform executions
remain deferred by the user override.

The explicit **Recover Refactoring Journal and Engine Lease** action is available after opening
and trusting a project, even when no in-memory refactoring coordinator exists. It initializes
the owned native recovery store and replays/resumes only existing durable decisions. Native
PendingRecovery blocks engine reconciliation. After complete native settlement it requests the
actual `assets.recover-move-plan {}` observation: `{recovered:false}` means the engine has no
stranded disconnected lease; `recovered:true` requires the complete verified/refreshed retained
receipt with exact unsigned handle/path. A mixed state remains an explicit error with its lock
held. Ordinary engine Rename/Move preflights this same owned-journal boundary before obtaining a
new engine lease. No engine expected bytes or new filesystem write plan are supplied by recovery.
Authored regressions cover native same-process Resume and a real native interrupted journal
restart coupled to a labelled authenticated socket peer whose recovery reply follows complete
native file/metadata images. These sources have not executed.
