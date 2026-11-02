# SPDX-License-Identifier: Apache-2.0
# Unit tests for frost::core. The assert_*_tu.cpp files each compile the
# contract macros under a different FROST_CONTRACT_SEMANTIC.
frost_add_test(core_tests
    SOURCES
        assert_test.cpp
        assert_ignore_tu.cpp
        assert_observe_tu.cpp
        assert_quick_tu.cpp
        clock_test.cpp
        error_test.cpp
        i18n_test.cpp
        log_test.cpp
        mutex_test.cpp
        strong_type_test.cpp
    DEPS frost::core)
target_include_directories(core_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
