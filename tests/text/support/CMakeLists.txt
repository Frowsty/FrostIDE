# SPDX-License-Identifier: Apache-2.0
#
# frost_text_test_support: the brute-force TextOracle and the OpDriver that the text differential,
# stress and fuzz tests share (ADR 0013), and the UndoOracle and UndoDriver of the undo and anchor
# tests (ADR 0017). A plain static library with TEST options and no gtest, so the libFuzzer target
# can link it; tests/fuzz adds this directory itself when tests are off.
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
