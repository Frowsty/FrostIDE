# SPDX-License-Identifier: Apache-2.0
#
# toolchain_probe (PLAN.md section 2.1): proves on every supported compiler that
#  - the C++26 allowlist compiles and behaves (allowlist_test, a normal unit test), and
#  - banned constructs are rejected by the compiler under our warning flags.
# Each banned fixture is an EXCLUDE_FROM_ALL object library built with the same
# options as production code; its test builds it and passes only if the build log
# contains the expected diagnostic, so an unrelated failure cannot pass. Constructs
# the compiler accepts are rejected by frost-lint instead: clang 23 compiles
# expansion statements (`template for`) and `export module` units without complaint.

frost_add_test(toolchain_probe_tests
    SOURCES allowlist_test.cpp
    DEPS frost::compat)

# Adds a compile-fail probe: builds banned/<name>.cpp and expects `regex` in the output.
function(frost_add_banned_probe name regex)
    set(target probe_banned_${name})
    add_library(${target} OBJECT EXCLUDE_FROM_ALL banned/${name}.cpp)
    frost_apply_compile_options(${target} PRODUCTION)
    add_test(NAME ToolchainProbe.Rejects.${name}
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --config $<CONFIG> --target ${target})
    set_tests_properties(ToolchainProbe.Rejects.${name} PROPERTIES
        PASS_REGULAR_EXPRESSION "${regex}"
        LABELS unit
        # Each probe runs a build in this tree; never two at once.
        RESOURCE_LOCK frost_build_tree)
endfunction()

frost_add_banned_probe(embed "#embed is a Clang extension")
frost_add_banned_probe(contracts "expected function body after function declarator")
frost_add_banned_probe(reflection "error: (type name requires a specifier or qualifier|expected expression)")
