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
# Anchors, selections, Document and the undo tree (ADR 0017) add to each executable:
#   text_tests         anchors, AnchorSet, SelectionSet, Document (undo, coalescing, groups, IME),
#                      region composition (white-box), the 200-seed undo differential against the
#                      UndoOracle, the undo identity properties and concurrent anchor readers
#   text_stress_tests  the undo differential at 500k ops, the same with thousands of anchors at
#                      100k ops, and concurrent anchor readers at 500k ops
#   text_perf_tests    Document typing and undo/redo latency (B5 text sub-budget) and the anchor and
#                      caret report
# The white-box tests read the library's private undo_tree.hpp and anchor_paths.hpp (src/text/src).
add_subdirectory(support)

frost_add_test(text_tests
    SOURCES
        concurrency_test.cpp
        differential_test.cpp
        sum_tree_test.cpp
        text_buffer_test.cpp
        text_summary_test.cpp
        anchor_test.cpp
        anchor_set_test.cpp
        selection_test.cpp
        document_test.cpp
        undo_compose_test.cpp
        undo_differential_test.cpp
        undo_property_test.cpp
        anchor_concurrency_test.cpp
    DEPS frost::text frost_text_test_support
    LABELS unit)
target_include_directories(text_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
target_include_directories(text_tests PRIVATE ${FROST_SOURCE_DIR}/src/text/src)

frost_add_test(text_stress_tests
    SOURCES
        stress_test.cpp
        undo_stress_test.cpp
    DEPS frost::text frost_text_test_support
    LABELS stress)
target_include_directories(text_stress_tests PRIVATE ${FROST_SOURCE_DIR}/tests)

# The perf tests read detail::treeStats from the library's private piece_tree.hpp (and the undo
# history from undo_tree.hpp); frost::sched provides the header-only SplitMix64.
frost_add_test(text_perf_tests
    SOURCES
        perf_test.cpp
        document_perf_test.cpp
    DEPS frost::text frost::sched
    LABELS perf)
target_include_directories(text_perf_tests PRIVATE ${FROST_SOURCE_DIR}/tests ${FROST_SOURCE_DIR}/src/text/src)
