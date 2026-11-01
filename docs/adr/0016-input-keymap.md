# ADR 0016: Input, keymaps and actions (frost::input)

- Status: accepted
- Date: 2026-11-01
- Plan reference: PLAN.md section 2.2 ("Keyboard and IME", "Accessibility"), section 1.4 row 227 (keymaps),
  row 78 (Find Action), infra row I6, sections 3.3, 3.5 and 3.6, section 7 (M1); D24 (macOS unsupported)

## Context
M1 needs shortcuts that behave like Rider's on every keyboard layout the user may have, without Qt's
QShortcut machinery (no AltGr control, Qt in every consumer) and without OS behaviour that differs between
Windows and Linux. The M1 exit requires that typing on de-DE, pl-PL and fr-FR never fires a shortcut, that AltGr
text wins over Ctrl+Alt bindings, that double Shift is suppressed during IME composition, and that the IntelliJ
keymap is available on Windows and Linux. Find Action (row 78) needs a registry of named actions with context,
availability-level and trust gating; actions that run project code must stay disabled in restricted mode
(PLAN.md section 3.5).

Constraints: Qt only in `src/ui` and `src/app`; no exception crosses a module; the keystroke path is on the
keystroke-to-paint budget (B5) and may not allocate; start-up work is on B1. Tests may never switch, load or
unload the machine's keyboard layouts, and may never inject real input: layouts are data, and a deterministic
simulator produces the events.

## Decision
1. **Two libraries.** `frost::input` (`src/input`, layer rank 4, no Qt) holds the key model, layouts, the
   shortcut matcher, keymaps, the action registry, KeymapService and the Find Action model. `frost::ui_input`
   (`src/ui/input`, ui layer) is the Qt adapter: `translateKeyEvent` and `ImeTracker`. It lives in neither core
   (it is product logic with its own data) nor ui (the matcher must be testable without Qt and usable by the
   automation endpoint). FindActionModel at rank 4 cannot use `frost::search` (rank 7); when the M2 fuzzy
   matcher lands, either its core moves to a low layer or the model moves up. PLAN.md section 3.2 does not list
   `src/input` and `src/ui/input` yet; until it does, this decision is their record, and the `input` rank line in
   `cmake/FrostLayers.cmake` cites it.
2. **Key model.** `Code` is the physical key, named after W3C `code` values (from the Windows 9-bit scan code or
   the evdev code). `Key` is the logical key, named after its US-QWERTY key; bindings store Keys. Writing keys
   (KeyA..IntlYen) produce characters; typing keys (letters, digits, the 11 US punctuation keys, Space) may never
   be bound alone or with Shift only. `KeyEvent` carries an `altGr` flag separate from the modifiers,
   `KeyPlatform` (Windows, X11, Wayland) selects the platform semantics, and `osTimeMs` is the OS event time
   (32-bit, wrapping). The key list's X-macro parameter is a lowercase `x`, because an uppercase `X` also
   replaced the key name `X` (found by the keymap tests: `Ctrl+X` did not parse).
3. **Layout-aware mapping (FullTable mode)**, computed once per table by `detail::computeLayout`:
   1. named keys map to themselves (NumpadEnter to Enter; the NumLock-off numpad remap is applied per event);
   2. digit-row keys are positional (AZERTY Alt+1 stays Alt+1);
   3. other writing keys: a Latin letter maps to that letter (QWERTZ Ctrl+Z is the key labelled Z); one of the
      11 US punctuation characters maps to that key (French ',' on KeyM is Comma); anything else maps to the
      US key of its position on punctuation positions, to the US letter on letter-area positions unless another
      key produces that letter, and to nothing on IntlBackslash, IntlRo and IntlYen; a non-Latin layout is
      positional throughout;
   4. LettersByLabel (Linux in M1): letters follow the event's keyCharHint (the printable ASCII character Qt
      reports as the key, of the current Shift level); a letter-area key whose hint is one of the 11 US
      punctuation characters is that key (character dispatch, as rule 3: French ',' on KeyM is Comma) and one
      whose hint is any other ASCII character ('?') is None, so a punctuation-labelled key never fires its
      position's letter bindings (a review finding: on fr-FR, Ctrl+Alt+',' fired Extract Method and Ctrl+Shift+'?'
      Matching Brace); everything else is positional.
   Facts from the simulated tables (goldens `layout-mapping-*.txt`): de-DE swaps Y and Z, dispatches ',' '.' '-'
   by character and has no '/' (so Ctrl+Slash and Ctrl+Shift+Slash are reachable only through their numpad
   alternatives; KeymapService reports them as unreachable); fr-FR remaps A/Q, Z/W and M; pl-PL equals en-US at
   base and Shift. `labelFor` prefers a character-dispatched code over a positional one (de Key::Minus shows
   '-', not 'ß'). Known divergence: de '+' is BracketRight, so de Ctrl++ is editor.codeBlockEnd; fold expand
   stays on Ctrl+´ (Equal) and Ctrl+NumpadAdd.
4. **AltGr.** When AltGr (Windows right Alt on an AltGr layout, X11 ISO_Level3_Shift) produces text or a dead
   key, the press is text and passes through (cancelling a pending stroke). AltGr with nothing to produce matches
   as Ctrl+Alt. Physical left Ctrl + left Alt matches Ctrl+Alt bindings even where Windows would produce AltGr
   text: on pl-PL, LCtrl+LAlt+L is Reformat Code while RAlt+L still types 'ł'. `typedText()` decides what a
   widget inserts: printable text with no Ctrl/Alt/Meta, with AltGr, or with Windows Ctrl+Alt; never Windows
   Alt+letter (WM_SYSCHAR). The Windows adapter tells AltGr from Ctrl+Alt by the per-side bits of
   `QKeyEvent::nativeModifiers()` (`frost::ui::windows_native`: ShiftLeft 0x01, ControlLeft 0x02, AltLeft 0x04,
   MetaLeft 0x08, ShiftRight 0x10, ControlRight 0x20, AltRight 0x40, MetaRight 0x80; AltGr reads 0x42), discarding
   AltGr's fake left Ctrl. Qt reads them while it processes the key message, but queues the QKeyEvent and delivers
   it only after the dispatcher has retrieved later messages. The first design read the live key state
   (`GetKeyState` through an `IKeyStateProbe`) at delivery, which during a UI stall could already show a later
   AltGr release, so pl-PL AltGr+L ran Reformat Code instead of typing 'ł', or the reverse (a review finding).
   `NativeKeyDelivery` reproduces both cases on a real, never-shown window (posted messages and the test thread's
   own key state only) and fails with the live-state logic. The bit values are undocumented Qt internals, observed
   on Qt 6.12; `NativeKeyDelivery.NativeModifierBits` pins them, so a Qt upgrade that changes them fails the
   tests. Events without per-side bits (QTest, the automation endpoint) keep Ctrl+Alt as reported. The refinement
   runs whenever Ctrl+Alt is reported with the right-Alt bit and a Ctrl bit on an AltGr layout, also when Qt
   already reports GroupSwitchModifier (the `windows:altgr` option, and the `Key_AltGr` key itself). The AltGr
   key's own press is AltGr, not Alt. Synthesized
   events without a scan code take their `Code` from the Qt::Key: named keys by table, letters, digits and the 11
   US punctuation keys through the layout, and other printable characters (Key_Question, Key_Adiaeresis) through
   the writing key whose base or Shift level produces them.
5. **Dead keys.** A dead press without Ctrl/Alt/Meta passes through (or cancels a pending stroke and is
   consumed); the OS composes. Consuming does not clear the OS's own dead state, so the next character may still
   compose. On Windows the adapter reads the dead flag from the layout table first (Qt reports no dead key on
   some paths), then `Qt::Key_Dead_*`; X11 and Wayland use `Qt::Key_Dead_*` only.
6. **Sequences.** At most two strokes. A two-stroke prefix beats a single-stroke binding of the same stroke
   (BindingIndex::conflicts reports it). An unmatched second stroke, Escape included, is consumed and cancels.
   There is no timeout: `reset()` cancels (focus change, mouse press, IME start, Qt shortcut, layout change).
   Autorepeat of the key that completed or cancelled a sequence is swallowed until its release; autorepeat
   while pending is consumed. `Code::Unknown` doubles as "no swallowed key", so an unmapped key's autorepeat is
   never swallowed. A bare second stroke (`Ctrl+K, C`) is allowed.
7. **Double tap (DoubleShift, DoubleCtrl).** Four events, press-release-press-release of one modifier class,
   with no other key or modifier in between, each gap at most T (inclusive), firing on the second release (so
   Shift+letter never fires it). Gaps use unsigned 32-bit OS-time differences (wrap-safe); if any event lacks an
   OS time, all four use the steady clock. T is the user's preference or the platform double-click interval
   (the shell passes `QStyleHints::mouseDoubleClickInterval()`); 0 disables gestures. IME composition resets the
   detector. AltGr's fake left Ctrl is followed by right Alt, which resets the detector.
8. **Keymap format v1** (`frost-keymap 1`): UTF-8, comments on their own lines, `id` and `name` directives,
   sections `[default]`, `[windows]`, `[linux]`, `[macos]`, lines `action.id = A | B` in the Strict syntax, and
   `action.id =` for an explicit unbinding. Errors are Corrupt with line and column (a duplicate header is
   reported as such), Unsupported for other versions, OutOfRange over 1 MiB; lines are at most 4096 bytes. An
   action lists at most 8 sequences (`kMaxSequencesPerBinding`, as for an override), and a file whose canonical
   form would be over 1 MiB is OutOfRange too, so `parseKeymap(formatKeymap(k)) == k` holds for every parsed
   keymap: the canonical form's " = " and " | " spacing made a compact 4094-byte line 4790 bytes long (a review
   finding; seed `seed-18-compact-long-line.keymap`). Duplicate actions are found with a per-section `std::set`
   (O(log B) per line), because a sorted-vector insert was quadratic for ids in descending order (0.88 s for
   80,000 lines), and imported keymaps (row 227, M11) are untrusted input.
   Resolution: a platform section line wins, else the default line; on macOS only, unlisted default lines are
   derived by swapping Ctrl and Meta. Every derivation collision is resolved in `[macos]` by a fact from the
   public macOS reference card or by an explicit unbinding; the MacOSConflicts test enforces it. macOS is
   defined but untested (D24). The IntelliJ data is written from public documentation facts only: the IntelliJ
   IDEA Help pages "Predefined Windows keymap" and "Predefined macOS keymap", and the published default-keymap
   reference cards. These are documentation, not code; no JetBrains keymap file or source was consulted, and
   doubtful entries are left out. The keymap is a raw string in `keymap_intellij.cpp`, parsed on each
   `builtinKeymap()` call (B1).
   Action ids are `group.camelCaseName`, with the groups editor, code, search, navigate, refactor, window, file,
   view, vcs, run, debug, build, bookmarks, tools and agent. The keymap may name actions later milestones
   register; they are inert until registered. `BindingIndex::build` applies overrides through an id-to-position
   map, O(B + O), because a rebuild can run on the keystroke path with up to 4096 overrides. `KeymapService`
   rebuilds a stale index (the registry grew) in `handle`, `peek` and in every binding query (`shortcutsFor`,
   `primaryShortcut`, `bindings`, `unreachableBindings`), each time incrementing `bindingsGeneration()`.
9. **ActionRegistry.** Entries live in a `std::deque` (stable references for `info()` and the callbacks), with
   a sorted id index. Registration order is kept; ids are never removed in v1. `whyDisabled` checks, in order,
   NotApplicable (the context rule), Level (the availability level), Untrusted (`executesProjectCode` in an
   untrusted context, PLAN.md section 3.5), NoHandler (a placeholder) and Callback. Trust and level gating
   disable an action rather than hide it, so menus and Find Action can say why. The gate applies to every
   InvokeSource; `tryInvoke` re-checks it. A gated action never fires, never makes a prefix pending and never
   completes a gesture; the next enabled candidate of the stroke fires instead. The registry trusts the
   `ActionContext` it is handed: the shell builds `trusted` and `level` from the live trust and level state for
   every key event and every invoke (a Find Action popup reuses only the focus keys of its opening context), and
   no frost::input component caches them. Moving trust behind a provider interface the registry queries itself
   is a question for the trust slice (see the slice report).
10. **Find Action.** Deterministic integer scoring over names and synonyms (exact, prefix, word-prefix
    segments, substring, subsequence; synonyms 50 lower), plus a shortcut tier only for queries that parse as a
    shortcut and contain a modifier, a function key or an explicit separator, so "up", "end" and bare letters
    never match by shortcut. Inapplicable and non-searchable actions are excluded; disabled ones are listed after
    the enabled ones of the same score with their reason. The order is total and independent of registration
    order. Usage counts are session-local by string id; the shell persists them. Names and synonyms are decoded
    per query (0.43 ms for 500 actions); a per-generation cache is a TODO(M2) if the M2 action set needs it.
    frost::input has one private UTF-8 decoder (`src/input/src/utf8.hpp`), used by the keymap parser (which
    needs the offset of the first bad byte) and by Find Action; frost::input does not link frost::io.
11. **Settings seam.** `IKeymapSettings` (load/store of KeymapPreferences: keymap id, double-tap interval,
    overrides as Strict strings) is application-scoped, never project-shared and never an `executes` key. Loading
    sanitises per entry (valid overrides apply even when others are invalid) and never stores. The
    implementation over m1/settings belongs to the composition root; it performs no I/O on the UI thread.
    `MemoryKeymapSettings` (with fault injection) is the test double.
12. **Active-layout reader.** Windows: `GetKeyboardLayout(0)`, then per writing key `MapVirtualKeyExW` and
    `ToUnicodeEx` with flag 0x4 (keyboard state unchanged), so a dead key the user has pending is never
    consumed. It only queries the calling thread's current layout and never changes the user's input
    configuration. Linux in M1: `linuxM1Layout()`, the en-US table in LettersByLabel mode (letters follow labels,
    a letter-area key labelled with punctuation follows its label or is None, other punctuation is positional).
    The guard test `input.NoLayoutSwitching` scans the sources for the OS calls and
    tools that would change or load layouts or inject input, and a fixture proves the scan detects each name.
13. **Oracles.**
    - Independent: the read-only Win32 cross-checks on whatever layout is active (`MapVirtualKeyExW` characters
      and dead flags, `VkKeyScanExW` round trips, named scan codes), equality with the simulated en-US
      table when the active KLID is 00000409 (it is on REF-W1, and the check passes), and Qt's own Windows
      plugin in `NativeKeyDelivery` (real message processing and queued delivery; not real input).
    - Consistency checks: the layout, keymap and Find Action goldens, the simulator-driven suites
      (ShortcutLayouts, ShortcutSequences, DoubleShift, the random-stream property) and the reader over a
      scripted query.
    - Not adopted: a full Linux reader through libxkbcommon (a new direct dependency; question Q1 in the slice
      report), reading the Windows kbd*.dll tables (needs the WDK struct layout, a legal question; Q2), and the
      optional xkeyboard-config data oracle (deferred; it would read data files shipped with xkb-data,
      read-only at test time, link and ship nothing; xkb-data's copyright file mixes MIT and HPND-style
      notices, and HPND is not on the allowlist, so this waits for the user's answer).

New dependencies: none (no vcpkg port, no system library; Qt Gui only, already allowlisted). Budgets: no new IDs;
`// Budget: B5.` marks the keystroke path and `// Budget: B1.` builtinKeymap and KeymapService::create.

## Measurements
REF-W1, clang-cl Release (win-clangcl), `input_perf_tests`, medians of three runs:
- `builtinKeymap("intellij")` plus `BindingIndex::build` (the whole IntelliJ keymap): 0.199-0.205 ms (bound 1 ms,
  B1; 0.171-0.192 ms before the canonical-size accounting).
- `ShortcutMatcher::feed` over 1M mixed de-DE events: 17 ns per event (bound 2 µs, B5).
- Find Action query over 500 actions: 0.42 ms (bound 2 ms).
- `parseKeymap` of 80,000 explicit unbindings: 24.6-27.0 ms with ids ascending or descending, a ratio of
  0.99-1.02 (bound 3); the review measured 13.8 ms ascending and 877 ms descending with the sorted vector.
- Allocations on the keystroke path: 0 bytes over 10,000 mixed `handle`/`peek` calls after warm-up
  (`input_alloc_tests`, Debug and Release).
- Tests: input_tests 223, input_stress_tests 8 (50,000 streams of 32 events per layout and platform, 49-66 s
  in Debug), input_alloc_tests 1, input_layout_os_tests 5, input_perf_tests 4, ui_input_tests 31,
  ui_input_native_tests 3 (Windows only), and the two guard tests.

## Consequences
- The composition root (m1/shell-frame with m1/settings) implements IKeymapSettings over the application keys
  `keymap.id`, `keymap.doubleTapIntervalMs` and `keymap.overrides`, posts live reloads to the UI thread and calls
  `KeymapService::reloadPreferences()`. frost::settings does not link frost::input.
- m1/shell-frame owns the dispatch filter: `ImeTracker::observe` on every event (and `reset()` on a composition
  start); ShortcutOverride accepted when `peek()` is not PassThrough; every press through `handle()` exactly
  once; Fire runs `tryInvoke` and `noteInvoked`; Pending and Consumed are consumed; `reset()` on QEvent::Shortcut,
  mouse press, focus change and deactivation; `readActiveLayout()` and `setLayout()` on KeyboardLayoutChange;
  menus show `toDisplayText(primaryShortcut)` and refresh when `bindingsGeneration()` changes; QAction shortcuts
  are never set for keymap-bound actions; window.focusToolbar and window.focusStatusBar get registered.
- m1/editor-view and m1/editor-model register the editor actions and the EditorFocus and EditorReadOnly
  contexts, and insert text from a PassThrough press only through `typedText()`.
- m1/shell-nav builds the Find Action popup on FindActionModel with the context captured before the popup
  opened, persists usage, shows names as plain text, and builds the Keymap page on `strokeFromEvent`,
  `conflicts()`, `unreachableBindings()` and DisabledReason.
- m1/automation-bench verifies the real native key path (scan-code encoding, the Windows dead-key signal,
  modifier conventions including the `windows_native` bits under real AltGr and Ctrl+Alt input, timestamps) on
  the real desktop; this slice tests synthesized QKeyEvents and posted messages only.
- Fuzzing: `tests/fuzz/corpus/input_keymap` and `runKeymapFuzzInput` are ready; once FrostFuzz.cmake is on the
  base (m1/piece-tree), the integrator registers `input_keymap` and deletes the KeymapFuzzReplay test.
- M11: desktop-environment conflicts on Linux (GNOME and KDE take Ctrl+Alt+arrows and Ctrl+Alt+L), the other
  built-in keymaps, and macOS once a test leg exists.
- Known approximations: macOS `code.smartCompletion` stays derived (Shift+Meta+Space), as no override for it
  was confirmed for this slice. Until a real Linux layout reader lands: punctuation keys outside the letter area
  are positional; Qt reports the Shift level's character, so a letter-area punctuation key is reachable only
  where that character is US punctuation (fr-FR Ctrl+Shift+Comma cannot be typed on KeyM, which reports '?');
  and a letter-area key whose hint is not ASCII (no Qt::Key value in 0x21..0x7E) stays positional.

## Alternatives considered
- JSON keymaps with yyjson: a dependency m1/settings adds with its own ADR, so a merge clash; the line format is
  also easier to review.
- QShortcut and QKeySequence: Qt in the core, no AltGr control, no layout policy of our own.
- Windows virtual-key dispatch: Windows-only semantics (de Ctrl++ would become fold expand only on Windows).
- Pure positional mapping (VS Code's keyCode style): Ctrl+Z would sit on the Y label on QWERTZ.
- Pure character dispatch: AZERTY digits and many punctuation bindings become unreachable.
- A general "owned elsewhere" exclusion for punctuation: needs a global table Linux cannot provide; kept only for
  letter-area keys in FullTable mode.
- Firing double Shift on the second press: it would fire during Shift+letter.
- Steady-clock-only tap timing: spurious double taps after stalls (the OS time stamps the real key times).
- A sequence timeout: it would fail slow second strokes, and reset() covers the cases that need cancelling.
- Oracles built by loading layouts: they change the user's layout list, which is banned.
- Reading kbd*.dll tables: needs the WDK struct layout, a legal question (Q2).
- A frost-lint rule for the layout ban: a tools edit outside this slice; a CTest script was chosen.
- A separate keymap file under `resources/`: needs a frost-lint suffix entry and an embedding step; a raw string
  in a .cpp was chosen.
