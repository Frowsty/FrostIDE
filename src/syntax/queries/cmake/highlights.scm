; SPDX-License-Identifier: Apache-2.0
;
; highlights.scm (CMake): frost::syntax's highlight query for tree-sitter-cmake 0.7.6 (ADR 0023,
; D8). Written for FrostIDE from the vendored grammar's node types. The first pattern for a node
; wins; ${...} references are variables even inside quoted arguments.

(line_comment) @comment
(bracket_comment) @comment

; Commands: the block keywords are named nodes in this grammar.
[
  (function) (endfunction) (macro) (endmacro) (if) (elseif) (else) (endif) (foreach) (endforeach)
  (while) (endwhile) (block) (endblock)
] @keyword
(normal_command
  (identifier) @function.call)

; Arguments
(variable_ref) @variable
(variable) @variable
(escape_sequence) @string.escape
(quoted_argument) @string
(bracket_argument) @string
((unquoted_argument) @boolean
  (#any-of? @boolean "ON" "OFF" "TRUE" "FALSE" "YES" "NO" "Y" "N" "on" "off" "true" "false"))
((unquoted_argument) @keyword
  (#any-of? @keyword
    "PUBLIC" "PRIVATE" "INTERFACE" "STATIC" "SHARED" "MODULE" "OBJECT" "IMPORTED" "ALIAS"
    "REQUIRED" "COMPONENTS" "CONFIG" "QUIET" "CACHE" "PARENT_SCOPE" "FORCE" "NOT" "AND" "OR"
    "DEFINED" "EQUAL" "LESS" "GREATER" "STREQUAL" "MATCHES" "VERSION_LESS" "VERSION_GREATER"
    "EXISTS" "TARGET" "IN" "LISTS" "ITEMS" "RANGE" "COMMAND" "DEPENDS" "OUTPUT" "SOURCES"
    "PROPERTIES"))
["ENV" "CACHE"] @keyword

["(" ")"] @punctuation.bracket
