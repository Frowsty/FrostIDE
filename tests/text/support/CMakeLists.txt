# SPDX-License-Identifier: Apache-2.0
#
# Static support libraries for frost::text's tests, with TEST options and no gtest, so the libFuzzer
# targets can link them; tests/fuzz adds this directory itself when tests are off.
#   frost_text_test_support           the brute-force TextOracle and the OpDriver that the text
#                                     differential, stress and fuzz tests share (ADR 0013), and the
#                                     UndoOracle and UndoDriver of the undo and anchor tests (ADR 0017)
#   frost_text_encoding_test_support  the independent encoding oracles, the table-driven fake codec,
#                                     the byte-exact encoding fixtures and the load/save checks and
#                                     golden comparison the unit, stress and fuzz tests share (ADR 0018)
add_library(frost_text_test_support STATIC
    text_oracle.cpp
    text_ops.cpp
    undo_oracle.cpp
    undo_ops.cpp)
target_include_directories(frost_text_test_support PUBLIC ${FROST_SOURCE_DIR}/tests)
# undo_ops.cpp reads the undo tree (src/text/src/undo_tree.hpp) for its white-box region check;
# the headers do not, so users of the library need no private include path.
target_include_directories(frost_text_test_support PRIVATE ${FROST_SOURCE_DIR}/src/text/src)
# frost::sched only for the header-only detail::SplitMix64.
target_link_libraries(frost_text_test_support PUBLIC frost::text frost::io frost::sched)
frost_apply_compile_options(frost_text_test_support TEST)

add_library(frost_text_encoding_test_support STATIC
    encoding_checks.cpp
    encoding_oracle.cpp
    fake_codec.cpp
    encoding_fixtures.cpp)
target_include_directories(frost_text_encoding_test_support PUBLIC ${FROST_SOURCE_DIR}/tests)
# frost::sched for SplitMix64; frost_text_test_support for text::test::oracleDecode, the
# bit-pattern UTF-8 decoder the encoding oracles build on.
target_link_libraries(frost_text_encoding_test_support PUBLIC frost::text frost::io frost::sched frost_text_test_support)
frost_apply_compile_options(frost_text_encoding_test_support TEST)
