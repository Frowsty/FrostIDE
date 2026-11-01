; SPDX-License-Identifier: Apache-2.0
;
; highlights.scm (C#): frost::syntax's highlight query for tree-sitter-c-sharp 0.23.5 (ADR 0023,
; D8). Written for FrostIDE from the vendored grammar's node types; no third-party query was
; copied. Precedence: the innermost capture wins, and among captures of one node the first pattern
; in this file wins, so specific patterns come before generic ones.

; Comments
((comment) @comment.doc
  (#frost-doc? @comment.doc))
(comment) @comment

; Preprocessor, including #region and #endregion
(preproc_region) @keyword.directive
(preproc_endregion) @keyword.directive
[
  "#if" "#elif" "#else" "#endif" "#define" "#undef" "#warning" "#error" "#line" "#pragma"
  "#nullable"
] @keyword.directive

; Literals
(interpolation_brace) @punctuation.bracket
(interpolated_string_expression) @string
(raw_string_literal) @string
(verbatim_string_literal) @string
(string_literal) @string
(escape_sequence) @string.escape
(character_literal) @character
(integer_literal) @number
(real_literal) @number
(boolean_literal) @boolean
(null_literal) @constant

; Attributes
(attribute
  name: (_) @attribute)

; Calls and declarations
(invocation_expression
  function: (identifier) @function.call)
(invocation_expression
  function: (member_access_expression
    name: (identifier) @function.call))
(invocation_expression
  function: (generic_name
    (identifier) @function.call))
(method_declaration
  name: (identifier) @function)
(local_function_statement
  name: (identifier) @function)
(constructor_declaration
  name: (identifier) @function)
(destructor_declaration
  name: (identifier) @function)

; Types and namespaces
(predefined_type) @type.builtin
(implicit_type) @type.builtin
(class_declaration
  name: (identifier) @type)
(struct_declaration
  name: (identifier) @type)
(interface_declaration
  name: (identifier) @type)
(enum_declaration
  name: (identifier) @type)
(record_declaration
  name: (identifier) @type)
(object_creation_expression
  type: (identifier) @type)
(variable_declaration
  type: (identifier) @type)
(parameter
  type: (identifier) @type)
(property_declaration
  type: (identifier) @type)
(method_declaration
  returns: (identifier) @type)
(base_list
  (identifier) @type)
(generic_name
  (identifier) @type)
; The innermost capture wins, so namespaces are captured on their identifiers: every qualifier of
; a qualified name, and the last part too in namespace declarations and using directives.
(namespace_declaration
  name: (identifier) @namespace)
(namespace_declaration
  name: (qualified_name
    name: (identifier) @namespace))
(file_scoped_namespace_declaration
  name: (identifier) @namespace)
(file_scoped_namespace_declaration
  name: (qualified_name
    name: (identifier) @namespace))
(using_directive
  (identifier) @namespace)
(using_directive
  (qualified_name
    name: (identifier) @namespace))
(qualified_name
  qualifier: (identifier) @namespace)
(qualified_name
  qualifier: (qualified_name
    name: (identifier) @namespace))

; Members, parameters and names
(property_declaration
  name: (identifier) @property)
(enum_member_declaration
  name: (identifier) @constant)
(member_access_expression
  name: (identifier) @property)
(parameter
  name: (identifier) @parameter)
(identifier) @variable

; Keywords
(modifier) @keyword
[
  "abstract" "as" "async" "await" "base" "break" "case" "catch" "checked" "class" "const"
  "continue" "default" "delegate" "do" "else" "enum" "event" "explicit" "extern" "finally" "fixed"
  "for" "foreach" "get" "goto" "if" "implicit" "in" "init" "interface" "internal" "is" "lock"
  "namespace" "new" "operator" "out" "override" "params" "partial" "private" "protected" "public"
  "readonly" "record" "ref" "required" "return" "sealed" "set" "sizeof" "stackalloc" "static"
  "struct" "switch" "this" "throw" "try" "typeof" "unchecked" "unsafe" "using" "var" "virtual"
  "volatile" "when" "where" "while" "with" "yield" "add" "remove" "global" "not" "and" "or"
  "from" "select" "orderby" "group" "by" "into" "join" "let" "on" "equals" "ascending"
  "descending" "file" "scoped"
] @keyword

; Operators and punctuation
[
  "=" "+=" "-=" "*=" "/=" "%=" "&=" "|=" "^=" "<<=" ">>=" ">>>=" "??=" "+" "-" "*" "/" "%" "++"
  "--" "==" "!=" "<" ">" "<=" ">=" "&&" "||" "!" "&" "|" "^" "~" "<<" ">>" ">>>" "??" "?" "=>"
  "->" "." ".." "::"
] @operator
["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ";" ":"] @punctuation
