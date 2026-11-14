# ADR 0099: Engine assets, actual usages and transaction receipts

Status: source implementation in progress; native/engine acceptance unexecuted.

## Asset observations

FEC-4 reuses project.info/assets.list rather than indexing file extensions or scanning folders.
An immutable AssetIndex owns the validated ProjectContent and maps exact unsigned handles and
native path identity keys to engine-reported assets. Refresh replaces the snapshot as a whole.
FEC-5 queries the selected authenticated session's actual assets.find-references command, using
its existing command policy and physical project/trust/session admission. Reply targets and source
identities must agree with the retained index. Counts distinguish referencing documents from total
occurrences. Code Vision uses the document count matching the CLI top-level count. Unsaved FrostEditor scenes and project/import settings remain explicit. Unregistered unsaved
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
