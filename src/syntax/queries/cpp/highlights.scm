; SPDX-License-Identifier: Apache-2.0
;
; highlights.scm (C++): frost::syntax's highlight query for tree-sitter-cpp 0.23.4 (ADR 0023, D8).
; Written for FrostIDE from the vendored grammar's node types; no third-party query was copied.
; Precedence: the innermost capture wins, and among captures of one node the first pattern in this
; file wins, so specific patterns come before generic ones. Capture names are HighlightKind's.

; Comments
((comment) @comment.doc
  (#frost-doc? @comment.doc))
(comment) @comment

; Preprocessor
(preproc_include
  path: (_) @string)
(preproc_def
  name: (identifier) @macro)
(preproc_function_def
  name: (identifier) @macro)
[
  "#include" "#define" "#if" "#ifdef" "#ifndef" "#else" "#elif" "#elifdef" "#elifndef" "#endif"
  "defined"
] @keyword.directive
(preproc_directive) @keyword.directive

; Literals
; The parentheses of R"x(...)x" are the expression tokens; inside a raw string they are string.
(raw_string_literal
  ["(" ")"] @string)
(raw_string_literal) @string
(string_literal) @string
(system_lib_string) @string
(escape_sequence) @string.escape
(char_literal) @character
(number_literal) @number
[(true) (false)] @boolean
(null) @constant
(this) @keyword

; Calls and declarations
(call_expression
  function: (identifier) @macro
  (#frost-upper? @macro))
(call_expression
  function: (identifier) @function.call)
(call_expression
  function: (field_expression
    field: (field_identifier) @function.call))
(call_expression
  function: (qualified_identifier
    name: (identifier) @function.call))
(call_expression
  function: (template_function
    name: (identifier) @function.call))
(call_expression
  function: (qualified_identifier
    name: (template_function
      name: (identifier) @function.call)))
(call_expression
  function: (field_expression
    field: (template_method
      name: (field_identifier) @function.call)))
(function_declarator
  declarator: (identifier) @function)
(function_declarator
  declarator: (field_identifier) @function)
(function_declarator
  declarator: (qualified_identifier
    name: (identifier) @function))
; The innermost capture wins, so a destructor's name is captured on its identifier.
(destructor_name
  (identifier) @function)

; Types, namespaces, labels and attributes
(primitive_type) @type.builtin
(sized_type_specifier) @type.builtin
(placeholder_type_specifier
  (auto) @type.builtin)
(type_identifier) @type
(namespace_identifier) @namespace
(statement_identifier) @label
(attribute
  name: (identifier) @attribute)

; Members, parameters and names
(field_identifier) @property
(parameter_declaration
  declarator: (identifier) @parameter)
(parameter_declaration
  declarator: (reference_declarator
    (identifier) @parameter))
(parameter_declaration
  declarator: (pointer_declarator
    declarator: (identifier) @parameter))
(optional_parameter_declaration
  declarator: (identifier) @parameter)
(enumerator
  name: (identifier) @constant)
((identifier) @constant
  (#frost-upper? @constant))
(identifier) @variable

; Keywords
[
  "break" "case" "catch" "class" "co_await" "co_return" "co_yield" "concept" "const" "consteval"
  "constexpr" "constinit" "continue" "decltype" "default" "delete" "do" "else" "enum" "explicit"
  "extern" "final" "for" "friend" "goto" "if" "inline" "mutable" "namespace" "new" "noexcept"
  "operator" "override" "private" "protected" "public" "register" "requires" "return" "sizeof"
  "static" "static_assert" "struct" "switch" "template" "thread_local" "throw" "try" "typedef"
  "typename" "union" "using" "virtual" "volatile" "while" "alignas" "alignof" "restrict" "asm"
  "__attribute__" "__declspec" "and" "or" "not" "xor" "bitand" "bitor" "compl" "and_eq" "or_eq"
  "xor_eq" "not_eq"
] @keyword

; Operators and punctuation
[
  "=" "+=" "-=" "*=" "/=" "%=" "&=" "|=" "^=" "<<=" ">>=" "+" "-" "*" "/" "%" "++" "--" "==" "!="
  "<" ">" "<=" ">=" "<=>" "&&" "||" "!" "&" "|" "^" "~" "<<" ">>" "->" "." "->*" ".*" "::" "?"
  "..."
] @operator
["(" ")" "[" "]" "{" "}" "[[" "]]"] @punctuation.bracket
["," ";" ":"] @punctuation
