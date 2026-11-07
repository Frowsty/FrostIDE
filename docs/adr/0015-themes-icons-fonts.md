# ADR 0015: Themes, icons and fonts

- Status: proposed
- Date: 2026-11-01
- Amended: 2026-11-01 by ADR 0029 (m1/large-file is m15/large-file, so the atlas mapping to-do is `TODO(M15)`;
  nothing else changes)
- Amended: 2026-11-07 (r0/fnd-1-green-head): decision 9a records the eight editor font families bundled on
  2026-11-05 (d540658) and their Reserved Font Names; licensecheck now cross-checks those names (decision 10)
- Plan reference: PLAN.md section 2 ("Theming", "Icons and fonts"), 2.1 (`frost/compat`, banned features),
  3.4 (resources), 4 (B1, B5, B19, B28), 6.1 (test oracles), 8 (L1, L7), 9 (D13); milestone M1;
  feature matrix rows 243 ("themes") and 244 ("compact mode, IDE zoom, HiDPI")

## Context
M1 needs the look of the IDE before the shell exists: island themes (dark, light, a flat classic theme and High
Contrast) that follow the OS, compact mode and an IDE zoom that scale every metric, crisp icons at every common
display scale, and a bundled UI and editor font so screenshots and layout never depend on what is installed.
PLAN.md fixes the constraints:
- Qt Widgets with Fusion as the base style; only `src/ui` and `src/app` see Qt.
- Start-up is budgeted (B1): no JSON parsing and no SVG rendering at start-up.
- Icons come from Lucide until our own set exists (D13). No JetBrains assets, names or theme files (L1); JetBrains
  Mono is allowed under the OFL (L1). Fonts need a licence verification record (L7).
- Tests need independent oracles (6.1); no CI, everything runs locally on Windows 11 and WSL Ubuntu 24.04.

The slice lands `frost::ui_model` (Qt-free: the key registry, theme blobs, contrast, scale, settings, OS
detection, the icon atlas format), `frost::ui_theme_compiler` and `frost-themec`, `frost-icon-baker`, the build of
`resources/`, `frost::ui` (FrostStyle, AppearanceManager, the icon runtime, fonts, islands) and the tests.

## Decision
**Decision 1. FrostStyle, a QProxyStyle over Fusion.**
- The palette comes from the theme for every role of the Active, Inactive and Disabled groups:
  Window = Panel.background, Base = TextField.background, Highlight = List.selectionBackground
  (Inactive: List.selectionInactiveBackground), HighlightedText = List.selectionForeground, Accent =
  Component.focusColor, and so on. Light, Midlight, Mid, Dark and Shadow derive from Component.borderColor with
  fixed factors: lighter(150), lighter(125), the border itself, darker(150) and darker(200).
- The palette is applied with `QApplication::setPalette` and assigned wholesale in `polish(QPalette&)`. The
  platform palette otherwise wins: the windows plugin applies its own palette and per-class fonts
  (`ui_native_tests` proves the theme wins, with `QT_QPA_PLATFORM=windows`).
- Metrics go through UiScale: icon sizes, scroll bar width, focus margins, button margin, layout spacing and
  margins, and Fusion's check and radio indicator sizes times the zoom. `sizeFromContents` makes heights at least
  Button.height, TextField.height, List/Tree.rowHeight, Menu.itemHeight and TabbedPane.tabHeight (Fusion's
  height when that is larger).
- FrostStyle draws buttons, text fields, focus rings, check boxes, radio buttons, item-view rows, menus (the panel
  in Menu.background, items in Menu.foreground and Menu.selection*), the tab-widget frame, the tab shape (a 2 px
  accent underline), combo boxes (a frame, fill, label and triangle arrow in the ComboBox.* keys), a tree's
  selected text (Tree.selectionForeground, unless the model or widget set a colour), the default button's label
  colour (Button.default.foreground) and icon-only tool buttons' icons. Everything else is Fusion with our
  palette. Registry keys nothing draws yet (List and Tree background and foreground, TextField.selection*,
  ScrollBar.*, the separators, StatusBar.foreground) are listed as reserved in `theme_registry.hpp`, with the
  slice that wires each.
- A focused button's ring is Component.focusColor on its outer edge. A default button adds a
  Button.default.foreground line just inside the ring (a two-colour indicator, WCAG 2.2 technique C40), because
  its fill is the focus colour itself in Glacier Light and close to it in the dark themes (1.69:1); no single
  colour reaches 3:1 against both Light's white island and its blue fill.
  `FrostStyle.FocusIndicatorVisibleOnEveryButton` checks every theme, both button kinds, resting and hovered, at
  100, 150 and 200%: the pixels that change on focus change by 3:1 or more along at least a one-device-pixel line
  around the straight edges.
- IslandFrame draws Island.borderColor with its alpha multiplied by Window.inactiveAlpha while its window is
  inactive and repaints on activation changes (`IslandFrame.InactiveWindowFadesTheBorder`).
- Every drawing path takes the device pixel ratio from the painter's device transform, not from
  `painter->device()`: `QWidget::render` into a QImage redirects the painter to the widget, whose ratio is its
  screen's (1.0 in a dpr-1 process), while the device transform carries the target's 1.5. Shape edges and icon
  origins are snapped to device pixels, so borders are whole device pixels at fractional scales and the in-process
  render equals Qt's backing store (decision 11).
- Qt style sheets and application style sheets are rejected: they bypass the style for every widget they touch,
  cost per-widget CSS matching, and `FrostStyle::current()` and the icon tint depend on our style being the
  application style.

**Decision 2. The `.theme.json` subset.** A subset of the IntelliJ Platform theme format as documented on the
public SDK page "Themes: Customize UI Theme Properties": `name`, `dark`, `colors` (named colours as `#RRGGBB` or
`#RRGGBBAA` literals), `ui` (nested and dotted keys, and the `"*"` wildcard, allowed only directly under `ui`).
FrostIDE extensions: `highContrast`, `compact` (values for keys the registry marks hasCompact) and
`icons.ColorPalette` keyed by icon role (`Icon.Default`, `Icon.Error`, ...), every role required. Each registry key
resolves to an explicit value, then the wildcard entry for its last segment, then its registry fallback, else the
error "unresolved key". Compilation is strict at build time: unknown keys and literal duplicate members are errors
(lenient mode, for M11's importer, makes them warnings). A Qt-free pre-scan finds duplicate member names before
QJsonDocument collapses them. Inputs are limited to 1 MiB and depth 8. Diagnostics stay small whatever the
input's shape: at most 32 duplicate member names are listed (one more diagnostic counts the rest), paths and
messages are elided beyond 256 and 1,024 bytes, and a `ui` key longer than any registry key is reported once
without descending. Without these bounds a long name over many duplicate or unknown members produced output about
the square of the input (gigabytes at 1 MiB); `ThemeCompiler.DiagnosticsStayBoundedOnHostileInput` covers objects,
arrays, `ui` and the 1 MiB limit.

**Decision 3. The compiler is C++ on QJsonDocument** (`frost::ui_theme_compiler`, `frost-themec`): no new
dependency, and the same code imports user themes in-product in M11. yyjson arrives with m1/settings under its own
ADR; moving the compiler to it later makes the `.theme.json` fuzz target Qt-free (TODO(M1)).

**Decision 4. Theme blob v1** (`.ftheme`, about 530 bytes: the shipped blobs are 528 to 546). A 48-byte header
(magic, version, flags, the registry hash, the registry's counts, lengths, total size, an FNV-1a 64 checksum over
everything but the checksum), then
64 ARGB colours, 21 metrics times two (normal and compact) as f32, 8 icon role colours, the id and the name.
`kRegistryHash` covers every key name, opacity flag, metric unit, range and compact flag, and the role names, so a
blob from another registry is refused (Unsupported) rather than misread. Lookup is O(1) by key index; start-up
parses no JSON. The decoder validates every field before use; every single-byte corruption is detected.

**Decision 5. Four themes and Sync with OS.** Glacier Dark (the default), Glacier Light, Glacier Classic (flat,
no island gap) and Glacier High Contrast. Settings: `theme` (an id or `sync-with-os`), `syncLightTheme`,
`syncDarkTheme`, `syncHighContrastTheme`, `density`, `zoomPercent`; the default is Glacier Dark with sync off.
Sync resolves: OS high contrast to the sync high-contrast theme, Dark to the sync dark theme, Light to the sync
light theme, and Unknown to the sync dark theme. Detection:
- Windows: `RegGetValueW` on `HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme`
  (absent means Light) and `SystemParametersInfoW(SPI_GETHIGHCONTRAST)`, combined with Qt's
  `QStyleHints::colorScheme` and `contrastPreference` (the registry wins when definite).
- Linux: Qt's style hints first, then a case-insensitive `GTK_THEME` look (`:dark` or `-dark`, `highcontrast`).
  Without a session bus the result is Unknown, which is specified, not skipped.
- FrostIDE never calls `QStyleHints::setColorScheme`. At start-up a missing, truncated or unreadable chosen
  theme falls back to Glacier Dark with a warning in the InstallReport; a broken default theme or a missing
  primary atlas fails `install()` without touching the application's style, palette, font or fonts. The
  fallback stays while the settings keep resolving to the failed theme (density and zoom changes apply to it,
  and the failed load is not retried); choosing another theme loads it, and returning to the broken one is then
  an apply error that keeps the current theme.

**Decision 6. The contrast policy** (`frost/ui/model/contrast.hpp`), checked on every shipped theme by the
compiler tests and again on the installed blobs and the rendered pixels by the screenshot tests:
- IslandSeparation 1.20:1 between islands and the window, in every theme (PLAN.md 2, "Theming");
- Text and SecondaryText 4.5:1 (WCAG 2.2 SC 1.4.3), 7:1 in High Contrast (SC 1.4.6);
- DisabledText: none in normal themes (WCAG exempts inactive components), 4.5:1 in High Contrast;
- NonText 3:1 in every theme (SC 1.4.11): focus rings, the unchecked check box border, icon roles on their
  surfaces (Disabled exempt; the High Contrast icon pairs are held to 7:1 by the compiler test);
- Border: none in normal themes, 3:1 in High Contrast.
Translucent backgrounds are composited over a declared underlay first. The selection text pair is checked on both
the active and the inactive selection background (HighlightedText is List.selectionForeground in every palette
group); this pair moved High Contrast's inactive selection from `#4D4D4D` to `#A0A0A0` (black text, 8.0:1).
Every drawn text colour has a Text pair on every fill it is drawn on: button labels on the hover and pressed
fills, the default label on Button.default.hoverBackground, ComboBox.foreground on ComboBox.background, and
Tree.selectionForeground on both tree selections. The default hover pair moved Glacier Dark's and Classic's
Button.default.hoverBackground from `#3F7AE3` (white text 4.11:1) to `#3166CC` (5.38:1).

**Decision 7. The icon pipeline.**
- Qt Svg is linked only by `frost-icon-baker` (a build tool); the runtime never renders SVG. licensecheck reads
  Svg as an icon-baker use only, frost-lint's `qt-svg` rule refuses Qt Svg includes outside `**/icon_baker/**`,
  and the baker links `frost::ui_model`, never `frost::ui`.
- The baker accepts a strict SVG subset: a root `<svg>` with a square viewBox, no `<text>`, no DTD or entities,
  Tiny 1.2 features only, SMIL and CSS animations off; a Qt Svg warning fails the bake. It normalises strokes:
  `strokePx` maps each design size to a stroke width in logical pixels (16 px: 1.0, 20 px: 1.25 for the Lucide
  subset), written as `stroke-width = strokePx x viewBox / designSize` on the root (a child stroke-width is
  refused).
- FIA v1, one file per scale (`icons-<pct>.fia`): a 64-byte header with a table checksum and a `sourceHash`
  (FNV-1a 64 over the format version and the length-prefixed manifest and SVGs, equal in every atlas of one
  bake), sorted 32-byte entries, an id table and 64-byte-aligned pages. Alpha8 pages hold tint masks; Argb32
  premultiplied pages hold untinted icons, with optional `_dark` variants for dark themes (exercised by the baker
  fixtures; every shipped Lucide icon is tinted). Only the canonical encoding parses. Page numbers and the page
  count are 16-bit fields, so packing refuses a bake that needs more than 65,535 pages (OutOfRange) instead of
  wrapping the page index.
- Scales 100, 125, 150, 175, 200, 250 and 300%; design sizes 14, 16 and 20 px (the Lucide subset bakes 16 and 20).
  A design size's device size is `lround(designSize x scale)`, so 14 px is 18 px at 125% and 25 px at 175%.
- Selection: an exact device size with the wanted design size, then an exact size with any design size, then
  the smallest larger cell (wanted design first), then the largest cell, upsampled. Unbaked scales (110%, 225%)
  downsample the next larger cell with SmoothTransformation and tint after resampling. Compact mode switches the
  toolbar slot from the 20 px to the 16 px design without recreating the QIcon.
- Atlases are read through IFileSystem (fault-injectable, long-path safe), not mapped: they are 43 to 353 KB,
  and only the primary screen's scale is read at start-up (0.21 ms cold for 100%): the scale the selection rules
  pick for a Small icon of the theme's Icon.size there, so 125% zoom on a 150% screen preloads 150% (the 20 px
  design is exactly 30 device pixels) rather than 200%. PLAN 3.4 says "mapped"; the
  mapping API is TODO(M15) with m15/large-file (question 4). A failed lazy load is remembered and the next best
  loaded atlas serves; atlases of different bakes are refused.
- Tinted pixmaps are cached by (scale, entry, device size, mode, tint ARGB, dark, dpr) in an 8 MiB QCache
  (Budget: B19), so a theme switch re-tints existing QIcons. A hit still chooses the cell first (two O(log n) id
  searches, at most 21 candidates and a few small allocations), then makes one cache lookup and the drawPixmap
  (Budget: B5); `AppearancePerf.IconPixmapCacheHit` measures 1,000 hits per sample.

**Decision 8. Lucide 1.66.0 is the D13 stopgap.** 47 icons (`resources/icons/icons.json`; `toolwindow.history`
uses `rotate-ccw-clock` because Lucide has no `history` icon), unmodified, licence "ISC AND MIT": ISC for Lucide
(Copyright (c) 2026 Lucide Icons and Contributors) and MIT for the Feather-derived icons (Copyright (c)
2013-present Cole Bemis). The subset is annotated as a whole because Feather's list uses pre-rename names. No
JetBrains icons (L1). Our own set replaces Lucide in a later slice.

**Decision 9. Fonts.** L7 verification record, 2026-11-01:
- Inter 4.1 (`Inter-4.1.zip`, SHA-256 `9883fdd4…`): LICENSE.txt read from the pinned archive and equal to the file
  at tag v4.1; SIL Open Font License 1.1; "Copyright (c) 2016 The Inter Project Authors"; no Reserved Font Name.
  The static TTFs (Regular, Medium, SemiBold, Bold) are single-source (the tag does not track them); their name
  tables carry the OFL (nameID 13 and 14) and the holder (nameID 0).
- JetBrains Mono 2.304 (`JetBrainsMono-2.304.zip`, SHA-256 `6f6376c6…`): OFL.txt and every bundled TTF (Regular,
  Italic, Bold, BoldItalic) equal the files at tag v2.304; SIL Open Font License 1.1; "Copyright 2020 The
  JetBrains Mono Project Authors"; no Reserved Font Name.
- Both are bundled unmodified and not subset, with their licence files next to them in the build output.
  licensecheck re-checks the name tables on every run.
The UI set registers at start-up from `<resources>/fonts/inter` (B1); the editor set registers on the first
`ensureEditorFonts()` (B19). A missing UI font is a warning, not a failure. Registration is all or nothing: a
failure removes the set's fonts registered so far, so retried editor opens never pile up partial registrations.

**Decision 9a (amendment 2026-11-07). More editor font families.** d540658 bundled eight more editor families
for the editor-font setting, registered with the editor set (`appearance_manager.cpp`, B19). All are SIL Open
Font License 1.1 (already on the allowlist, so no new licence), unmodified and not subset, pinned in
`resources/assets.lock.json` (release archive URL and SHA-256, every file's SHA-256), with the licence file next
to them in the build output, a NOTICE entry and a REUSE.toml override table. Record, re-checked 2026-11-07 by
licensecheck (pinned hashes, OFL 1.1 licence text, name tables naming the OFL and the holder):
- Fira Code 6.2: "Copyright (c) 2014-2021 The Fira Code Project Authors"; no Reserved Font Name.
- Cascadia Code and Cascadia Mono 2111.01: "Copyright (c) 2021 Microsoft Corporation"; Reserved Font Name
  "Cascadia Code". Its name tables' nameID 13 opens with Microsoft's generic "Microsoft supplied font ... Any
  other use is prohibited" paragraph before the OFL-based terms; nameID 14 and the repository LICENSE are the
  OFL (question 7, answered 2026-11-07: kept, see there).
- Source Code Pro 2.042R-u/1.062R-i: "Copyright 2023 Adobe"; Reserved Font Name "Source".
- IBM Plex Mono 1.1.0: "Copyright 2017 IBM Corp."; Reserved Font Name "Plex".
- Inconsolata 3.000: "Copyright 2006 The Inconsolata Project Authors"; no Reserved Font Name.
- Intel One Mono 1.4.0: "Copyright 2023-2024 The Intel One Mono Project Authors"; Reserved Font Name "Intel".
- Commit Mono 1.143: "Copyright 2023 Commit Mono authors" (name table; the archive's OFL.txt has no copyright
  line); no Reserved Font Name.
- Fantasque Sans Mono 1.8.0: "Copyright (c) 2013-2017 Jany Belluz"; no Reserved Font Name.
The first manifest recorded no Reserved Font Names for Cascadia Code, Source Code Pro, IBM Plex Mono and Intel One
Mono although their licence files declare them; the manifest now records them, and licensecheck fails when an OFL
licence file's preamble declares a name the manifest does not record, so the modified-font rule (OFL condition 3)
always sees every name. Unmodified bundling is allowed with Reserved Font Names.

**Decision 10. The asset manifest and gate.** `resources/assets.lock.json` pins each asset (https URL, archive
SHA-256, a recorded cross-check, SPDX licence, copyright lines, Reserved Font Names, files with SHA-256, extra
files at the pinned tag). `tools/asset_fetch.py` fetches (https only, redirects included; every hash pinned; the
archive hash checked before it is opened; listed members only; basenames that are not Windows device names;
destinations below `resources/`; size limits while streaming; atomic writes), verifies and pins.
`tools/licensecheck.py` gates the committed files: schema, allowlisted licence, a pinned archive, listed and hashed
files, no unlisted files under an asset directory, licence-file markers, the OFL name tables (read once, bounded),
the ADR, and SBOM packages with SHA-256 checksums. Both changes had an independent /code-review (PLAN 6.7); its
ten findings are fixed and tested. REUSE.toml annotates the asset directories with override rules and
`tests/ui/goldens/**` with "Apache-2.0 AND ISC AND MIT" and all three copyright lines (they are our renderings
showing Lucide icons; question 2). REUSE applies only the last table that matches a file, so that table lists
every holder itself rather than adding to the generic goldens rule. Network failures inside a reply
(`http.client` errors such as IncompleteRead) and a GitHub API reply that is not a release with a list of asset
objects are fetch errors (exit status 1), never tracebacks. `.gitattributes` marks
`resources/fonts/**` and `resources/icons/lucide/**` `-text`, so committed bytes equal the pins.

**Decision 11. Screenshot goldens.**
- The SamplePanel (480 x 320 logical) is rendered in-process with `QWidget::render` into a QImage at an explicit
  dpr, in a dpr-1, 96-DPI process (the test fails otherwise), on the offscreen platform, where the panel is shown
  and activated (no desktop window exists). No widget holds focus, so the tree's selection is drawn inactive.
- 13 goldens per OS under `tests/ui/goldens/{windows,linux}`: Glacier Dark and Light at 100, 125, 150 and 175%,
  Classic and High Contrast at 100%, Dark compact at 100%, and Dark at zoom 125% at 100 and 150%.
- The comparator: equal sizes, then every channel within 2, else at most 16 pixels over 2 and none of them over
  32 in any channel, so antialiasing noise passes but a small shape drawn missing or in the wrong colour does not
  (`GoldenComparator.BoundsOutliersInCountAndSize`). Text uses
  PreferNoHinting and grayscale antialiasing (test-only settings), so goldens test style, layout and colour
  rather than ClearType; the Windows font-smoothing settings are logged.
- Structural samples check theme colours at interior points independent of text (window band, island gap and
  corner, island, default button, focus ring, selected row, check box, radio dot, the disabled icon's tint).
- `ui_hidpi_tests` validates the explicit-dpr path against Qt's backing store under `QT_SCALE_FACTOR=1.5`
  (`QScreen::grabWindow`). The offscreen plugin returns only the top-left window-size device pixels there
  (480 x 320 of 720 x 480), so that region is compared; it covers the buttons, fields, check boxes, tab bar and a
  toolbar icon. Device-pixel snapping (decision 1) is what makes the two paths agree. When `grabWindow` returns
  nothing the tests fail rather than compare `QWidget::grab()`, which is the goldens' own render path and so no
  independent oracle.
- Update mode (`FROST_UPDATE_GOLDENS=1`) writes only failing goldens and always fails. Goldens are generated
  from Debug and pass unchanged in Release and ASan.
- In TSan builds the Qt widget tests carry three built-in suppressions, `called_from_lib` for libQt6Core,
  libQt6Gui and libQt6Widgets (`tests/ui/support/tsan_options.cpp`, through `__tsan_default_suppressions`). Qt's
  raster engine and QImage scaling use Qt's thread pool, synchronised by futex-based locks inside the
  uninstrumented libQt6Core, so TSan reported Qt's own malloc/free pairs as races (exit status 66; 7 to 34 reports
  per executable) although every assertion passed. A suppression drops only interceptor accesses made from the
  named library: libc, libstdc++, glib, fontconfig, freetype, ICU, D-Bus, the platform plugins and all of
  FrostIDE's instrumented code stay checked. (The first version used `ignore_noninstrumented_modules=1`, which
  ignores every uninstrumented module; the three named libraries are enough, so the blind spot is now named.) It
  still hides a real race whose other side is an interceptor call inside those libraries, such as a QByteArray
  or QImage copy of FrostIDE-owned memory, so these executables are no race oracle for code that shares data
  with Qt's threads: a later test of FrostIDE threads talking to Qt objects goes in its own executable without
  the suppressions, and the suppressions await the user's approval (question 5).
- The Windows and Linux goldens came out pixel-identical (both use the offscreen platform, the bundled fonts and
  Qt's own rasteriser). They stay separate files so a platform difference can appear later without weakening
  the comparator.

**Decision 12. Compact mode and zoom.** Compact changes metrics only (row heights, paddings, island arc and gap
8/3, layout margin and spacing, the toolbar icon size), never fonts; compact values inherit through metric
fallbacks. Zoom (50 to 300%) multiplies every logical metric, the UI font (Font.uiSize x zoom px, point size =
px x 72 / logical DPI) and icon sizes; Zoom In/Out step through 80, 90, 100, 110, 125, 150, 175 and 200% (from an
off-step value to the next or previous step). Zoom is applied at run time by a new style plus setPalette and
setFont, and is independent of `QT_SCALE_FACTOR`. The settings keys live in `AppearanceSettings`; the adapter
from m1/settings' typed registry lives in `src/ui`, because `src/settings` cannot see `frost::ui_model`.

**Decision 13. Animations off** (`SH_Widget_Animation_Duration` 0) until M11's reduced-motion work.

## Measurements
REF-W1 (AMD Ryzen 9 5900X, 32 GB, Windows 11 26300), Release, clang-cl, in-process probes
(`ui_appearance_perf_tests`, 50 warm iterations after one cold run, three runs):

| Probe | cold | p50 | p99 |
|---|---|---|---|
| theme blob decode (`loadThemeFile`) | 0.13 ms | 0.05 ms | 0.06-0.09 ms |
| FrostStyle construction + setStyle + setPalette, no widgets | 0.05 ms | 0.007 ms | 0.02 ms |
| IconAtlasSet open, one scale (100%) | 0.21 ms | 0.11-0.12 ms | 0.12-0.37 ms |
| UI font registration (4 Inter TTFs) | 1.42 ms | 0.53-0.57 ms | 0.83-1.30 ms |
| `install()` end to end | 1.17 ms | 0.69-0.73 ms | 0.78-1.06 ms |

`install()` adds about 1-2 ms to start-up. B1 is 150 ms and ADR 0007 measured frost.exe's warm first frame at
p50 264.9 ms already, so the appearance is under 1% of today's start-up and not what B1 has to win back. B1
itself is measured on screen by m1/automation-bench once m1/shell-frame wires `install()` into `main()`.

Atlas sizes (47 icons, 16 and 20 px designs, one page each): 100% 43,048 bytes; 125% 101,824; 150% 119,764;
175% 138,616; 200% 155,812; 250% 298,048; 300% 353,216. A full bake of all seven scales takes 0.26-0.39 s wall
(Release baker, five runs, process start included). Rendered island separation (screenshot tests, 100%): Dark
1.22:1, Light 1.26:1, Classic 1.25:1, High Contrast 1.23:1.

## Consequences
- Start-up reads four small files (theme blob, one atlas, four fonts) and parses no JSON or SVG.
- A registry change changes `kRegistryHash`, which rebuilds every blob and invalidates old ones by design.
- Goldens must be regenerated (and reviewed) whenever FrostStyle's drawing changes; update mode cannot green a run.
- Follow-ups:
  - TODO(M1) m1/shell-frame: call `install()` from `main()` (exit 4 on failure, like a missing platform plugin),
    surface InstallReport warnings and `appearanceApplyFailed`, copy `resources/` in `tests/app`, measure B1 on
    screen, build the shell on IslandHost and IslandFrame, reuse `frost_ui_test_support` for full-shell goldens.
  - TODO(M1) m1/settings: the IAppearanceSettings adapter in `src/ui`, with live reload on the GUI thread.
  - TODO(M1) m1/editor-view: editor colour keys and `ensureEditorFonts()`.
  - TODO(M1) after m1/piece-tree: libFuzzer adapters for the theme blob and atlas harnesses.
  - TODO(M15) m15/large-file: a mapping API for atlases (question 4).
  - M11: theme import (parentTheme, .icls, lenient mode in-product), reduced motion, accessibility extras.
  - Our own icon set replaces Lucide (D13).

## Alternatives considered
- **Qt style sheets.** Per-widget CSS matching, partial coverage of controls, and they disable the style's own
  drawing for styled widgets; rejected (decision 1).
- **Runtime QtSvg.** Rendering SVG at start-up and on every scale change costs B1 and B5 and adds a runtime
  module; rejected for build-time atlases.
- **PNG atlases.** PNG decode at start-up costs more than reading raw Alpha8 pages and needs the image codec path.
- **Per-theme ARGB atlases.** Four times the size and a re-bake per theme; Alpha8 masks tinted at run time serve
  every theme and the role overrides.
- **A Python theme compiler.** Not reusable in-product for M11's importer and not fuzzable with libFuzzer.
- **yyjson now.** Duplicates m1/settings' dependency decision and conflicts on vcpkg.json.
- **The Inter variable font.** Variable-font rendering differs more across rasterisers and is larger; four static
  weights cover the UI.
- **Font subsetting.** Changes the font (an OFL "Modified Version" question) and saves little; bundled unmodified.
- **QFile::map for atlases.** Bypasses IFileSystem (no fault injection, no long-path handling); the atlases are
  small (question 4).

## Questions for the user
1. Glacier Classic is held to the 1.20:1 island rule (the stricter reading) and passes at 1.25:1. Should a flat
   theme be exempt?
2. Goldens that show rendered Lucide icons are annotated "Apache-2.0 AND ISC AND MIT" with the FrostIDE, Lucide
   and Feather copyright lines. Acceptable, or should goldens use only FrostIDE-authored icons (which needs a
   minimal own set first)?
3. Unchecked check box borders are held to WCAG 1.4.11's 3:1 in every theme (NonText), stricter than "borders only
   in High Contrast"; other borders keep the original policy. Confirm.
4. Atlases are read, not mapped (decision 7). Confirm, or ask for QFile::map now.
5. The TSan runs of the Qt widget tests suppress interceptor reports from libQt6Core, libQt6Gui and
   libQt6Widgets (`called_from_lib`, decision 11), so they see less than the default. Approve, or should TSan
   skip these executables until an instrumented Qt build exists?
6. The golden comparator lets up to 16 pixels differ by up to 32 per channel (decision 11). Confirm, or tighten
   (for example to 8 pixels) once more platforms have run the goldens?
7. Cascadia Code's name tables (decision 9a) carry Microsoft's generic "Microsoft supplied font" paragraph next
   to the OFL terms, while its repository LICENSE and nameID 14 are the OFL 1.1. Keep bundling it on the
   strength of the OFL licence file (the usual reading for this upstream release), or drop it?
   **Answered 2026-11-07.** The user asked for a re-fetch from the GitHub release, so the bundle was checked
   against upstream again:
   - CascadiaCode-2111.01.zip, downloaded from the v2111.01 release of microsoft/cascadia-code (published
     2021-12-14 by a Microsoft maintainer), has the pinned sha256 51fd6817...e712. GitHub publishes no digest for
     this asset.
   - All 8 bundled TTFs are byte-identical to the archive members.
   - The bundled LICENSE is byte-identical to `LICENSE` at tag v2111.01 (sha256 51882cd3...8238): OFL-1.1, with
     Reserved Font Name "Cascadia Code".
   - The "Microsoft supplied font" paragraph in nameID 13 therefore comes with the official upstream release
     itself. The latest release, v2407.24, still carries it, so moving to a newer pin would not remove it.
     nameID 14 points to the OFL.

   Decision: keep Cascadia Code bundled under OFL-1.1, the licence of the upstream repository and release, and
   leave the files unmodified, because changing name tables would make an OFL Modified Version. Counsel's review
   before any public release (D28) should include this paragraph.
