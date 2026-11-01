; SPDX-License-Identifier: Apache-2.0
;
; outline.scm (JSON): frost::syntax's outline query for tree-sitter-json 0.24.8 (ADR 0023, D8).
; Every object member is a Key item named by its key; nested objects nest.

(pair
  key: (string
    (string_content) @outline.name)) @outline.key
