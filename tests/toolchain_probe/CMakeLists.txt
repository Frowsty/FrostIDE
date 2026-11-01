# SPDX-License-Identifier: Apache-2.0
#
# toolchain_probe (PLAN.md section 2.1): proves on every supported compiler that
#  - the C++26 allowlist compiles and behaves (allowlist_test, a normal unit test),
#  - in ASan builds, the sanitizer really instruments the code (sanitizer_test),
#  - standard-library hardening is on where PLAN.md section 2 requires it (hardening_test), and
#  - banned constructs are rejected by the compiler under our warning flags.
# Each of the three compile-fail probes below (embed, contracts, reflection) is an
# EXCLUDE_FROM_ALL object library built with the same options as production code; its
# test builds it and passes only if the build log contains the expected error, so an
# unrelated failure cannot pass. Constructs the compiler accepts are rejected by
# frost-lint instead: clang 23 compiles expansion statements (`template for`) and
# `export module` units without complaint. banned/expansion_statement.cpp and
# banned/named_module.cpp show them; they are not built here, and
# tests/tools/test_frost_lint.py checks that frost-lint's cpp26 rule rejects them.

frost_add_test(toolchain_probe_tests
    SOURCES allowlist_test.cpp hardening_test.cpp sanitizer_test.cpp
    DEPS frost::compat)
target_include_directories(toolchain_probe_tests PRIVATE ${FROST_SOURCE_DIR}/tests)
if("address" IN_LIST FROST_SANITIZE)
    # sanitizer_test.cpp turns this into a compile error if ASan did not reach it.
    target_compile_definitions(toolchain_probe_tests PRIVATE FROST_EXPECT_ASAN)
endif()
# hardening_test.cpp turns this into a compile error if the standard library is not
# hardened: Debug and every sanitizer (verification) build require it.
if(FROST_SANITIZE)
    target_compile_definitions(toolchain_probe_tests PRIVATE FROST_EXPECT_STL_HARDENING)
else()
    target_compile_definitions(toolchain_probe_tests PRIVATE $<$<CONFIG:Debug>:FROST_EXPECT_STL_HARDENING>)
endif()

# Adds a compile-fail probe: builds banned/<name>.cpp and expects `regex` in the output.
# CTest ignores the exit code when PASS_REGULAR_EXPRESSION is set, so `regex` must match
# only an error: a warning (warnings-as-errors lost) must not pass.
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

# #embed is only a warning (-Wc23-extensions) unless /WX or -Werror makes it an error,
# so the regex names the error form.
frost_add_banned_probe(embed "error: #embed is a Clang extension")
frost_add_banned_probe(contracts "expected function body after function declarator")
frost_add_banned_probe(reflection "error: (type name requires a specifier or qualifier|expected expression)")
