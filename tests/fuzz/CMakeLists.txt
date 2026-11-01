# SPDX-License-Identifier: Apache-2.0
#
# Fuzz targets (cmake/FrostFuzz.cmake). In a fuzz tree (FROST_FUZZ, tests off) each target is a
# libFuzzer executable with CTest fuzz.<name> (label fuzz); in a normal test tree it is a corpus
# replay with CTest fuzz_replay.<name> (label unit). corpus/<name>/ holds the committed seeds.
#
#   text_piece_tree  frost::text's piece tree against the brute-force oracle (ADR 0013)
#   diff_merge       frost::diff line diffs, refinement and three-way merges against the support
#                    library's independent appliers and invariants (ADR 0019)

# The text test support library: already a target in normal trees (tests/text), added here in
# fuzz trees. Its source is not a subdirectory of this one, so the binary directory is explicit.
if(NOT TARGET frost_text_test_support)
    add_subdirectory(${FROST_SOURCE_DIR}/tests/text/support ${CMAKE_BINARY_DIR}/tests/text/support)
endif()

frost_add_fuzz_target(text_piece_tree
    SOURCES
        text_piece_tree_harness.cpp
        text_piece_tree_fuzzer.cpp
    DEPS frost_text_test_support
    CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/text_piece_tree)

# The diff test support library: a target in normal trees (tests/diff, added before this
# directory), added here in fuzz trees, where only it (not the git support) is built.
if(NOT TARGET frost_diff_test_support)
    add_subdirectory(${FROST_SOURCE_DIR}/tests/diff/support ${CMAKE_BINARY_DIR}/tests/diff/support)
endif()

frost_add_fuzz_target(diff_merge
    SOURCES
        diff_merge_harness.cpp
        diff_merge_fuzzer.cpp
    DEPS frost_diff_test_support
    CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/diff_merge)
