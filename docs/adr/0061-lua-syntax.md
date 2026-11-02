<!-- SPDX-License-Identifier: Apache-2.0 -->

# ADR 0061: Lua syntax and editor indentation

- Status: accepted
- Date: 2026-11-02
- Plan reference: Appendix B LUA-02; ADR 0023, ADR 0025, ADR 0054.

## Decision

Vendor the MIT-licensed tree-sitter-grammars/tree-sitter-lua 0.5.0 at commit
`10fe0054734eec83049514ea2e718b2a56acd0c9` with unmodified generated parser, scanner and headers.
The vendor manifest pins archive and file hashes and the licence; FrostIDE authors its own highlight,
outline and fold queries against the upstream grammar. The existing syntax session handles `.lua`
case-insensitively and incrementally. Its fold-range API exposes nested, multiline Lua ranges;
there is not yet an interactive fold UI. The editor model uses Lua block keywords for Enter, ignoring
keywords in strings and comments. A current syntax tree supplies the lexical context at the
start of the edited line, avoiding scans from the start of a large file. If no current tree exists,
the fallback scans at most 64 KiB and keeps the existing indentation when context is unknown; a
single long line has the same conservative bound. The language table already supplies Lua comment
tokens.

## Consequences

Parsing, Unicode and edits use the same tree-sitter runtime and safety limits as other languages.
The grammar is MIT and introduces no new runtime dependency. Folding controls, formatting, snippets,
debugging and annotation/generic support are separate capabilities and are not claimed by this slice.
