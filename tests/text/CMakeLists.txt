# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::text (ADR 0013), split by label because labels apply per executable:
#   text_tests         unit:   TextSummary, SumTree, TextBuffer edge cases, the 200-seed differential
#                              against the brute-force oracle, and concurrent readers (TSan in WSL -Full)
#   text_stress_tests  stress: the 1M-op differential (FROST_STRESS_FULL=1: the 10M-op M1 exit run),
#                              the F2 single line and 1M edits with concurrent readers
#   text_perf_tests    perf:   edit latency (B5 text sub-budget, asserted only in optimised, unsanitised
#                              builds), load throughput (B15 projection) and seek latency
# and for the encodings (ADR 0018):
#   text_encoding_tests              unit:        names and BOMs, the Unicode and legacy decoders against
#                                                 independent oracles, the cross-OS fingerprint differential,
#                                                 line separators, detection, load/save, the byte-exact
#                                                 fixtures (row 60's acceptance test) and parallel codecs
#   text_encoding_integration_tests  integration: fixtures through the real file system
#   text_encoding_stress_tests       stress:      every BMP scalar through every code page, and the 20k-seed
#                                                 lossless property
#   text_encoding_perf_tests         perf:        the UTF-8 load overhead (D15), bounded detection (B15),
#                                                 decode throughput and dense-invalid decoding (D17)
# support/ holds the oracles, op driver, fake codec and fixtures they share with the fuzz targets
# (tests/fuzz).
add_subdirectory(support)

frost_add_test(text_tests
    SOURCES
        concurrency_test.cpp
        differential_test.cpp
        sum_tree_test.cpp
        text_buffer_test.cpp
        text_summary_test.cpp
    DEPS frost::text frost_text_test_support
    LABELS unit)
target_include_directories(text_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

frost_add_test(text_stress_tests
    SOURCES
        stress_test.cpp
    DEPS frost::text frost_text_test_support
    LABELS stress)
target_include_directories(text_stress_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

# The perf tests read detail::treeStats from the library's private piece_tree.hpp; frost::sched
# provides the header-only SplitMix64.
frost_add_test(text_perf_tests
    SOURCES
        perf_test.cpp
    DEPS frost::text frost::sched
    LABELS perf)
target_include_directories(text_perf_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/text/src)

# The encoding tests' data directories: committed fixtures, goldens and fuzz seeds (written only
# with FROST_UPDATE_GOLDENS=1), and the directory differential mismatches dump their tables to.
set(frost_encoding_test_dirs
    FROST_ENCODING_FIXTURES_DIR="${FROST_SOURCE_DIR}/tests/text/fixtures/encodings"
    FROST_ENCODING_GOLDENS_DIR="${FROST_SOURCE_DIR}/tests/text/goldens/encodings"
    FROST_ENCODING_CORPUS_DIR="${FROST_SOURCE_DIR}/tests/fuzz/corpus/text_decode"
    FROST_ENCODING_DUMP_DIR="${CMAKE_BINARY_DIR}/encoding-tables")

frost_add_test(text_encoding_tests
    SOURCES
        encoding_table_test.cpp
        unicode_codec_test.cpp
        legacy_codec_test.cpp
        encoding_differential_test.cpp
        line_separator_test.cpp
        encoding_detection_test.cpp
        text_file_test.cpp
        encoding_round_trip_test.cpp
        encoding_concurrency_test.cpp
    DEPS frost::text frost_text_encoding_test_support
    LABELS unit)
target_include_directories(text_encoding_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(text_encoding_tests PRIVATE ${frost_encoding_test_dirs})

frost_add_test(text_encoding_integration_tests
    SOURCES
        encoding_files_test.cpp
    DEPS frost::text frost_text_encoding_test_support
    LABELS integration)
target_include_directories(text_encoding_integration_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(text_encoding_integration_tests PRIVATE ${frost_encoding_test_dirs})

frost_add_test(text_encoding_stress_tests
    SOURCES
        encoding_differential_stress_test.cpp
    DEPS frost::text frost_text_encoding_test_support
    LABELS stress)
target_include_directories(text_encoding_stress_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(text_encoding_stress_tests PRIVATE ${frost_encoding_test_dirs})

frost_add_test(text_encoding_perf_tests
    SOURCES
        encoding_perf_test.cpp
    DEPS frost::text frost::sched
    LABELS perf)
target_include_directories(text_encoding_perf_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_compile_definitions(text_encoding_perf_tests PRIVATE ${frost_encoding_test_dirs})
