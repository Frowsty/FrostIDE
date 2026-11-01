# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::text (ADR 0013), split by label because labels apply per executable:
#   text_tests         unit:   TextSummary, SumTree, TextBuffer edge cases, the 200-seed differential
#                              against the brute-force oracle, and concurrent readers (TSan in WSL -Full)
#   text_stress_tests  stress: the 1M-op differential (FROST_STRESS_FULL=1: the 10M-op M1 exit run),
#                              the F2 single line and 1M edits with concurrent readers
#   text_perf_tests    perf:   edit latency (B5 text sub-budget, asserted only in optimised, unsanitised
#                              builds), load throughput (B15 projection) and seek latency
# support/ holds the oracle and op driver they share with the fuzz target (tests/fuzz).
# Parallel ctest (frost_add_test's policy; ADR 0027 decision 3): text_tests runs concurrent readers
# (PROCESSORS 2), the stress runs PROCESSORS 4 (-Full only), and the perf test asserts edit latency
# in Release, so it runs with nothing beside it (RUN_SERIAL, the perf label's default).
add_subdirectory(support)

frost_add_test(text_tests
    SOURCES
        concurrency_test.cpp
        differential_test.cpp
        sum_tree_test.cpp
        text_buffer_test.cpp
        text_summary_test.cpp
    DEPS frost::text frost_text_test_support
    LABELS unit
    PROCESSORS 2)
target_include_directories(text_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

frost_add_test(text_stress_tests
    SOURCES
        stress_test.cpp
    DEPS frost::text frost_text_test_support
    LABELS stress
    PROCESSORS 4)
target_include_directories(text_stress_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

# The perf tests read detail::treeStats from the library's private piece_tree.hpp; frost::sched
# provides the header-only SplitMix64.
frost_add_test(text_perf_tests
    SOURCES
        perf_test.cpp
    DEPS frost::text frost::sched
    LABELS perf)
target_include_directories(text_perf_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/text/src)
