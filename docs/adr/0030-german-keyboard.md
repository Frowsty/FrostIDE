<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0030: German keyboard support for the default keymap

- Status: accepted
- Date: 2026-11-01
- Plan reference: PLAN.md section 2.2 ("Keyboard and IME"); ADR 0016 decisions 3, 8 and 13 and its "User
  decisions (2026-11-01, evening)"; matrix row 227 (keymaps). Built in the feature-first phase (ADR 0031).

## Context
The user (ADR 0016, user decisions): "Support german keyboard layout"; for the xkb oracle "Do what you
recommend", with libxkbcommon and xkeyboard-config approved by name in their own ADR.

Before this slice, on the simulated Windows de-DE table, `Ctrl+Slash` (Comment Line) and `Ctrl+Shift+Slash`
(Comment Block) were unreachable except through the numpad: rule 3.3 dispatched the '-' key (US '/' position) by
its character to Key::Minus, so no key reached Key::Slash. pl-PL and fr-FR had no gap; X11 under the M1 Linux
policy (LettersByLabel) had none.

Shortcut facts, public documentation only (ADR 0016 decision 8; retrieved 2026-11-01):
- The German layout itself (the simulated de-DE table, golden `layout-mapping-de-DE.txt`): no key types '/' at
  base ('/' is Shift+7), so Ctrl+/ cannot be pressed as written.
- Rider Help, Settings | Keymap, "Use national layout for shortcuts": on Linux (option off by default) keys with
  special characters are read as on an English layout, so on a German keyboard Alt + / is pressed as Alt + -.
  On Windows and macOS the national layout is used (hard-coded on Windows), which leaves de without Ctrl+/.
No JetBrains keymap file, source or plugin table was consulted.

## Decision
1. **The German defaults are the IntelliJ defaults, read positionally at punctuation** (no German keymap lines):
   Ctrl+- (the key right of '.') is Comment Line, Ctrl+Shift+- Comment Block, Ctrl+ß and Ctrl+Num - Fold
   Collapse; menus show "Ctrl+-" and "Ctrl+ß". This is JetBrains' documented Linux behaviour, adopted on Windows
   too (where JetBrains' national reading leaves Ctrl+/ unpressable), so both platforms agree and the
   comment shortcuts sit on one key. `Ctrl+NumpadDivide` stays as the IntelliJ alternative. Every other default is
   unchanged: letters follow their labels (QWERTZ Ctrl+Z is the key labelled Z), named keys are themselves.
2. **Rule 3.6, positional preference** (`detail::computeLayout`, FullTable mode; documented in layout.hpp): at
   the 11 US punctuation positions, a character dispatch that is not a letter is re-pointed to the position's own
   key when another writing code already reaches the dispatched key; repeated to a fixpoint (at most 11 changes,
   ties by Code order). No key loses reachability. On the four simulated tables only de `Code::Slash` changes
   (Key::Minus -> Key::Slash); French ';' on Comma stays Semicolon because 'm' is on Semicolon. Swiss-like,
   backward-chain, swap and two-producer shapes are covered by synthetic tables; real de-CH, es and Nordic tables
   are M11.
3. **Adapter fallback** (`ui::translateKeyEvent`): a synthesized event without a scan code for one of the 11 US
   punctuation Qt keys resolves to the first writing code whose non-dead base level types the character, else
   `codeFor`, so QTest and automation Ctrl+Key_Minus on de matches the hardware '-' key.
4. **Reachability suite** (`KeymapReachability`, `tests/input/support/reachability`): every sequence of every
   action of the built-in keymap is driven through SimulatedKeyboard and the matcher on Windows and X11. de-DE and
   en-US must be complete with no numpad-only action; pl-PL and fr-FR are reported against an expected list (empty
   today, so no fix was needed); a no-'/' synthetic table proves the suite detects gaps.
   `KeymapReachability.GermanDailyShortcuts` presses the user's most-used shortcuts (ADR 0032) on the physical de
   keys on both platforms: Build (`build.project`, Ctrl+F9, the IntelliJ default the W1 build slice can register as Build Solution),
   comment line and block, find, go to file, back and forward (also AltGr+arrow on Windows), duplicate line, move
   line and statement, Find Action, undo/redo, Save All and double Shift.
5. **libxkbcommon and xkeyboard-config are approved by name** (the user, ADR 0016) for a test-only layout oracle
   and a later Linux active-layout reader. Their notices (read in WSL from the Debian copyright files of
   libxkbcommon0 1.6.0 and xkb-data 2.41) are MIT/X11-style plus HPND-style (HPND, HPND-sell-variant), with
   MIT-open-group, ICU and one custom permissive notice in xkb-data; none is copyleft. HPND is **not** added to the
   general allowlist in `tools/licensecheck.py`. Nothing is linked, shipped or copied by this slice.
6. **The oracle is deferred** to the hardening phase (feature-first, ADR 0031): no oracle code is committed.

## Consequences
- Every default shortcut is reachable on de-DE (Windows, and Linux under its M1 policy): 211 actions, 223
  sequences, 0 gaps on en-US, de-DE, pl-PL and fr-FR, both platforms. Informational dead-key strokes (they fire;
  the dead key is reported, not composed): de Alt/Ctrl+^ and Ctrl(+Shift)+´, fr Ctrl(+Shift)+^.
- Behaviour change on Windows de-DE: Ctrl+- was Fold Collapse, now Comment Line. Reversible in one rule.
- The Linux reader slice must add a named system-library entry to `tools/licensecheck.py` if shipped code loads
  libxkbcommon, and gets a FullTable de-DE equal to today's Linux mapping.
- Find Action parses US key names, so "ctrl+-" finds Fold Collapse while the menu shows Comment Line as "Ctrl+-";
  the Keymap page should search display text too (m1/shell-nav).
- `logicalKey` stays an O(1) read (B5); rule 3.6 runs at compile time for the simulated tables and in
  `readActiveLayout` on Windows (a few thousand comparisons).

## Alternatives considered
- Orphan-only recovery (re-point only a key nothing reaches): fails the Swiss and Spanish chains.
- Pure positional punctuation: loses fr ';' (Comma types ';', Semicolon types 'm').
- Ctrl+# or Shift+7-based German lines: layout-specific keymap sections; Shift+7 collides with bookmarks.
- Generated national keymaps (Keymap Nationalizer style): provenance.
