# SPDX-License-Identifier: Apache-2.0
# Unit tests for frost::compat polyfills.
frost_add_test(compat_tests
    SOURCES function_ref_test.cpp saturate_test.cpp
    DEPS frost::compat)
