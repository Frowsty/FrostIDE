# SPDX-License-Identifier: Apache-2.0
#
# frost_text_test_support: the brute-force TextOracle and the OpDriver that the text differential,
# stress and fuzz tests share (ADR 0013). A plain static library with TEST options and no gtest,
# so the libFuzzer target can link it; tests/fuzz adds this directory itself when tests are off.
add_library(frost_text_test_support STATIC
    text_oracle.cpp
    text_ops.cpp)
target_include_directories(frost_text_test_support PUBLIC ${FROST_SOURCE_DIR}/tests)
# frost::sched only for the header-only detail::SplitMix64.
target_link_libraries(frost_text_test_support PUBLIC frost::text frost::io frost::sched)
frost_apply_compile_options(frost_text_test_support TEST)
