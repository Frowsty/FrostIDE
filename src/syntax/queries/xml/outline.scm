; SPDX-License-Identifier: Apache-2.0
;
; outline.scm (XML): frost::syntax's outline query for the xml grammar of tree-sitter-xml 0.7.0
; (ADR 0023, D8). Every element is an Element item named by its tag; children nest.

(element
  (STag
    (Name) @outline.name)) @outline.element
(element
  (EmptyElemTag
    (Name) @outline.name)) @outline.element
