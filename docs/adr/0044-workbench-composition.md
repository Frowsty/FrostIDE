<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0044: The W1 workbench composition

- Status: accepted (feature-first phase, ADR 0031)
- Date: 2026-11-02
- Plan reference: PLAN.md section 3.4 (UI); ADR 0032 (W1), ADR 0035 (build service), ADR 0036 (clangd),
  ADR 0037 (editor view), ADR 0038 (shell frame), ADR 0039 (build tool windows)

## Context
W1's pieces existed separately: the shell frame, the editor view, the save manager, the build service and its
tool windows, clangd and the settings store. W1 needs them wired into one window that opens a Visual Studio
solution, edits, saves, builds, lists errors and jumps to them, with clangd's live errors.

## Decision
1. **One composition root, `frost::app::Workbench` (`src/app`).** It owns the registry, the keymap, the
   settings adapters (`StoreShellSettings`, `StoreKeymapSettings`, `StoreAppearanceSettings` over
   `frost::settings`), the save manager with its hot-exit journal and local history, the editor factory
   (`DocumentEditorFactory`), one `BuildController` with `BuildServiceBackend`, and clangd. `main()` builds the
   workbench, then the `MainWindow` from `workbench.environment()`, then calls `attach()`. The window is
   destroyed before the workbench; `shutdown()` runs first. Dialogs go through `IWorkbenchPrompts`, scripted in
   tests.
2. **One open-file path.** The command line, File > Open, Open Recent, the Welcome screen, the Solution
   Explorer, the Problems window and the Build output all end in `OpenFileService::open(path, line:column)`.
   - It canonicalises the path: normalised, and on Windows the file's real case from
     `GetFinalPathNameByHandleW`.
   - It opens or activates the tab, then goes to the location.
   - The save manager keeps one `FileDocument` per document key. Load refusals reach the user as
     notifications.
   - `Workbench::openPath` routes first: a `.sln` or `.slnx` opens as the project; a folder opens its single
     solution, else becomes a folder project. Any other path goes to the open-file path.
3. **Saving follows Rider.**
   - `Ctrl+S` is IntelliJ's `file.saveAll`. `file.saveDocument` saves the current document and has no default.
   - Closing a dirty tab saves it first. The user is asked (Discard or Cancel) only when that save fails.
   - Autosave runs on frame deactivation and on idle (`save.autosave.*`). The hot-exit journal is written on
     idle and at exit.
   - At start-up, after the first frame, "Restore N unsaved files?" calls `SaveManager::restore`.
   - Every build saves all first, through the build tool windows' Save-All hook (`build.saveAllBeforeBuild`).
4. **External changes** are checked when the frame is activated and by Reload All from Disk. A clean buffer
   reloads silently. A dirty one gets a sticky notification offering "Keep my version" and "Take disk version".
5. **Auto-show rules.**
   - The bottom "build" tool window shows when a build starts.
   - Problems shows after a build that reported errors.
   - The Solution Explorer shows when a project opens.
   - None of them takes focus.
6. **Diagnostics in the editor.**
   - Build diagnostics become gutter markers. They are refreshed on every build event and when a document
     opens.
   - clangd's diagnostics become Problems' Live rows, wavy range decorations (the new
     `EditorView::setRangeDecorations`, wavy or straight, with a tooltip) and gutter markers.
   - The editor's `textChanged(ChangeSet)` feeds didChange. Open, save and close feed didOpen, didSave and
     didClose.
   - A reload resends the whole text.
7. **Staging.** `tools/stage.ps1` builds Release or RelWithDebInfo and stages:
   - frost.exe and frost_app.dll;
   - the crash handler and the resources;
   - the Qt runtime from the pinned Qt's `windeployqt`, plus the build's offscreen plugin for `--selftest`;
   - LICENSE, NOTICE and the vendored licences.

   It checks the folder for frost.exe and the platform plugin.

## Consequences
- The Welcome screen is the editor area's empty state while no project is open. A project's empty state is the
  shell's label.
- Recent projects are the `general.recentProjects` application setting, newest first, at most 20.
- The save manager's disk work still runs on the GUI thread (TODO(M1) in `workbench.hpp`).
- Range decorations keep their byte ranges until clangd publishes again. They are not mapped through edits.
- frost_app.dll now links the editor, the build service and clangd. Its import allowlist
  (FrostProcess.AppDllImportsOnlyAllowedDlls) is unchanged, and two seams keep it:
  - frost::build and frost::lang find Program Files (x86) with `SHGetFolderPathW`. `SHGetKnownFolderPath`
    would need `CoTaskMemFree`, which is in ole32.dll.
  - `src/app/src/absl_symbolize_windows.cpp` replaces abseil's symbolizer, which re2's fatal-error stack dump
    uses, with one that names nothing, as abseil's own unimplemented symbolizer does. Without it frost_app.dll
    would import dbghelp.dll. FrostIDE symbolizes offline (ADR 0022).
