; SPDX-License-Identifier: Apache-2.0
;
; highlights.scm (XML): frost::syntax's highlight query for the xml grammar of
; tree-sitter-xml 0.7.0 (ADR 0023, D8). Written for FrostIDE from the vendored grammar's node
; types. The first pattern for a node wins.

(Comment) @comment
(STag
  (Name) @tag)
(ETag
  (Name) @tag)
(EmptyElemTag
  (Name) @tag)
(Attribute
  (Name) @tag.attribute)
(AttValue) @string
(EntityRef) @string.escape
(CharRef) @string.escape
(CDSect) @markup.raw
(XMLDecl) @keyword.directive
(PI) @keyword.directive
(doctypedecl) @keyword.directive
["<" ">" "</" "/>"] @punctuation.bracket
"=" @operator
