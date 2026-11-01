; SPDX-License-Identifier: Apache-2.0
;
; outline.scm (CMake): frost::syntax's outline query for tree-sitter-cmake 0.7.6 (ADR 0023, D8).
; function() and macro() definitions are Command items, and add_library, add_executable and
; add_custom_target calls are Target items, each named by its first argument. CMake command names
; are case-insensitive; the common spellings are listed.

(function_def
  (function_command
    (argument_list
      .
      (argument) @outline.name))) @outline.command
(macro_def
  (macro_command
    (argument_list
      .
      (argument) @outline.name))) @outline.command
((normal_command
  (identifier) @_command
  (argument_list
    .
    (argument) @outline.name)) @outline.target
  (#any-of? @_command
    "add_library" "add_executable" "add_custom_target" "ADD_LIBRARY" "ADD_EXECUTABLE"
    "ADD_CUSTOM_TARGET"))
