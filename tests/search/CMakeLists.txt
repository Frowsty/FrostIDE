# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::search (ADR 0042), label "unit": the fuzzy path matcher of Go to File (word
# starts, CamelHump, path segments, ranking) and the "file:line" suffix parser.
frost_add_test(search_tests
    SOURCES fuzzy_path_test.cpp
    DEPS frost::search)
