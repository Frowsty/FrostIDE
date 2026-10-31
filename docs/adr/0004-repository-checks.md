# ADR 0004: Repository checks (frost-lint, frost-licensecheck, frost-featurecheck, corpus fetch)

- Status: accepted
- Date: 2026-10-31
- Plan reference: PLAN.md section 2 ("Licences", "GUI toolkit"), 2.1, 4 (budgets, corpora), 6.1 ("Feature matrix"), 6.3 ("Checks"), 6.6 ("Enforcement"), M0 deliverables and exit, Appendix A

## Context
M0 needs the repository checks that run on every change in both verify legs (PLAN.md 6.3): frost-lint v0
(SPDX headers on source files, REUSE coverage for everything else, the to-do, budget, narrow-Win32 and C++26
rules, no CI), frost-licensecheck (every vcpkg port's declared licence against the SPDX allowlist, Qt modules
keyed on LGPL/GPL status, an SBOM, and the Qt Charts and GPL-port negative tests the M0 exit names),
`matrix.json` generated from Appendix A with frost-featurecheck, `budgets.json` as the budget source of truth,
and the corpus manifest with its fetcher.

Constraints: no CI (D24); every check runs on Windows and in the WSL ext4 mirror, which has no `.git`; any
third-party package would need its own ADR and licence check, so the tools use only the Python standard
library (3.11+ for `tomllib`; Windows has 3.14 in E:\sdk\py, WSL has 3.12); the clean-room rule forbids reading
GPL tools (the `reuse` tool is GPL-3.0-or-later), so REUSE and gitignore glob matching are written from their
specifications.

## Decision
All four tools take `--repo <path>`, print `path:line: message` per violation and exit 1 on any violation
(2 on usage errors). They are Python scripts in `tools/` with unittest suites in `tests/tools` (registered with
CTest under the `unit` label, so both legs run them). verify.ps1 and verify.sh run frost_lint.py,
licensecheck.py and featurecheck.py after the build.

**File set.** `git ls-files --cached --others --exclude-standard` when `--repo` is the top of a git work tree;
otherwise a walk that applies every `.gitignore` (gitignore(5) semantics: anchoring, `**`, negation,
directory-only rules, pruned ignored directories). The WSL mirror therefore checks the same files as the
Windows checkout.

**frost-lint v0.**
- *Source files* are defined by an explicit table (`SOURCE_SYNTAX_BY_SUFFIX`/`_BY_NAME` in frost_lint.py):
  C/C++ (including module units and `.in` templates), C#, `.rc`, CMake, PowerShell, shell, Python, YAML, TOML,
  INI (`.editorconfig`), XML that we author (`.manifest`, `.natvis`, `.props`, `.targets`, `.csproj`, `.wxs`,
  `.xsd`, `.xml`) and batch files. Each needs an `SPDX-License-Identifier:` line in its header comment within the
  first 10 lines, plus a descriptive header comment (at least one non-SPDX header line of three or more words).
  A shebang, an XML declaration or `@echo off` may precede the header.
- *Every other file* needs a REUSE.toml annotation or a `<file>.license` sidecar. REUSE.toml globs follow REUSE
  3.x: `*` stays in one directory, `**` crosses directories, `**/` also matches at the root, `\*` is literal.
  Annotations must carry both SPDX keys. Unknown file types fail this rule until they are added to the table or
  to REUSE.toml, so nothing is silently skipped.
- *Byte-exact data* (`tests/**/fixtures/**`, `tests/**/goldens/**`, `tests/fuzz/corpus/**`, the `-text` paths
  in .gitattributes) only needs REUSE coverage, because it must stay byte-identical and may contain deliberate
  violations.
- A small lexer per comment family separates code, string literals and comments (C/C++/C# with raw, verbatim
  and char literals and digit separators; `#` languages with their quoting, Python triple quotes, PowerShell
  block comments and here-strings, CMake bracket arguments and comments; XML; batch). Rules then look only at
  comments (to-do, budget) or only at code (narrow Win32, C++26), which keeps false positives out of strings and
  prose.
- *To-do markers*: a TODO word at the start of a comment line, or followed by `:` or `(`, must read
  `TODO(Mn)` with n in 0..14 and not in `CLOSED_MILESTONES` (empty until the M0 sign-off). Prose that merely
  mentions the word (the "TODO window" feature) is not a marker.
- *Budget references* (`Budget: B5.`) must name IDs present in tests/perf/budgets.json.
- *Narrow Win32 APIs*: an explicit list of about 100 `...A` functions (files, processes, modules, registry,
  shell, messages, windows), matched as identifiers in C/C++ code.
- *Banned C++26 constructs* in C/C++ code: `#embed`, `contract_assert`, `pre(...)`/`post(...)` contract
  specifiers, reflection `^^` and `[: ... :]` splices, `template for` (clang 23 accepts it silently, so this is
  the only enforcement), `export module`, `module;`, module declarations and `import` declarations (including
  `import std`), `#include <execution>`, and the C++26 headers `<inplace_vector>`, `<hazard_pointer>`, `<rcu>`,
  `<linalg>`, `<debugging>`, `<text_encoding>`, `<contracts>`, `<meta>`, `<simd>`, `<hive>`. Contract
  specifiers are matched conservatively: after the closing parenthesis of a parameter list (through cv, ref and
  noexcept qualifiers) whose opening parenthesis follows a function name, `]` or `>`, or after a trailing return
  type; control statements (`if (x) post(y);`) and preprocessor lines never match. Exempt from this rule only:
  `src/compat/**` (the polyfill layer) and `tests/toolchain_probe/banned/**` (intentional compile-fail probes);
  every other rule still applies there.
- *No CI*: a `.github/workflows` directory fails (D24).

**frost-licensecheck v0.**
- *Allowlist*: the PLAN.md "Licences" row as simple expressions matched exactly (an ID, or `ID WITH exception`
  as one unit). Compound expressions are evaluated: `OR` needs one allowed alternative, `AND` needs all. So
  `MIT OR GPL-2.0-only` passes (the dependency is used under MIT) and `MIT AND Unicode-3.0` needs an ADR.
  LGPL-3.0-only is added only when evaluating Qt.
- *vcpkg ports*: every port in vcpkg's status database for each triplet under the installed root (default
  `build/vcpkg_installed`, the presets' VCPKG_INSTALLED_DIR) must have a `share/<port>/vcpkg.spdx.json` whose
  licence the allowlist satisfies. vcpkg 2026-09-26 writes the port's `license` field to `licenseConcluded` and
  leaves `licenseDeclared` as NOASSERTION, so the check reads `licenseDeclared` and falls back to
  `licenseConcluded`. A port without a licence, or with one the allowlist does not satisfy, "needs an ADR"; an
  accepted ADR is recorded in `ADR_EXCEPTIONS` with the exact expression (empty now), so a licence change
  fails again. A manifest with dependencies but no installed tree also fails, so the gate never passes vacuously.
- *Qt modules*: uses come from `find_package(Qt6 ... COMPONENTS ...)`, `find_package(Qt6<Module>)` and `Qt6::`
  targets in the repository's CMake files (comments stripped; lower-case tool targets such as `Qt6::qmake`
  ignored; `XPrivate` maps to `X`), and from the build trees: `Qt6*.dll` and plugin DLLs next to Windows
  binaries, `libQt6*.so*` and plugins on Linux, and the `DT_NEEDED` entries of Linux ELF executables and shared
  objects (read from the section headers, from the ELF specification). Licences come from the installed Qt's own
  SBOMs (`$FROST_QT_ROOT/sbom/*.spdx.json`, keyed by the `qt-module-`/`qt-plugin-` SPDX IDs), falling back to a
  table: the LGPL modules as declared in the Qt 6.12.0 SBOMs, and the GPL-3.0-only modules confirmed on their
  doc.qt.io pages on 2026-10-31 (Charts, Data Visualization, Graphs, Canvas Painter, Network Authorization,
  HTTP Server, Virtual Keyboard, Quick 3D, Quick 3D Physics, Quick Timeline, Lottie, CoAP, MQTT). A module with no
  LGPL-3.0-only option fails as GPL-only; an unknown module needs an ADR.
- *Module allowlist*: per platform and context. Runtime: Windows Core, Gui, Widgets (plus EntryPoint, Qt's
  static BSD-licensed WinMain shim) and the qwindows and qoffscreen platform plugins (offscreen because
  `frost --selftest` ships with it); Linux adds DBus, XcbQpa, WaylandClient and the qxcb, qwayland, qoffscreen
  and IBus input-context plugins. Svg is allowed only under an icon-baker path, Test only under `tests/`. Build
  tree uses are checked against the platform of the binary; CMake uses, which cannot see `if(WIN32)`, against the
  union of both platforms.
- *SBOM*: `--sbom <file>` writes SPDX 2.3 JSON: FrostIDE (Apache-2.0), every port (declared licence, purl) and
  every Qt component (declared expression, concluded LGPL-3.0-only), DEPENDS_ON or TEST_DEPENDENCY_OF
  relationships, and `hasExtractedLicensingInfos` for each LicenseRef. `SOURCE_DATE_EPOCH` fixes the timestamp.

**frost-featurecheck v0.** `generate` parses Appendix A (the milestone/tier table and the infrastructure list)
into `tests/features/matrix.json`: id (the row number, or `I<n>`), kind, feature, priority (P0/P1), owning
milestone, `completes` for `(+Mx)`, `p2_pulled_in` for `(P2 in)`, `new` for `*`, and the hand-maintained
`acceptance_test` and `status`, which regeneration preserves along with `inspection_counts.s9_baseline` (the slot
spike S9 fills). The section 6.1 inspection-count targets (M7, M9, M13) are parsed into
`inspection_counts.targets`. `check` fails on a tier count or totals line that disagrees with the listed rows,
duplicate rows, a P0, P1 or infrastructure row without an owning milestone, invalid `status`/`acceptance_test`
values (green needs a test), and a matrix that is out of date against PLAN.md.

**budgets.json** holds all 28 rows of the PLAN.md section 4 table. Each row repeats the table cells verbatim
(`plan_metric`, `plan_target`, `plan_corpus`, `plan_gate`), which a unit test compares with PLAN.md, and adds
structured targets (percentile, max, unit, optional variant, corpus, goal, rig, gate), preconditions, method and
gate milestones; shared definitions (rigs and scale factors, cold/warm, measurement, enforcement) sit in
`defaults`.

**Corpora.** `tests/corpus/manifest.json` pins R2a Notepad++ v8.9.8.1 (dd40fe45, GPL-3.0-or-later), R2b
microsoft/terminal v1.25.2733.0 (dc8ae365, MIT; v1.26 is a preview) and R3 llvm-project llvmorg-23.1.4
(7a941dfb, the commit the pinned binaries report; Apache-2.0 WITH LLVM-exception), resolved with
`git ls-remote` and the GitHub releases API on 2026-10-31, with URL, tag, tag object, licence, GitHub-reported
size, prerequisites and purpose. `tools/corpus_fetch.py` lists by default and fetches only with `--only <id>` or
`--all`: `git init`, a depth-1 fetch of the exact SHA (falling back to the tag), a detached checkout, and a
HEAD == pinned SHA check that rejects the checkout otherwise. Tests use a local bare repository.

## Measurements
REF-W1, Python 3.14 (E:\sdk\py), this repository at the commit introducing the tools (145 files), wall time of
one run each:
- frost_lint.py: 0.32 s (git listing plus lexing every file).
- featurecheck.py check: 0.13 s.
- licensecheck.py with the Windows Debug build tree and its vcpkg tree (3 ports, Qt Core/Gui/Widgets and two
  platform plugins): 0.30 s.
- The five unittest suites: 66 tests, about 12 s, most of it git in the corpus tests.

## Consequences
- Every new file type needs a decision: a row in the source table (and a header) or REUSE coverage.
- Narrow-API and C++26 rules are regex/lexer based and can miss constructs hidden by macros; the
  contracts heuristic accepts misses over false positives. TODO(M1): tree-sitter queries for C++, C# and CMake,
  and the function-comment rules for scripts and CMake functions (PLAN.md 6.6).
- The Qt allowlist is enforced from M0, so adding a module (for example a Wayland shell-integration plugin or
  the compose input context when packaging Linux in M5/M6) needs an ADR and a table change.
- The licence check reads only what vcpkg declares; a port that bundles code under other licences (utf8proc's
  Unicode tables) is caught only if the port declares it. TODO(M1): ScanCode licence-notice detection on diffs
  (needs its own ADR), the bundled-process class (appimageupdatetool) and the vsdbg/cpptools artifact scan.
- featurecheck does not yet gate milestone exits on acceptance tests or inspection counts. TODO(M1): acceptance
  tests at milestone exit and the IntelliJ settings-page checklist; TODO(M7): inspection-count gates.
- verify.ps1 does not yet pass `--sbom`; the SBOM is produced on demand until packaging needs it (M5).

## Alternatives considered
- The `reuse` tool for coverage: GPL-3.0-or-later and a third-party package (ADR, licence check, clean-room
  concerns); the coverage rule is about 60 lines of our own code.
- Tree-sitter now: needs the vendored grammars and Python bindings (a dependency) before the editor slice
  delivers them in M1; v0 uses a lexer and regular expressions.
- Matching whole licence expressions only: would force an ADR for harmless dual licences (`MIT OR Apache-2.0`)
  and still needs a parser to report what is wrong; leaves are matched exactly instead.
- Deriving Qt modules from binaries only: Linux binaries carry no copies next to them and CMake shows intent
  before a build exists; both sources are used.
- `git ls-files` only: the WSL mirror has no `.git`, so the walk fallback is required for the Linux leg.
