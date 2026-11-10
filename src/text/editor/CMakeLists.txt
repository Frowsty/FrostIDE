# SPDX-License-Identifier: Apache-2.0
#
# frost::text_editor (PLAN.md section 3.2, src/text; ADR 0025): the editor model. Editor commands as
# pure functions over a Snapshot and its selections (typing and smart keys, deletion and movement
# by grapheme cluster and word, line operations, comments, find/replace, Go to Line, next/previous
# member through the IOutlineProvider seam) and the Editor that applies them to a Document and its
# DisplayMap. Qt-free, layer text. A library of its own, not part of frost::text, because it links
# RE2 (regex search) and utf8proc (grapheme clusters) from vcpkg, which the fuzz trees do not
# install (src/CMakeLists.txt). Both are linked PRIVATE, so no header of theirs leaks out.

frost_add_library(text_editor LAYER text
    SOURCES
        src/command_support.cpp
        src/commands_edit.cpp
        src/commands_essentials.cpp
        src/commands_lines.cpp
        src/commands_move.cpp
        src/editor.cpp
        src/language.cpp
        src/search.cpp
        src/unicode.cpp
    DEPS frost::text)
target_link_libraries(frost_text_editor PRIVATE re2::re2 utf8proc::utf8proc)
