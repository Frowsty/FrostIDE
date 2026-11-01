; SPDX-License-Identifier: Apache-2.0
;
; outline.scm (C): frost::syntax's outline query for tree-sitter-c 0.24.2 (ADR 0023, D8). Written
; for FrostIDE from the vendored grammar's node types. Each pattern captures a declaration as
; @outline.<kind> and its name as @outline.name; items nest by containment, and a node matched by
; several patterns keeps the first one.

(function_definition
  declarator: (function_declarator
    declarator: (identifier) @outline.name)) @outline.function
(function_definition
  declarator: (pointer_declarator
    declarator: (function_declarator
      declarator: (identifier) @outline.name))) @outline.function
(function_definition
  declarator: (pointer_declarator
    declarator: (pointer_declarator
      declarator: (function_declarator
        declarator: (identifier) @outline.name)))) @outline.function

(struct_specifier
  name: (type_identifier) @outline.name
  body: (field_declaration_list)) @outline.struct
(union_specifier
  name: (type_identifier) @outline.name
  body: (field_declaration_list)) @outline.union
(enum_specifier
  name: (type_identifier) @outline.name
  body: (enumerator_list)) @outline.enum
(enumerator
  name: (identifier) @outline.name) @outline.enum_member
(field_declaration
  declarator: (field_identifier) @outline.name) @outline.field
(field_declaration
  declarator: (pointer_declarator
    declarator: (field_identifier) @outline.name)) @outline.field

(type_definition
  declarator: (type_identifier) @outline.name) @outline.type_alias
(preproc_def
  name: (identifier) @outline.name) @outline.macro
(preproc_function_def
  name: (identifier) @outline.name) @outline.macro
