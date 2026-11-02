; SPDX-License-Identifier: Apache-2.0
;
; FrostIDE Lua highlights, authored from the pinned grammar's node types.
(comment) @comment
(hash_bang_line) @comment
(string) @string
(escape_sequence) @string.escape
(number) @number
[(true) (false)] @boolean
(nil) @constant
(break_statement) @keyword
(function_declaration name: (_) @function)
(function_call name: (identifier) @function.call)
(parameters name: (identifier) @parameter)
(dot_index_expression field: (identifier) @property)
(method_index_expression method: (identifier) @property)
["and" "do" "else" "elseif" "end" "for" "function" "goto" "if" "in" "local" "not" "or" "repeat" "return" "then" "until" "while"] @keyword
["+" "-" "*" "/" "//" "%" "^" "#" "&" "~" "|" ">>" "<<" ".." "<" "<=" ">" ">=" "==" "~=" "="] @operator
["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ";" ":" "."] @punctuation
