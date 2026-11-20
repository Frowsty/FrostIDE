<!-- SPDX-License-Identifier: Apache-2.0 -->

# Structural pattern contract (REF-15)

Structural Search and Structural Replace use the selected document's actual pinned tree-sitter
grammar. They match syntax nodes, fields, anonymous tokens and their relationships. They do not
resolve C++/C#/Lua bindings, infer types, inspect runtime values or promise compilation correctness.
Unsupported parsers and predicates are errors rather than a fallback to regex/text search.

One S-expression pattern must capture exactly one target node as `@match`. Additional captures
have ASCII identifier names and bind exactly one node each. Alternatives inside that one pattern
are supported by the grammar query engine; unions of top-level patterns, optional/repeated captures
and missing captures refuse. All source must parse without ERROR/MISSING nodes. Strings/comments
are opaque unless the pattern explicitly selects their grammar nodes.

For C++ call expressions:

```scheme
((call_expression
    function: (identifier) @callee
    arguments: (argument_list) @args) @match
  (#eq? @callee "old_function"))
```

Replacement `new_function${args}` copies the exact argument-list bytes, including trivia. A
replacement `${match}` is a no-op. `${capture}` refers to one bound capture; `$$` inserts a literal
dollar. Other dollar forms, missing names and unclosed references refuse before matching. Literal
`#eq?`, `#not-eq?`, `#any-of?` and `#not-any-of?` predicates are supported. Capture-to-capture equality,
regular expressions, type/binding predicates, directives and unknown predicates are unsupported.

An identifier example for C++, C# and Lua is:

```scheme
((identifier) @match (#eq? @match "old"))
```

This selects identifier nodes only. It is **not a semantic Rename**: unrelated same-spelled
identifiers can match. Use the ordinary Rename action when binding identity is required. Node names
and field names belong to the selected grammar; a C++ query is not silently interpreted as Lua.

Search can report nested matches. Replace refuses every overlapping/nested proposal rather than
choosing an arbitrary subset. The complete proposed source is reparsed with the same grammar and
must remain syntactically valid. That check is not a compiler, type checker or semantic oracle.

Scopes are the current document, open same-language workspace documents, or the navigation index's
current same-language project-file inventory plus open overlays. The last scope is labelled Indexed
project files; it does not assert that an unfinished index has found the entire filesystem. Captured
input bytes, open-document identities/versions, trust for replacement and physical workspace
containment are checked. Disk encodings round-trip through the existing text codec. Any failed file,
cancelled query, budget overflow, unavailable grammar or malformed output refuses the complete
scope; partial search/replacement subsets are never passed to preview as complete.

The library bounds each source to 4 MiB, query/template to 16 KiB, captures to 32, captures/output to
8 MiB and matches to 2000. Fresh parsing and querying each have a two-second deadline. Product
scopes additionally cap 128 files, 16 MiB input, 16 MiB retained captured bytes, 2000 total matches
and ten seconds between files (one bounded file operation may finish after that checkpoint).
Cancellation is cooperative. These are enforced ceilings, not measured performance claims.

Search results are labelled captured syntax matches. Double-click navigates only if current source
still matches the captured parse. Replacement uses the existing complete before/after preview and
one REF-1 transaction with global Undo/Redo; it writes nothing before Apply. Changed inputs or
captured context refuse before mutation, and committed-but-unsettled operations retain their REF-1
recovery token instead of being retried as new replacements. No new dependency is introduced.

Regression sources exercise actual grammar queries, comment/string exclusion, exact trivia
capture, invalid syntax/predicates/overlap/cancellation, the normal preview action and global
Undo/Redo. Builds, tests and feature execution remain deferred under the implementation-first
instruction; this contract is not runtime acceptance evidence.
