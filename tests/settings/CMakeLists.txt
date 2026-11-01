# SPDX-License-Identifier: Apache-2.0
#
# Tests for frost::settings (ADR 0021), label "unit": the registry and its built-in keys, values and
# schemas, byte-exact document edits, migrations against the golden files in goldens/, the store
# (scope precedence, writes, the trust rule, live reload on MemFsWatcher, validation on save, the
# stand-in actions, the .frost/ layout) and EditorConfig. Everything runs on MemFs and SimExecutor;
# the goldens are read from the source tree.

frost_add_test(settings_unit_tests
    SOURCES
        document_test.cpp
        editorconfig_test.cpp
        migration_test.cpp
        registry_test.cpp
        store_test.cpp
    DEPS frost::settings)
target_compile_definitions(settings_unit_tests PRIVATE FROST_SETTINGS_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/goldens")
