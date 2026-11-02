<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0039: The W1 build tool windows

- Status: accepted (feature-first phase, ADR 0031)
- Date: 2026-11-02
- Plan reference: PLAN.md section 3.4 (UI), section 3.5 (trust); ADR 0016 (input), ADR 0032 (W1), ADR 0035
  (build service)

## Context
W1 needs the user-visible half of the build: the Solution Explorer, the Build output pane, the Problems list,
the Build actions with their shortcuts and the configuration combo, and the trust prompt. The pieces must dock
into m1/shell-frame's ToolWindowManager without depending on it. They must be testable offscreen without a
window, and they must respect the build service's threading: every callback runs on the service's own thread.

## Decision
1. **Widgets over small models, in `src/ui/toolwindows` (`frost::ui_toolwindows`).** Each tool window is a
   standalone QWidget (`SolutionExplorer`, `BuildOutputPane`, `ProblemsView`, plus the dialog `TrustDialog`).
   Each sits over a model that tests can drive directly:
   - `SolutionTreeModel` and `SolutionFilterModel` over the Qt-free `project::SolutionModel`;
   - `BuildOutputLog`, which is Qt-free;
   - `ProblemsModel` and `ProblemsFilterModel`.

   The shell owns placement, docking and persistence. Speed search, Copy Path and the context menu belong to the
   widgets. Open Containing Folder goes through `IProcessLauncher`: an absolute `explorer.exe` under
   `GetWindowsDirectoryW`, or `/usr/bin/xdg-open`, launched uncontained.
2. **One `BuildController` per main window is the only thing that talks to the build service.**
   - It reaches the service through `IBuildBackend`. `BuildServiceBackend` forwards to `build::BuildService`;
     tests use a fake.
   - It caches the state, the configurations, the diagnostics and the current build's output. A tool window
     opened mid-build therefore replays what it missed. The replay keeps at most 200 000 lines.
   - It turns refused requests into `requestFailed(message)` for the shell's notifications.
3. **Marshalling.** `serviceCallbacks()` returns callbacks that hold a shared relay, not the controller. The
   relay posts each call with `QMetaObject::invokeMethod(controller, ..., Qt::QueuedConnection)` under a mutex.
   The controller's destructor clears the relay's target. A callback after that, or a call still queued when
   the controller dies, is dropped. A Save-All request that cannot be delivered completes with `Cancelled`, so
   the build never hangs. The Save-All hook itself is marshalled the same way. On the GUI thread the hook calls
   the shell's `ISaveAllHandler`, which completes when it has finished saving. With no handler there is nothing
   to save, and the hook succeeds at once.
4. **Default shortcuts live in one table, `kBuildShortcutDefaults` (`build_actions.hpp`)**, one entry per
   action, in frost::input's Strict syntax.
   - Build Solution is `Ctrl+Shift+B`, Visual Studio's binding (IntelliJ's is `Ctrl+F9`). The user has not
     chosen yet, so the choice is one entry.
   - Cancel Build is `Ctrl+Pause`, which is Ctrl+Break.
   - Rebuild and Clean have no default.

   The table reaches the matcher through the new `KeymapService::setModuleDefaults`. A module default binds an
   action only when the built-in keymap does not name it, and the user's overrides win over both. Module
   defaults are never stored. This keeps the IntelliJ keymap a faithful transcription; Visual Studio bindings
   are not added to it.
5. **Shortcut conflicts are solved with priority, not removal.** IntelliJ binds `Ctrl+Shift+B` to
   `navigate.typeDeclaration`. The build actions register with priority 1. The matcher tries candidates in
   priority order, and the first enabled one wins. So `Ctrl+Shift+B` builds when a solution is open and idle,
   and otherwise reaches Go to Type Declaration.
6. **Enablement.**
   - Build, Rebuild and Clean need an open solution and no build in progress.
   - Cancel needs a build in any state that is not idle, NeedsTrust included.
   - The actions are not `executesProjectCode`. The service's NeedsTrust prompt, `TrustDialog` ("Trust and
     Open Project?"), asks before project code runs. Trust calls `trust()` for each listed root. Cancel cancels
     the waiting build.
7. **Problems rows carry a source (`Build` or `Live`).** W2's clangd diagnostics can then share the list, and
   each source can be replaced without touching the other's rows.

## Consequences
- The shell wires the widgets: one controller and one `BuildServiceBackend` per main window, a `TrustPrompter`,
  `BuildActions::registerActions`, and `setModuleDefaults(buildShortcutBindings())`. The openFile and
  openLocation signals go to the editor host.
- Changing a default build shortcut is a one-line edit with no keymap change.
- Deferred to the hardening phase (ADR 0031): grouping Problems by file, Show All Files, highlighting
  speed-search matches, capping very long output in the pane, and the review, sanitizer and perf work.
