<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0021: The settings registry, JSON scope files on yyjson, and EditorConfig

- Status: accepted for `m1/settings` (feature-first phase, ADR 0031). No budget, test or warning is weakened.
- Date: 2026-11-01
- Plan reference: PLAN.md section 3.6 (settings model), 3.5 (trust model: `executes` keys), 3.2 (`src/settings`,
  layer rank 5), section 2 dependency list (yyjson); feature rows I1 and 59 (EditorConfig); ADRs 0014 (settings
  never in SQLite), 0015 decision 12 and 0016 decision 11 (the appearance and keymap keys this registry owns),
  0020 (watch fan-out), 0032 (W1: settings must never block opening a project). This is the dependency ADR for
  yyjson (CLAUDE.md "Licences").

## Context
M1 needs one typed source for every preference: the editor, SaveManager (autosave, local-history retention),
the theme and keymap adapters, and later the M15 Settings dialog, which is generated from the registry. Scope
files are hand-edited and committed (`.frost/settings.json`), so edits FrostIDE makes must leave everything else in
the file byte-for-byte as it was. Project files can name programs; PLAN 3.5 requires those values to wait for
trust. Nothing in the repository parses JSON outside Qt (the theme compiler uses QJsonDocument, ADR 0015).

## Decision
1. **Dependency: yyjson 0.13.0** from the vcpkg baseline (default features: reader, writer, non-standard,
   fast-fp-conv), licence MIT (on the `tools/licensecheck.py` allowlist). Found in the root CMakeLists beside
   SQLite (`if(NOT FROST_FUZZ)`: fuzz trees install no ports, so `src/settings` is left out of them like storage);
   `frost_settings` links it PRIVATE, so no public header includes `yyjson.h`. Comments and trailing commas are
   accepted on read (users edit these files); nothing else non-standard is enabled.
2. **Module.** `src/settings` (frost::settings, layer `settings`, rank 5), Qt-free, depending on frost::io. It does
   not depend on frost::input or frost::ui: the keymap and appearance adapters live above it (as their ADRs say),
   and the Settings-dialog stand-in actions are described as data (`settingsActions()`) for the shell to register.
3. **Registry.** Keys are flat dotted names. Each `KeySpec` has a schema (bool, integer, number, string, enum,
   string list, record list, with ranges, options, fields and a widget hint), a default, allowed scopes, an
   `executes` flag, an `advanced` flag, and a title, description and category path for the dialog. Typed handles
   (`keys.hpp`) give readers compile-time types. Defaults follow Rider/IntelliJ: JetBrains Mono 13 at 1.2, 4-space
   indents, soft wrap off, line numbers on, the IntelliJ keymap, `glacier-dark`, autosave on frame deactivation and
   after 15 s idle, local history for 5 days and up to 1 GiB.
4. **Scopes and precedence** (low to high): default, application (`%APPDATA%\FrostIDE\settings.json`,
   `$XDG_CONFIG_HOME/frostide/settings.json`), project-shared (`.frost/settings.json`), project-personal
   (`.frost/workspace.json`, gitignored through `.frost/.gitignore`), file level (EditorConfig, per file). A value in
   a scope the key does not allow, or failing its schema, is ignored with a diagnostic and the next lower scope
   applies. Appearance, keymap and tool-path keys are application-only.
5. **Trust rule.** Values of `executes` keys in either project scope count only while the project is trusted; the
   snapshot lists the ignored keys for the trust banner. Tool paths and plugin lists are application-only on top.
   The trust state comes from the project layer's TrustStore (w1/project-sln) through `setProjectTrusted`.
6. **Byte-exact files.** `SettingsDocument` keeps a file's bytes; yyjson validates and decodes, and a small scanner
   records each top-level member's key and value spans in the accepted text. Edits replace a value, append a member
   in the file's indentation and line-break style, remove a member with its separator, or rename a key; the result
   is reparsed every time. Unknown members, comments and formatting survive untouched.
7. **Migrations** are pure functions over a `SettingsDocument` (0 -> 1 only stamps `schemaVersion`), with golden
   tests. The store migrates in memory and writes the migrated text only when it next saves that file; a file newer
   than the build is read as far as understood and never written.
8. **Never blocking.** A missing file is an empty scope; a broken one is reported and, on reload, keeps its last
   good values. Opening a project creates nothing; `.frost/` and its `.gitignore` appear on FrostIDE's first write.
9. **Live reload** takes `FileChangeBatch`es from any watch (the project root's recursive watch, or non-recursive
   watches of `watchDirectories()`), following ADR 0020's single-path rules; `follow()` pumps one watch.
10. **Validation on save** (`checkSavedFile`) reads exactly what the editor saved, never writes, reloads the scope
    and returns diagnostics plus notification texts. Until M15 the Settings dialog is three actions that create the
    scope file if needed and open it in the editor.
11. **EditorConfig** is implemented here from the public format description (no implementation consulted):
    indent_style, indent_size, tab_width, end_of_line, charset, trim_trailing_whitespace, insert_final_newline and
    max_line_length map onto registry keys; charset and end_of_line map onto `files.encoding` (frost::text names:
    "UTF-8", "UTF-16LE", "ISO-8859-1", ...), `files.utf8Bom` and `files.lineSeparator`, which m1/encodings' load and
    save code reads through the composition root.

## Consequences
- M15 can generate every Settings page from the registry; importers (M11) write through the same documents.
- The store is internally locked and publishes immutable snapshots; listeners run on the changing thread.
- Deferred to hardening (feature-first): fuzzing the document parser and scanner, sanitizer runs, a stress test of
  concurrent reloads (two reloads of one file can publish out of order), case-insensitive path matching on
  Windows for scope files, a bound on EditorConfig glob backtracking, and comment-preserving removal of a file's
  last member.

## Alternatives considered
- **QJsonDocument**: Qt is banned below `src/ui`.
- **nlohmann/json, simdjson, Boost.JSON**: nlohmann is slower and larger to compile; simdjson has no writer;
  Boost.JSON drags in Boost. PLAN section 2 already lists yyjson for the protocol code.
- **Re-serialising the whole document on every write**: loses comments, member order and formatting, so committed
  files would churn and unknown keys would not survive byte-for-byte.
- **Nested JSON objects instead of dotted keys**: harder to edit byte-exactly and to diff; VS Code's flat style is
  familiar to users.
- **Writing migrated files on load**: would dirty committed files just by opening a project.
