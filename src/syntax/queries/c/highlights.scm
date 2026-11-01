; SPDX-License-Identifier: Apache-2.0
;
; highlights.scm (C): frost::syntax's highlight query for tree-sitter-c 0.24.2 (ADR 0023, D8).
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
(string_literal) @string
(system_lib_string) @string
(escape_sequence) @string.escape
(char_literal) @character
(number_literal) @number
[(true) (false)] @boolean
(null) @constant

; Calls and declarations
(call_expression
  function: (identifier) @macro
  (#frost-upper? @macro))
(call_expression
  function: (identifier) @function.call)
(call_expression
  function: (field_expression
    field: (field_identifier) @function.call))
(function_declarator
  declarator: (identifier) @function)

; Types and labels
(primitive_type) @type.builtin
(sized_type_specifier) @type.builtin
(type_identifier) @type
(statement_identifier) @label
(attribute
  name: (identifier) @attribute)

; Members, parameters and names
(field_identifier) @property
(parameter_declaration
  declarator: (identifier) @parameter)
(parameter_declaration
  declarator: (pointer_declarator
    declarator: (identifier) @parameter))
(enumerator
  name: (identifier) @constant)
((identifier) @constant
  (#frost-upper? @constant))
(identifier) @variable

; Keywords
[
  "break" "case" "const" "continue" "default" "do" "else" "enum" "extern" "for" "goto" "if"
  "inline" "register" "restrict" "return" "sizeof" "static" "struct" "switch" "typedef" "union"
  "volatile" "while" "auto" "constexpr" "thread_local" "_Atomic" "_Noreturn" "noreturn"
  "_Alignas" "_Alignof" "alignas" "alignof" "_Generic" "offsetof" "__attribute__" "__declspec"
  "asm" "__asm__"
] @keyword

; Operators and punctuation
[
  "=" "+=" "-=" "*=" "/=" "%=" "&=" "|=" "^=" "<<=" ">>=" "+" "-" "*" "/" "%" "++" "--" "==" "!="
  "<" ">" "<=" ">=" "&&" "||" "!" "&" "|" "^" "~" "<<" ">>" "->" "." "?" "..."
] @operator
["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ";" ":"] @punctuation
