; SPDX-License-Identifier: Apache-2.0
;
; outline.scm (Markdown): frost::syntax's outline query for the block grammar of
; tree-sitter-markdown 0.5.4 (ADR 0023, D8). A section (a heading and everything up to the next
; heading of its level or higher) is a Heading item named by the heading's text; subsections nest.

(section
  (atx_heading
    heading_content: (_) @outline.name)) @outline.heading
(section
  (setext_heading
    heading_content: (_) @outline.name)) @outline.heading
