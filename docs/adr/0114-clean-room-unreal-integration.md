# ADR 0114: Clean-room Unreal integration

Status: source implementation in progress; real-tool acceptance deferred.

## Decision and provenance

D41 permits independent FrostIDE-side Unreal support using primary public Epic documentation and metadata. The user approved original IDE support for publication and decided that the optional Unreal Editor plug-in remains local pending license review. The latter belongs only on `codex/r19-unreal-plugin-local`, never in IDE main, staging inputs or payload manifests. No engine source, installed proprietary sources, downloaded examples, SDK, binaries or copied plug-in code were inspected or incorporated. This ADR is a scope decision, not a legal conclusion or license acceptance.

Public sources used for the independent implementation:

- [FProjectDescriptor public metadata](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Projects/FProjectDescriptor): descriptor fields, engine association, module and plug-in references.
- [Managing Game Code](https://dev.epicgames.com/documentation/en-us/unreal-engine/managing-game-code-in-unreal-engine): explicit engine-association metadata.
- [Unreal Build Tool](https://dev.epicgames.com/documentation/unreal-engine/unreal-build-tool-in-unreal-engine), [Modules](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-modules), and [Unreal Header Tool](https://dev.epicgames.com/documentation/unreal-engine/unreal-header-tool-for-unreal-engine): selected build targets and UBT-owned header generation; explicit UHT manifest mode.
- [Epic developer support discussion of GenerateClangDatabase](https://forums.unrealengine.com/t/obtaining-paths-for-includes-and-directives-from-the-ubt/2718496): compile-database command mode, subject to the selected installed version's real oracle.
- [Running Unreal Engine](https://dev.epicgames.com/documentation/en-us/unreal-engine/running-unreal-engine) and [Command-Line Arguments](https://dev.epicgames.com/documentation/unreal-engine/command-line-arguments-in-unreal-engine): project argument for an explicitly selected installed Editor.
- [Reflection System](https://dev.epicgames.com/documentation/en-us/unreal-engine/reflection-system-in-unreal-engine) and [Plugins](https://dev.epicgames.com/documentation/en-us/unreal-engine/plugins-in-unreal-engine): public reflection vocabulary and JSON plug-in metadata. API names and small independently authored fixtures are used; engine implementation text is not reproduced.

## Source boundaries

The Qt-free project reader accepts bounded descriptor JSON, rejects duplicate keys, wrong known-field types and ambiguous identities, and preserves the user's file by never rewriting it. Unknown metadata is not interpreted. Build.cs and Target.cs are executable rules and are never evaluated by metadata reading. Engine association is matched only against explicitly supplied installed metadata; missing or duplicate matches require selection.

The build request identifies an absolute UBT executable or an explicitly selected dotnet host plus absolute UBT DLL, project, target, configuration and platform. It produces literal argv for Build, Clean, project-file generation, GenerateClangDatabase or UHT with an explicit manifest. Shell/batch launch wrappers and implicit PATH discovery are refused. Opening a descriptor does not invoke these tools. Only explicit user actions may start the trust-gated BuildSession; its real process outcome and diagnostics remain authoritative. Callers retain save-all, project-generation and trust-revision admission before applying results.

Unreal Editor run and debug adapters use the existing native configuration and native debugger paths, with a project trust root. No FrostEngine credentials or private launch semantics are reused. Selection of the installed Unreal visualizer must use the existing trust-scoped native visualizer admission; no Epic natvis ships with FrostIDE. Remote translation remains a later explicit toolchain boundary, with no automatic transport or path rewriting here.

Reflection navigation filters actual Clang preprocessor callback events for the public macros, including UCLASS. The caller must validate the captured dependency receipt and current configuration before publication. No regex reconstruction, inferred reflected declarations, fabricated Blueprint usages or counts are produced. Blueprint relationships and Editor controls require a real authenticated Editor producer; the local-only plug-in remains separate.

Staging already uses explicit package paths (`integrations/unity/com.frostide.editor` and separately enumerated managed-debug inputs), not a recursive integrations discovery rule. No packaging change is needed for the local-only Unreal plug-in. A publication tree/range audit must verify that the local-only branch's paths are absent from main and its staging inputs.

## Deferred evidence and remaining integration

The original `tests/build/fixtures/unreal` project contains independently authored project/module/target descriptors and one UObject with a reflection macro and a breakpointable method. The fixture has not been built. Unit regression sources cover descriptor validation, exact argv, trust refusal with zero launches, process cancellation, native adapters and macro-event navigation. They have not been run.

The `FROST_UNREAL_ORACLE` CMake option defaults off. Explicitly enabling it requires selected installed tool, project, target, generated-header and compile-database paths; an optional UBT assembly identifies dotnet-hosted UBT. Its source invokes real UBT, requires exit zero, a real generated header and an actual nonempty parsable compilation database. Execute only after the user's later runtime authorization and license prerequisites; copy the authored fixture into a writable local oracle workspace first. Neither configuring this source nor this ADR records an oracle pass.

The app now recognizes explicit .uproject entries, reads descriptors on a worker with bounded retained handles, and installs a project-scoped Unreal build backend. The Unreal menu explicitly selects tools/target/platform/configuration, generates project files/databases or invokes UHT, creates ordinary native Editor Run/Debug configurations, and admits a user-selected installed .natvis through the existing visualizer path. Save cancellation, late save completion, trust revocation during save and actual child failure have authored backend regressions. Accurate module/target tree publication, validated generated database admission, actual breakpoint-stop verification, full reflection/specifier completion and Blueprint link presentation remain integration work. LTR-15/LTR-16 and R19 are not complete from these sources alone. No build, test, feature probe, Editor/plugin launch, engine scan, install, license acceptance, network engine access or push was performed.


## Profile and project-model source continuation

The Unreal menu now offers a native tree for project modules, plug-in references, project-local plug-in descriptors and target rule filenames. A bounded worker inventories only the project's Source/Plugins directories; directory links are skipped, limits fail explicitly, and rule contents are never evaluated. Local descriptor discovery does not imply plug-in enablement. Installed engine directories remain outside this inventory. Tree activation opens the real descriptor/rule file. A separate action opens an explicitly selected existing UHT generated header; normal include navigation uses clangd and the admitted actual flags.

A configured UBT profile can explicitly select its generated compile_commands.json. Changing tools/target/platform/configuration clears that selection. A worker reads the actual file through a bounded retained handle, validates entries, rebases directory/file fields against the producer directory without changing compiler argv, and publishes a content-addressed snapshot under the IDE cache. The provider binds the selected profile and captured trust revision. Unreal clangd startup requires the database: absence or admission failure refuses launch, while existing non-Unreal callers retain their prior fallback behavior. Response files and generated includes remain real producer dependencies; this selection is a user assertion of the chosen UBT profile, not a fabricated proof that another tool invocation produced that exact configuration.

Authored regression sources exercise local inventory without rule evaluation, stopping discovery below plug-ins, exact argument preservation, immutable old/new snapshots, trust-revision rejection, profile invalidation and zero-launch strict database failure. They have not run. Automatic UBT target evaluation, automatic generated-header production, the native breakpoint oracle and full Editor-partner semantics remain unverified or incomplete.
