; SPDX-License-Identifier: Apache-2.0
;
; highlights.scm (Markdown): frost::syntax's highlight query for the block grammar of
; tree-sitter-markdown 0.5.4 (ADR 0023, D8). Written for FrostIDE from the vendored grammar's node
; types. Inline markup (emphasis, code spans, links in paragraphs) needs the inline grammar.
; TODO(M4): inject tree-sitter-markdown-inline and fenced-code languages.

(atx_heading) @markup.heading
(setext_heading) @markup.heading
(pipe_table_header) @markup.heading
(fenced_code_block_delimiter) @punctuation
(info_string) @label
(fenced_code_block) @markup.raw
(indented_code_block) @markup.raw
[
  (list_marker_minus) (list_marker_plus) (list_marker_star) (list_marker_dot)
  (list_marker_parenthesis) (task_list_marker_checked) (task_list_marker_unchecked)
] @markup.list
(block_quote) @markup.quote
(thematic_break) @punctuation
(pipe_table_delimiter_row) @punctuation
(link_reference_definition
  (link_label) @markup.link)
(link_destination) @markup.link
(link_title) @string
(html_block) @tag
(minus_metadata) @comment
(plus_metadata) @comment
(backslash_escape) @string.escape
(entity_reference) @string.escape
(numeric_character_reference) @string.escape
