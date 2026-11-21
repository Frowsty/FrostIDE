<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0048: Git core over the git CLI

- Status: accepted (feature-first phase, ADR 0031)
- Date: 2026-11-02
- Plan reference: PLAN.md M5 (Git), section 2 (VCS notes); ADR 0019 (frost::diff, D6 histogram dialects),
  ADR 0016 (input), ADR 0037 (editor view), ADR 0038 (shell)

## B14 Git status measurement

The Windows runner uses the actual Git for Windows executable configured in
FrostIDE. Build `s1_probe_validate` before measuring. Prepare a new private
checkout from the manifest-pinned R3 source and keep the fixture idle afterward:

```powershell
tools\perf.ps1 -Budget B14 -B14Git $gitExe -B14Source E:\corpus\R3 `
    -B14Fixture E:\FrostBuilds\perf\b14-r3-private
tools\perf.ps1 -Budget B14 -B14Git $gitExe -B14Fixture E:\FrostBuilds\perf\b14-r3-private `
    -B14SessionProbe $sessionProbeExe
```

Run the measurement from a quiet, unlocked Windows console with an attached
powered monitor and keep keyboard and mouse idle. Preparation only reads the
original R3. The measurement owns the built-in fsmonitor daemon and index priming
in the private checkout. It uses the background VCS status arguments and checks
all twenty process exits and porcelain outputs. An independent scan before and
after the series and two untimed incremental IPC witnesses confirm corpus and
monitor state. Results use the shared median, nearest-rank p99 and absolute limits in
`tests/perf/budgets.json`. Preserve stdout as a JSON receipt. Preparation emits
`prepared`. A valid series emits `within` or `over`, with the raw samples.
Precondition failures or desktop input invalidate the series. This path records
an absolute B14 result. Reference-hardware qualification, cross-run statistics,
and full-release performance history remain separate.

## Context
M5 needs the everyday Git workflow: file status colours, change markers in the editor, the Commit window, a
side-by-side diff, branches, and fetch, pull and push. A Rider user's repositories use hooks, credential helpers,
LFS filters, `includeIf` configuration and worktrees. All of those must behave exactly as they do on the command
line.

## Decision
1. **The git CLI, not libgit2.** `frost::vcs` (layer vcs, Qt-free) runs the user's git. The lookup order is
   the setting, then Git for Windows' install paths, then PATH, resolved to an absolute path. Every process
   goes through `IProcessLauncher` with `killOnClose`, so the whole process tree ends with the call.
   - Hooks, filters, credential helpers, configuration and future git features then behave as they do in
     the terminal.
   - libgit2 has gaps (hooks, some filters, `includeIf` and fsmonitor variants), is GPL-with-linking-exception
     (another licence review) and is a second implementation to keep in step.
   - The cost is one process start per call, which matters on Windows; a persistent `cat-file --batch`
     is deferred.
2. **Environment.**
   - Every call gets `--no-pager -c core.quotepath=false -c color.ui=false`.
   - The environment is set to `LC_ALL=C`, `LANG=C`, `LANGUAGE` unset, `GIT_TERMINAL_PROMPT=0`,
     `GIT_EDITOR=:`, `GIT_SEQUENCE_EDITOR=:` and `GIT_MERGE_AUTOEDIT=no`. Output is therefore English, and
     git never blocks on a prompt or an editor.
   - Background reads (status, file contents, branch lists) add `GIT_OPTIONAL_LOCKS=0`, so they never
     contend with the user's own git for `index.lock`.
   - Commit messages go through `git commit -F <file in .git>`, never through the command line.
   - Credentials come from the user's credential helper (GCM on Windows). Without one, a push or fetch that
     needs credentials fails at once with an error that says how to set one up, instead of hanging.
3. **Restricted mode.** In an untrusted folder (PLAN.md section 3.5), git still reads:
   - status, HEAD and index contents, with `core.fsmonitor=false` and `core.hooksPath` pointing at an empty
     directory;
   - `cat-file blob` without `--filters`, so no clean or smudge filter runs.
   Commit, checkout, pull, push and rollback are refused with Untrusted. The actions that run project code
   are marked `executesProjectCode`.
4. **Status and refresh.**
   - Status is `git status --porcelain=v2 -z --branch --untracked-files=all`.
   - Submodules are listed through `.gitmodules`, but status never descends into them.
   - `GitService` keeps one immutable `VcsStatusMap` snapshot behind `IVcsStatusProvider`, so status
     queries are thread-safe and O(1).
   - A refresh runs after every command of our own. It also runs, debounced (300 ms, at most 2 s late), on
     watcher events in the work tree or on git's state files (`index`, `HEAD`, refs, `MERGE_HEAD` and the
     rebase directories); lock files do not count.
   - Reads and operations run on two serial workers, so a long push never delays a status refresh. A
     refresh whose status did not change notifies nobody.
5. **Change markers** diff the buffer against the HEAD blob (git's line model, `frost::diff` histogram).
   - The diff runs 300 ms after typing stops.
   - The editor gets a new gutter stripe kind (`DisplayChangeStripe` in `EditorDisplayList`), which every
     renderer backend draws from the list.
   - A click opens a popup with the old text and Rollback Lines (one undoable edit).
6. **Diff viewer.**
   - Side by side: HEAD or the index against the working copy.
   - Lines from `frost::diff`'s histogram diff, changed words from `refineHunk`.
   - The two sides scroll together through `mapLine`, and F7 / Shift+F7 move between changes.
   - Both sides are read-only. An editable right side needs the editor widget in that pane, which this
     slice does not attempt.
   - On the fixtures the hunks match `git diff --histogram` under ADR 0019 D6. They must be exact against
     classic-dialect git (WSL's 2.43). Against the newer dialect (Windows git 2.56) they must be exact, or
     git reports fewer changed lines, at most once. The new dialect itself waits on ADR 0019 Q3.
7. **Actions** use the IntelliJ ids (`vcs.commit`, `vcs.push`, `vcs.update`, `vcs.rollback`), whose shortcuts
   the IntelliJ keymap already binds. Show Diff (`Ctrl+D`) and Next / Previous Change (`F7` / `Shift+F7`) are
   module defaults with context keys:
   - `CommitViewFocus` for Show Diff and `DiffViewFocus` for the change navigation, both new and appended;
   - priority 1, so they win only in those views. Elsewhere Ctrl+D stays Duplicate Line and F7 stays Step
     Into.

## Deferred
- `frost-askpass` (credential prompts inside the IDE) and commit signing.
- A persistent `cat-file --batch` process.
- Partial (hunk) commits and changelists.
- Stash and shelve, and LFS-specific UI.
- An editable diff right side.
- Running the markers' diff off the GUI thread for very large files.
- The File System view's colours: the view does not exist yet; `vcsFileColorProvider` is its seam.
- Drawing the stripes in a QRhi backend: no such backend was in the slice's tree. Done at the merge into `main`,
  which has ADR 0043's QRhi backend: RhiGlyphPainter draws `changeStripes` as solid quads in the gutter.
- Wiring the widgets and actions into the workbench.
