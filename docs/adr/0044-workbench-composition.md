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

## Unified with shell-nav (w1/unify, 2026-11-02)
W1 and shell-nav (ADR 0042) each built a composition root, a document store and an open-file path. One of each
remains. Where they differed, Rider's behaviour decided.
1. **The composition root** is `frost::app::Workbench`. It now also owns what shell-nav's `AppServices` did:
   - the thread pool, the I/O reactor and the real file watcher (`watchFiles`);
   - the session store (`sessionDirectory`) and the autosave controller;
   - the `nav::DocumentRegistry`, and `ui::DocumentEditorFactory`'s banner views around the app's editor views.

   `attach()` creates shell-nav's `ui::Workbench` (the navigation workbench) on the window. `AppServices`,
   `OpenFileService`, `app::canonicalPath` and their tests were deleted; the navigation workbench's and this
   ADR's tests cover the same behaviour. `MainWindow()` without arguments owns an in-memory workbench.
2. **One open-file path.**
   - Every caller ends in `Workbench::openPath`: the command line, menus, Welcome, the Solution Explorer,
     Problems, the Build output and the language features.
   - Shell-nav's own entry points join it: Go to File, Search Everywhere, the File System view, recent files,
     back and forward, and session restore.
   - `openPath` routes projects. Files go to `ui::Workbench::openFile`, keyed by `DocumentRegistry::canonicalize`
     over `IFileSystem::canonicalPath`.
   - `canonicalPath` keeps the file's real case on Windows: `GetFinalPathNameByHandleW` when it only changes
     case, else each component's name from `FindFirstFileExW`, so links, junctions and subst drives keep the
     user's spelling. On Linux, `realpath` resolves `..`.
3. **Saving, reloading and closing** are the navigation workbench's registrations:
   - `file.saveAll` (Ctrl+S) and `file.saveDocument`;
   - `file.reloadFromDisk`, now "Reload All from Disk";
   - `file.closeProject`, which routes to `Workbench::closeProject` through a hook.

   The app registers only `file.openFolder` (now in the File menu) and `file.openRecent`. `file.openProject`
   was removed: Open Folder and Open cover it.
   - Save-on-close stays the app's listener: save, else Discard or Cancel. A cancelled tab keeps the project
     open (closeProject returns false).
   - Autosave, the hot-exit journal and the one restore prompt are the navigation workbench's: Restore or
     Discard in a sticky notification, once per start, after the first frame.
   - The editor views follow the registry's change notifications: a reload, a save or a resolved merge.
4. **External changes.**
   - A clean buffer reloads silently.
   - A dirty buffer gets a sticky notification: "Keep my version", "Take disk version" and "Compare...". Compare
     opens shell-nav's merge view.
   - Changes are checked on frame activation, by the file watcher, and by Reload All from Disk.
5. **Projects and sessions.**
   - Recent projects (Welcome, File > Open Recent) and recent files (Ctrl+E) stay separate lists.
   - At start-up without a project argument, `restoreLastProject()` reopens the session's last root, mapped back
     to the recent-projects entry the user opened (the solution file), with its tabs.
   - Close Project clears the last project (`SessionStore::clearLastProject`), so the next start shows Welcome.
   - A folder project shows the File System view, not an empty Solution Explorer.
6. **Run** (ADR 0040).
   - The shell's "run" tool window (Alt+4) holds the `RunConsole`. `RunController` drives the toolbar's
     startup-project combo, and the Solution Explorer gained "Set as Startup Project".
   - `run.*` are run-service's registrations with the keymap's defaults.
   - Runs save all and build through the build service, which keeps the trust refusal. The window shows when a
     run starts.
7. **Language features** (ADR 0041).
   - There is one `ui::lang::LanguageController` per window over `LanguageBridge`. The bridge is the
     controller's `ILspRequester`: it proxies the running clangd and answers Busy without one.
   - The bridge is also the `IEditorLanguageHost` over the active `EditorView`, and the `IWorkspaceDocuments`
     over the open editors. Edits apply as one undo step, so they reach clangd through the editor's change
     path.
   - The workbench's application event filter gives the controller its keys before the shortcut dispatcher.
     Ctrl+click is a new `EditorView::ctrlClicked`. `EditorView::offsetAt` and `globalCaretRect` place the
     popups. `ClangdService::documentVersion` gives the versions.
   - Find Usages has a bottom tool window ("findUsages").
   - The compile commands come from `solutionCompileCommandsChain`: an existing database, then MSBuild's
     evaluation through `findMsBuild` when the root is trusted, then synthesis. A configuration switch
     restarts clangd on the new configuration's database.
8. **Range decorations** are kept per owner ("diagnostics", "usages") in the workbench and merged into the
   editor's one list. editor-insight's `setDecorations(owner, ...)` can absorb them.
9. **The window's teardown** runs hooks first (`ShellWindow::addTeardownHook`). The workbench detaches the
   navigation workbench there, which saves the session while the tabs still exist.
10. **The editor renderer** follows ADR 0043's rule (`editor.renderer`, default `auto`).
    - The new `frost --selftest=render` draws one editor with the default renderer on the real platform and
      reads a frame back. On the hidden desktop through the no-box runner it passed three times: QRhi on
      Direct3D 11 (NVIDIA GeForce RTX 4070 SUPER), a 623x360 frame with text, no fallback.
    - So `auto`, and with it QRhi on Windows, stays the default. Raster stays the fallback and Linux's renderer.
    - `editor.renderer = raster` or `rhi` still overrides it. FrostProcess runs the self-test on its private
      desktop.
