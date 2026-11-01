<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0023: frost::syntax on vendored tree-sitter

- Status: accepted
- Date: 2026-11-01
- Plan reference: PLAN.md section 2 ("Syntax" row), section 3 (edit path step 2), M1 slice `m1/syntax`; ADR 0013
  (TextChange), ADR 0031 (feature-first phase)

## Context
M1 needs highlighting and a member outline (next/previous member) for C, C++, C#, CMake, XML, JSON and Markdown,
parsed incrementally from piece-tree snapshots within 2 ms per edit, off above 50 MB. PLAN.md fixes tree-sitter
0.27.1 (ABI 15) with pinned grammars and our own queries, vendored rather than taken from vcpkg.

## Decision
1. **Pins.** `third_party/vendor.lock.json` pins every component by upstream commit, codeload archive SHA-256 and
   per-file SHA-256; all are MIT (on the allowlist):

   | Component | Version | Commit | Copyright |
   |---|---|---|---|
   | tree-sitter (runtime, `lib/`) | 0.27.1 | 10eb5ac1 | 2018 Max Brunsfeld |
   | tree-sitter-c | 0.24.2 | b780e47f | 2014 Max Brunsfeld |
   | tree-sitter-cpp | 0.23.4 | f41e1a04 | 2014 Max Brunsfeld |
   | tree-sitter-c-sharp | 0.23.5 | cac6d5fb | 2014-2023 Max Brunsfeld, Damien Guard, Amaan Qureshi, and contributors |
   | tree-sitter-cmake (uyha) | 0.7.6 | a3ec2b21 | 2025 Uy Ha |
   | tree-sitter-xml (xml grammar) | 0.7.0 | 4b64dd3a | 2023 ObserverOfTime |
   | tree-sitter-json | 0.24.8 | ee35a6eb | 2014 Max Brunsfeld |
   | tree-sitter-markdown (block grammar) | 0.5.4 | 41ff24a6 | 2021 Matthias Deiml |

   `tools/asset_fetch.py --manifest third_party/vendor.lock.json` verifies the files offline (a vendor lock is the
   ADR 0015 manifest schema with a `components` list, `.tar.gz` archives, nested paths, `commit` and `purl`);
   `tools/licensecheck.py` runs the asset gate over it (licence, pins, no unlisted file, licence text, SBOM
   package with purl). REUSE.toml carries each component's copyright; NOTICE lists them.
2. **Raw vendoring.** The generated `parser.c` files are committed as upstream bytes: 54.6 MB in the work tree
   (the C# and C++ grammars 30 and 17 MB), 2.8 MB compressed with zlib as git stores objects. `.gitattributes`
   marks `third_party/tree-sitter/**` and `third_party/grammars/**` `-text`; clang-format and frost-lint's code
   rules skip them (frost-lint `VENDORED_GLOBS`: REUSE coverage only).
3. **Build.** `tree_sitter_runtime` is `lib/src/lib.c` in one C11 translation unit (`src/syntax/src/c/
   frost_tree_sitter.c`) with upstream diagnostics suppressed around the include; our C compiles with `-Werror`.
   Each grammar is its own static library with warnings off, `TREE_SITTER_HIDE_SYMBOLS` (no dllexport) and
   `TREE_SITTER_REUSE_ALLOCATOR`. WebAssembly is off.
4. **Unicode (D2).** `lib/src/unicode.h` needs ICU headers, whose licence is not on the allowlist, so they are not
   vendored: `frost_ts_unicode.h` defines that header's guard and supplies `ts_decode_utf8` (Unicode Table 3-7,
   maximal-subpart errors) and the UTF-16 decoders, written for FrostIDE.
5. **Rows (D3).** The TSInput reads snapshot chunks and presents every lone CR as LF (a one-byte substitution
   with a look-ahead at chunk ends; CRLF passes through), so tree-sitter's rows and byte columns equal the
   editor's LSP lines and a D8 `TextChange` maps field by field to a `TSInputEdit`.
6. **Budget (D4/D5).** Each parse first runs synchronously with a 2 ms deadline (`ts_parser_parse_with_options`'
   progress callback, checked every 100 operations). On overrun the same halted parse resumes on a sched worker
   (`Lane::UserVisible`); meanwhile the last tree, edited into the current coordinates, keeps serving highlights
   (`SyntaxState::Stale`). Edits during a worker parse queue and a catch-up parse follows. A worker parse that
   exceeds 30 s fails the session (`TimeLimit`) until the next `open`.
7. **Size.** Syntax is off above 50,000,000 bytes (and always above tree-sitter's uint32 offsets); a session that
   shrank to 49,000,000 bytes or less reopens.
8. **Queries.** `src/syntax/queries/<lang>/{highlights,outline}.scm` are ours, written from node names in the
   vendored grammars' symbol tables; no third-party query file was consulted or copied. Captures map to the
   closed `HighlightKind` and `OutlineKind` vocabularies; unknown captures, `#match?` and other unknown predicates
   fail compilation (`ErrorCode::Corrupt`). The innermost capture wins and, for one node, the first pattern.

## Measurements
None in the feature-first phase (ADR 0031): Debug test runs only, no perf gates. The whole `syntax_tests` suite
takes about 4 s in Debug on the development machine.

## Consequences
- The editor model gets `SyntaxSession` (state, stale-tree highlights, outline, `enclosingRanges` for extend
  selection); M4 adds language services on the same trees.
- Raw generated sources make clones about 2.8 MB larger and grammar updates large diffs; every update repins the
  lock and repeats the licence check.
- TODO(M1): hardening per ADR 0031 (see the slice's deferred list): a counting allocator with a syntax memory cap,
  off-thread tree release, narrowing the suppressed runtime warnings to the recorded set, compile-time and binary
  size measurements, `.tar.gz` fetch and pin in `asset_fetch.py`, fuzz, stress, TSan and perf suites.
- TODO(M15): fold ranges and the Structure-view model (ADR 0029).

## Alternatives considered
- **vcpkg's tree-sitter port.** Its version and grammars are not ours to pin, and grammars are not ported.
- **Generating `parser.c` at build time.** Needs Node.js and the tree-sitter CLI in every build.
- **Fetching sources at configure time.** Builds would need the network; offline verification is a hard rule.
- **Compressed sources in git.** A custom unpack step that saves about 50 MB in the work tree only.
- **ICU for decoding.** Licence not on the allowlist, and large for two decoders.
- **TextMate grammars.** Not incremental, and no tree for the outline (PLAN.md section 2).
