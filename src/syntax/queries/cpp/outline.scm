; SPDX-License-Identifier: Apache-2.0
;
; outline.scm (C++): frost::syntax's outline query for tree-sitter-cpp 0.23.4 (ADR 0023, D8).
; Written for FrostIDE from the vendored grammar's node types. Each pattern captures a declaration
; as @outline.<kind> and its name as @outline.name; items nest by containment (template_declaration
; is transparent: the function or class inside it is the item), and a node matched by several
; patterns keeps the first one, so members come before free functions.

(namespace_definition
  name: (_) @outline.name) @outline.namespace

(class_specifier
  name: (_) @outline.name
  body: (field_declaration_list)) @outline.class
(struct_specifier
  name: (_) @outline.name
  body: (field_declaration_list)) @outline.struct
(union_specifier
  name: (_) @outline.name
  body: (field_declaration_list)) @outline.union
(enum_specifier
  name: (_) @outline.name
  body: (enumerator_list)) @outline.enum
(enumerator
  name: (_) @outline.name) @outline.enum_member

; Members of a class body. A member function without a return type named by a plain identifier is a
; constructor (conversion operators are operator_name, not identifier).
(field_declaration_list
  (function_definition
    !type
    declarator: (function_declarator
      declarator: (identifier) @outline.name)) @outline.constructor)
(field_declaration_list
  (declaration
    !type
    declarator: (function_declarator
      declarator: (identifier) @outline.name)) @outline.constructor)
(field_declaration_list
  (function_definition
    declarator: (function_declarator
      declarator: (destructor_name) @outline.name)) @outline.destructor)
(field_declaration_list
  (declaration
    declarator: (function_declarator
      declarator: (destructor_name) @outline.name)) @outline.destructor)
(field_declaration_list
  (function_definition
    declarator: (function_declarator
      declarator: (_) @outline.name)) @outline.method)
(field_declaration_list
  (template_declaration
    (function_definition
      declarator: (function_declarator
        declarator: (_) @outline.name)) @outline.method))
(field_declaration_list
  (declaration
    declarator: (function_declarator
      declarator: (_) @outline.name)) @outline.method)
(field_declaration_list
  (field_declaration
    declarator: (function_declarator
      declarator: (_) @outline.name)) @outline.method)
(field_declaration
  declarator: (field_identifier) @outline.name) @outline.field
(field_declaration
  declarator: (pointer_declarator
    declarator: (field_identifier) @outline.name)) @outline.field
(field_declaration
  declarator: (reference_declarator
    (field_identifier) @outline.name)) @outline.field
(field_declaration
  declarator: (array_declarator
    declarator: (field_identifier) @outline.name)) @outline.field

; Free functions and out-of-class member definitions.
(function_definition
  declarator: (function_declarator
    declarator: (destructor_name) @outline.name)) @outline.destructor
(function_definition
  declarator: (function_declarator
    declarator: (qualified_identifier
      name: (destructor_name)) @outline.name)) @outline.destructor
(function_definition
  declarator: (function_declarator
    declarator: (_) @outline.name)) @outline.function
(function_definition
  declarator: (pointer_declarator
    declarator: (function_declarator
      declarator: (_) @outline.name))) @outline.function
(function_definition
  declarator: (reference_declarator
    (function_declarator
      declarator: (_) @outline.name))) @outline.function

; Aliases and macros.
(alias_declaration
  name: (_) @outline.name) @outline.type_alias
(type_definition
  declarator: (type_identifier) @outline.name) @outline.type_alias
(concept_definition
  name: (_) @outline.name) @outline.type_alias
(preproc_def
  name: (_) @outline.name) @outline.macro
(preproc_function_def
  name: (_) @outline.name) @outline.macro
