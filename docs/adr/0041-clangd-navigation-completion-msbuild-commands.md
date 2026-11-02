<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0041: Navigation, completion and MSBuild-evaluated compile commands (W2)

- Status: accepted for `w2/clangd-nav` (feature-first phase, ADR 0031). No budget or warning is weakened. Four
  keymap goldens gain one line each for the new `navigate.relatedFiles` binding.
- Date: 2026-11-02
- Plan reference: PLAN.md section 2 ("Semantic engine"), 3.2 (`src/lang`, `src/ui`), 3.4 ("Completion",
  "Navigation"); ADR 0032 decision 3 (Rider-like navigation and completion on frost::lang), ADR 0036 (the LSP
  client, ClangdService and the compile-command sources), ADR 0034 (MSBuild discovery), ADR 0016 (actions and the
  IntelliJ keymap).

## Context
ADR 0036 gave clangd a static compile database for Visual Studio solutions. That database is wrong for
SDK-generated solutions: their include directories and defines come from imported `.props` sheets and `$(VAR)`
environment paths that only MSBuild's evaluation resolves, so clangd reports false missing-include errors. Work use
also needs Rider's everyday code features (completion, navigation, find usages, rename, quick fixes, reformat) on
top of ClangdService. The editor view is not on this branch.

## Decision
1. **Provider order.** `solutionCompileCommandsChain` asks, in order: an existing `compile_commands.json` under the
   solution folder, then the MSBuild-evaluated provider, then the static synthesizer of ADR 0036. A provider's
   error is used only when no later provider has a database.
2. **MSBuild evaluation (Windows, trusted solutions only).**
   - Per project and configuration, MSBuild runs `-getItem:ClCompile -getProperty:IncludePath;
     ExternalIncludePath;MSBuildAllProjects;...` with `-p:Configuration`, `-p:Platform`, `-p:DesignTimeBuild=true`
     and the `Solution*` properties. No target runs, `-nodeReuse:false` is set, and the absolute MSBuild path comes
     from discovery (frost::build, injected because `lang` cannot depend on `build`). The run goes through
     IProcessLauncher, off the UI thread, with a time limit (60 s).
   - The evaluated item metadata becomes clang-cl flags: AdditionalIncludeDirectories, PreprocessorDefinitions,
     UndefinePreprocessorDefinitions, LanguageStandard/_C, AdditionalOptions (without /Y*, /Fp, /Fo, /Fd, /MP),
     ForcedIncludeFiles, RuntimeLibrary, ExceptionHandling, ConformanceMode, TreatWChar_tAsBuiltInType and
     CompileAs. A `Use` precompiled header becomes `/FI` of its header. The evaluated IncludePath and
     ExternalIncludePath become `/imsvc` system includes.
   - **Cache key.** MSBuild 16+ no longer lists imports in MSBuildAllProjects, so a second `-pp` run writes the
     preprocessed project, and its banner comments give the import list. Each import's size and modification time
     are compared before cached commands are reused. MSBuild writes `--` as `__` in those comments; such paths are
     repaired when the written form does not exist.
   - **Fallback.** A project whose evaluation fails gets the static synthesizer's commands, with a warning. If no
     project was evaluated, the provider returns the error, and the chain's static provider answers.
   - **Trust.** Evaluation runs project code (property functions, imported props), so an untrusted solution
     (`trusted()` false) is never evaluated and keeps the static fallback.
3. **Feature models in frost::lang (Qt-free).** Each feature is a coroutine over the new `ILspRequester` seam,
   which ClangdService and LspClient both implement:
   - completion, with resolve, text edits, additional edits and snippets turned into plain text plus tab stops. A
     client-side fuzzy matcher filters and sorts Rider-like (exact, prefix, case-insensitive prefix, word
     starts, fuzzy) without asking the server again unless the list was incomplete;
   - signature help, hover (Markdown), definition/declaration/implementation, Rider's "declaration or usages",
     switchSourceHeader, references grouped by file with line previews, document highlights, the document symbol
     tree with speed search, workspace symbols, prepareRename and rename, code actions with clangd fix-its, and
     formatting/rangeFormatting.
   - WorkspaceEdits are applied through `IWorkspaceEditApplier`. `WorkspaceEditApplier` checks every file first,
     then edits open documents as one undo step each and rewrites closed files atomically. It refuses the whole
     edit (Busy, with a message saying what to do) when a version is stale or when an open document has unsaved
     changes and also changed on disk.
4. **UI (`frost::ui_lang`, layer ui).**
   - Standalone widgets: the completion lookup with a documentation panel, parameter info, quick doc, the
     find-usages panel, the file-structure / go-to-symbol popup, the rename field and a choice list (quick fixes,
     several targets).
   - LanguageController drives them through `IEditorLanguageHost`, the seam the workbench implements over its
     editor. It posts results back to the UI thread through LatestWins slots.
   - It registers these actions under the IntelliJ ids: code.completion, code.parameterInfo,
     code.quickDocumentation, navigate.declaration, navigate.implementation, navigate.relatedFiles (new,
     Ctrl+Alt+Home; macOS Ctrl+Meta+Up), navigate.findUsages, navigate.highlightUsages, navigate.fileStructure,
     navigate.gotoSymbol, refactor.rename, code.showIntentions and code.reformat.
   - Kind icons are letter badges in theme-neutral hues until the atlas has symbol icons.

## Measurements
On this machine (VS Build Tools 2022, MSVC 14.44, Debug build), lang_msbuild_tests runs two property-sheet
evaluations, one cached locate and a clangd check in about 2.6 s. A cached locate runs no MSBuild.

## Consequences
- The workbench must implement IEditorLanguageHost over EditorView, IWorkspaceDocuments over its open documents,
  pass `locateMsBuild` (frost::build::findMsBuild) and the live trust to the chain, and dock the usages panel
  (TODO(M2)).
- Deferred to the hardening phase or later milestones:
  - evaluation of several projects in parallel;
  - environment variables in the cache key (today only the files are);
  - Directory.Build.props files created after the first evaluation;
  - clangd tweaks that need workspace/executeCommand and applyEdit;
  - atlas icons for symbol kinds;
  - matched-character highlighting in the lookup;
  - fuzzing of the snippet, evaluation-JSON and `-pp` parsers.

## Alternatives considered
- **Evaluating through `-preprocess` alone and parsing MSBuild XML ourselves.** Rejected: it re-implements
  MSBuild's evaluation; `-getItem` gives evaluated metadata directly.
- **A design-time build (`/t:GetClCommandLines`).** Rejected for now: it runs targets (slower, and it runs more
  project code); the evaluation query is enough for clangd.
- **Server-side filtering on every keystroke.** Rejected: one request per prefix adds latency; clangd's lists are
  complete for most prefixes, and `isIncomplete` still re-requests.
