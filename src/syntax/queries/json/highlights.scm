; SPDX-License-Identifier: Apache-2.0
;
; highlights.scm (JSON): frost::syntax's highlight query for tree-sitter-json 0.24.8 (ADR 0023,
; D8). Written for FrostIDE from the vendored grammar's node types. The first pattern for a node
; wins, so object keys are properties rather than strings.

(comment) @comment
(pair
  key: (string) @property)
(string) @string
(escape_sequence) @string.escape
(number) @number
[(true) (false)] @boolean
(null) @constant
["{" "}" "[" "]"] @punctuation.bracket
["," ":"] @punctuation
