# SPDX-License-Identifier: Apache-2.0
#
# Fuzz targets (cmake/FrostFuzz.cmake). In a fuzz tree (FROST_FUZZ, tests off) each target is a
# libFuzzer executable with CTest fuzz.<name> (label fuzz); in a normal test tree it is a corpus
# replay with CTest fuzz_replay.<name> (label unit). corpus/<name>/ holds the committed seeds.
#
#   text_piece_tree  frost::text's piece tree against the brute-force oracle (ADR 0013)
#   text_decode      frost::text's encodings: the lossy Unicode decoders against independent
#                    oracles and the load/save pipeline over platform and fake codecs (ADR 0018)

# The text test support libraries (frost_text_test_support and frost_text_encoding_test_support):
# already targets in normal trees (tests/text), added here in fuzz trees. Their source is not a
# subdirectory of this one, so the binary directory is explicit.
if(NOT TARGET frost_text_test_support)
    add_subdirectory(${FROST_SOURCE_DIR}/tests/text/support ${CMAKE_BINARY_DIR}/tests/text/support)
endif()

frost_add_fuzz_target(text_piece_tree
    SOURCES
        text_piece_tree_harness.cpp
        text_piece_tree_fuzzer.cpp
    DEPS frost_text_test_support
    CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/text_piece_tree)

frost_add_fuzz_target(text_decode
    SOURCES
        text_decode_harness.cpp
        text_decode_fuzzer.cpp
    DEPS frost_text_encoding_test_support
    CORPUS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/text_decode)
