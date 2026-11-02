<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0051: The integrated terminal (libvterm, PTY layer, shell, trust)

- Status: accepted (feature-first phase, ADR 0031)
- Date: 2026-11-02
- Plan reference: PLAN.md section 2 "Terminal", section 3.2 (`src/term`, layer `term`, rank 7), section 3.3
  (dedicated threads), section 3.5 (trust, launching); ADR 0011 (E2); ADR 0016 (keymap)

## Context
M4 needs a Rider-style terminal tool window: real shells on a pseudo terminal, xterm-compatible rendering,
tabs, and keys that reach the shell instead of the IDE. FrostIDE launches every child through
`io::IProcessLauncher` with an absolute path, and an untrusted project must not get code run on its behalf.

## Decision
1. **libvterm 0.3.3 is vendored** (MIT, Paul Evans) under `third_party/libvterm`: the release tarball's
   library sources only, pinned with the archive's and every file's SHA-256 in `third_party/vendor.lock.json`,
   cross-checked against the neovim/libvterm mirror at v0.3.3 (the generated `.inc` tables match its `.tbl`
   sources). It builds as the static C library `vterm` with the vendored C options (no FrostIDE warnings) and
   is linked PRIVATE into `frost::term`; no libvterm type appears in a public header (`VtScreen` is PLAN.md's
   IVtEngine seam). No patch is needed yet: libvterm already answers XTVERSION and reports unknown DECRQM
   modes (2026) as unrecognised. Any later patch lands as a separate, documented patch file beside the
   pristine sources and sets `modified` in the lock.
2. **The PTY layer** (`frost/term/pty.hpp`) is `IPtyLauncher`/`IPty`. On Windows it is the system ConPTY:
   the pinned Microsoft.Windows.Console.ConPTY 1.25.260930003 DLL and OpenConsole are deployed beside
   the application; the DLL is loaded by absolute path. If unavailable, a warning is logged and the
   system ConPTY entry points are used. On Linux it
   is `posix_openpt`/`grantpt`/`unlockpt` with `TIOCSWINSZ`. The child still starts through
   `io::IProcessLauncher`: `ProcessSpec::pseudoTerminal` carries the pseudo console handle
   (`PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`, standard handles `INVALID_HANDLE_VALUE` as in wezterm's MIT
   portable-pty) or the slave device path (opened by `posix_spawn` after its `setsid`, so it becomes the
   controlling terminal). The absolute-path rule, the environment handling and the containment are frost::io's.
   Each terminal has a reader thread; `close()` ends the tree (the Job Object on Windows; on Linux a hangup,
   then SIGKILL to every process of the shell's session, which reaches job-control jobs in their own groups,
   then the process group), closes the console and joins the reader. Because a parent that ignores Ctrl+C
   passes that on to its children, opening a Windows terminal restores normal Ctrl+C processing in FrostIDE.
3. **Screen and sessions.** `VtScreen` keeps the cells (UTF-8 text, wide characters, 16/256/RGB colours, SGR
   attributes), the cursor and its shape, the alternate screen, the title, the bell and the mouse and focus
   modes, plus a scrollback of `terminal.scrollbackLines` lines (default 10000). Keys go through libvterm's
   encoder in legacy mode. An independent libvterm parser observes Win32 input mode 9001 and kitty
   keyboard flags without changing vendored sources. Kitty flags 1 and 4 are supported; other requested
   flags are not advertised. Shift+Enter is CSI 13;2u when kitty disambiguation is active, a deliberate
   compatibility extension to kitty's legacy Enter rule. Pastes are bracketed when requested and cannot
   contain the end marker.
   `TerminalSession` adds the reader, writer and waiter threads; its notifications are coalesced per repaint.
4. **The shell** is `terminal.shell` (Application scope only, an `executes` key) or the default: PowerShell 7
   (`%ProgramFiles%\PowerShell\7`, then absolute PATH entries), else Windows PowerShell, else `cmd.exe` from the
   system directory; on Linux `$SHELL`, else `/bin/bash`, else `/bin/sh`. Detection never picks a binary inside
   the open project root. The environment adds `TERM=xterm-256color`, `COLORTERM=truecolor`,
   `TERM_PROGRAM=FrostIDE` and `TERM_PROGRAM_VERSION`.
5. **Trust rule.** With an untrusted project root the terminal is restricted: the application-scope shell,
   the user's home directory as working directory, and `NoDefaultCurrentDirectoryInExePath=1`. A trusted root
   (or a folder given to "Open in Terminal") is the working directory and the variable is removed (ADR 0011
   E2). Without a project the shell starts in the home directory.
6. **Keys.** While a terminal has focus, the shortcut dispatcher passes the keys of `terminalKeepsKey` and
   `terminalKeepsChar` to it (a `KeyKeeper` widget): Escape, Tab, Enter, Backspace, the navigation keys,
   Ctrl+letters, Alt+letters, plain F1-F10, Ctrl+Shift+C/V. Alt+digits, Alt+F12, Shift+Escape, Ctrl+Tab,
   other Ctrl+Shift combinations and modified function keys stay the IDE's.

## Consequences
- Kitty keyboard flags 2, 8 and 16, full physical-key and layout reporting, and an authenticated
  Claude prompt-state probe are not established by this slice. Qt's existing shortcut policy still
  reserves some combinations for the IDE.
- Scrollback lines are stored as trimmed cell vectors; a compact store and reflow-aware selection come later.
- Linux `containedProcesses` lists the shell's process group only; the close path still ends the whole session.

## S8 investigation (2026-11-02)
A no-prompt startup probe of the installed Claude executable used a separate `CLAUDE_CONFIG_DIR`,
disabled nonessential traffic and auto-update, and answered only VT queries. It stopped at the workspace
trust screen, with "No, exit" selected; no trust decision or prompt was submitted. With the bundled
ConPTY and the kitty query answered, the host observed primary device attributes, XTVERSION, kitty
keyboard query, push of flags 5, and DECRQM 2026 and 1016. This does not establish prompt-state behaviour
or modifier delivery to Claude. libvterm currently reports those two unimplemented modes as unrecognised.

A purpose-built child on the bundled ConPTY received Shift+Enter with `SHIFT_PRESSED` via
`ReadConsoleInputW`; a Linux raw-PTY child received `CSI 13;2u` after requesting kitty flags 5. These
are independent child tests and do not claim that Claude received the key. Qt native press, release,
modifier-side snapshots and focus-loss cleanup have offscreen tests; complete layout fidelity, every
kitty enhancement, real IME delivery through ConPTY and a real Claude prompt-state test remain open.
The Microsoft NuGet archive SHA-256 is
`02b07b349af66d801159bdf9e440d4a1ce78bb951f37fc8609731665afdae7ee`; its pin, MIT notice, SBOM entry,
and runtime system-ConPTY fallback are part of the implementation.

## Workbench and staging integration
The application Workbench owns the terminal integration and a PTY launcher backed by its existing
`IProcessLauncher`. Each new tab reads the current project root, trust store and application settings;
Explorer's Open in Terminal uses the selected path's directory, subject to the same trust restriction.
Shutdown cancels queued auto-open and closes the terminal sessions. The Windows staging script requires
both pinned runtime files and verifies that the staged copies match the build outputs.
