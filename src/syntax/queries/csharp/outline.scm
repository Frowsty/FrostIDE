; SPDX-License-Identifier: Apache-2.0
;
; outline.scm (C#): frost::syntax's outline query for tree-sitter-c-sharp 0.23.5 (ADR 0023, D8).
; Written for FrostIDE from the vendored grammar's node types. Each pattern captures a declaration
; as @outline.<kind> and its name as @outline.name; items nest by containment.

(namespace_declaration
  name: (_) @outline.name) @outline.namespace
(file_scoped_namespace_declaration
  name: (_) @outline.name) @outline.namespace

(class_declaration
  name: (_) @outline.name) @outline.class
(record_declaration
  name: (_) @outline.name) @outline.class
(struct_declaration
  name: (_) @outline.name) @outline.struct
(interface_declaration
  name: (_) @outline.name) @outline.interface
(enum_declaration
  name: (_) @outline.name) @outline.enum
(enum_member_declaration
  name: (_) @outline.name) @outline.enum_member
(delegate_declaration
  name: (_) @outline.name) @outline.type_alias

(method_declaration
  name: (_) @outline.name) @outline.method
(local_function_statement
  name: (_) @outline.name) @outline.function
(constructor_declaration
  name: (_) @outline.name) @outline.constructor
(destructor_declaration
  name: (_) @outline.name) @outline.destructor
(property_declaration
  name: (_) @outline.name) @outline.property
(event_declaration
  name: (_) @outline.name) @outline.event
(event_field_declaration
  (variable_declaration
    (variable_declarator
      name: (_) @outline.name))) @outline.event
(field_declaration
  (variable_declaration
    (variable_declarator
      name: (_) @outline.name))) @outline.field
