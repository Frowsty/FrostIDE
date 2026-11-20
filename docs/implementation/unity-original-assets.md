# Unity original assets and semantic source slice (R18 / LTR-13)

Status: implemented source checkpoint; execution and Unity acceptance are deferred. This is not
completion of all UNITY-03/04 acceptance. No build, configuration, test, Unity launch, or download
was performed for this checkpoint.

The implementation extends ADR 0066/0071. It reads original project source-index candidates and
captured dirty buffers, never generated copies. Original `.meta` GUIDs resolve navigation to source
assets. Canonical flow references, including array elements, expose exact UTF-8 byte locations in
scenes, prefabs, ScriptableObject `.asset` files, materials, controllers and animations. Binary or
unknown encodings, unreadable inputs and exhausted budgets explicitly mark the index incomplete.
The parser bounds individual lines to 16 KiB, files to 16 MiB, and aggregate captures to 64 MiB /
4096 snapshots. It is a conservative reader, not a general YAML serializer.

Normal Go to Declaration follows a YAML GUID into its unique original asset. Normal Find Usages
keeps executable C# references and augments them only after the owned Roslyn host identifies the
selected Unity symbol. Authoritative identities include project, assembly and Roslyn declaration
ID. Field evidence joins a document-local serialized object to its original `m_Script` GUID.
Persistent UnityEvent evidence joins a local target object ID to that GUID; the supported method
slice is an unambiguous public instance parameterless void method with persistent mode Void.
Overloads, cross-asset event targets, dynamic event signatures, managed-reference polymorphism,
prefab override/property-path rewriting and unsupported YAML forms are excluded. Names alone
never establish a C# call or field identity.

`integrations/roslyn-host/UnitySemanticFeature.cs` is an additive owned module requiring the R17
`ISemanticFeature` and `ISemanticContributions` seam. Genuine compilation symbols establish
MonoBehaviour/ScriptableObject inheritance and attributes. A bounded catalog provides event
completion, implicit use and generation proposals; normal intention generation passes through the
host's captured-source/compiler-admitted WorkspaceEdit boundary. Normal diagnostics report
supported callback-signature and serialization limitations and actual bound object lookup calls in
per-frame callbacks. Deferred lambdas/local functions are excluded from per-frame call evidence.
No Unity assembly or framework is substituted.

The normal managed Code Vision frontend consumes callback and serialized-field CodeLenses.
At most eight field lenses per request obtain original captured asset-value previews; other lenses
retain their semantic field label. Clicking uses normal Find Usages and its original-file previews.
Publication requires the same source version, project/generation, managed server, trust revision,
and all captured open-buffer versions. Values are captured observations, not a live Unity Editor
property view. External asset changes are reflected on the next refresh; there is no claimed
continuous Unity serialization observer.

Normal Explorer file rename/move prepares the original source and mandatory original `.meta`
bytes as one existing REF-1 transaction with source/target and sidecar absence dependencies.
REF-1 owns adoption, recovery, open-document rebinding and Undo/Redo. GUIDs are never regenerated
and serialized assets are never rewritten just because a source path changes. Direct `.meta`,
folder and unsupported cross-Assets moves are refused. Delete remains refused before effects:
REF-1 currently has no file-deletion participant, so no independent sidecar deletion is attempted.
Duplicate/paste into Unity Assets is refused pending a Unity-authoritative new-GUID operation.
These explicit limitations remain unfinished LTR-13 work rather than acceptance claims.

Regression sources cover original byte navigation, ambiguous identities, array/comment/quoted
scalar boundaries, CRLF, encoding and resource limits, native Windows/Posix Assets boundaries,
source/meta REF-1 receipt assembly, YAML object/member/event joins, dirty overlays and normal
managed CodeLens consumption. `tests/unity/UnitySemanticFeatureProbe.csproj` is a deferred portable
Roslyn source probe using the existing audited `FrostManagedAssets` payload, with no package
references/downloads. It checks genuine inheritance, declared member IDs, callback signature
validation, bound hotspots, proposal-only generation and normal semantic contributions.

Public Unity contract reference: [MonoBehaviour messages](https://docs.unity.com/en-us/engine/6000.6/script-reference/unityengine/monobehaviour).
