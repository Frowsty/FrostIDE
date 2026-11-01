# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::text_editor, the editor model (ADR 0025), label unit: grapheme cluster and word
# navigation (emoji, flags, ZWJ, combining marks, CRLF) and reverse chunk iteration; typing and
# smart keys with their undo grouping; line operations and comments; movement and selection
# (vertical through the DisplayMap, Go to Line, members through an IOutlineProvider double); find
# and replace (plain, case, whole word, regex, replace all as one undo step). editor_fixture.hpp
# holds the caret-marker notation and the Editor fixture they share.

frost_add_test(text_editor_tests
    SOURCES
        lines_test.cpp
        movement_test.cpp
        search_test.cpp
        typing_test.cpp
        unicode_test.cpp
    DEPS frost::text_editor frost::text
    LABELS unit)
target_include_directories(text_editor_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
