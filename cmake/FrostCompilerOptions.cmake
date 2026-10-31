# SPDX-License-Identifier: Apache-2.0
#
# FrostCompilerOptions.cmake: the per-compiler warning sets, language mode and
# hardening flags from PLAN.md section 2 ("Warnings", "C++23/26 policy").
#
# frost_apply_compile_options(<target> <PRODUCTION|TEST>) applies them.
# PRODUCTION code additionally gets -Wglobal-constructors and
# -Wexit-time-destructors (startup hygiene, PLAN.md section 2.1); test code is
# exempt because GoogleTest's TEST() macros register through static objects.

include_guard(GLOBAL)

# MSVC-style flags for clang-cl. Never -Wall: clang-cl reads it as -Weverything.
set(FROST_CLANGCL_WARNINGS
    /W4 /WX
    -Wconversion -Wshadow -Wthread-safety
    # MSVC-compatibility noise that /W4 enables under clang-cl and that does not apply to us.
    -Wno-c++98-compat -Wno-c++98-compat-pedantic)

# GCC-style flags for clang on Linux.
set(FROST_CLANG_WARNINGS
    -Wall -Wextra -Wpedantic
    -Wconversion -Wshadow -Wthread-safety
    -Werror)

# Startup hygiene: no code may run before main() or after it returns.
set(FROST_STARTUP_HYGIENE_WARNINGS -Wglobal-constructors -Wexit-time-destructors)

function(frost_apply_compile_options target kind)
    if(NOT kind MATCHES "^(PRODUCTION|TEST)$")
        message(FATAL_ERROR "frost_apply_compile_options: kind must be PRODUCTION or TEST, got '${kind}'")
    endif()

    # C++23 baseline compiled in C++26 mode (/std:c++latest or -std=c++26), extensions off.
    target_compile_features(${target} PUBLIC cxx_std_26)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)

    if(MSVC)
        # clang-cl in MSVC-compatible mode.
        # Debug information format (/Z7) comes from CMAKE_MSVC_DEBUG_INFORMATION_FORMAT in the root file.
        target_compile_options(${target} PRIVATE
            ${FROST_CLANGCL_WARNINGS}
            /permissive- /Zc:__cplusplus /utf-8 /EHsc /GR)
        target_compile_definitions(${target} PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN STRICT UNICODE _UNICODE
            # Windows 10/11 API surface; FrostIDE supports Windows 11 only (PLAN.md section 1.3).
            _WIN32_WINNT=0x0A00 WINVER=0x0A00
            $<$<CONFIG:Debug>:_MSVC_STL_HARDENING=1>)
        target_link_options(${target} PRIVATE /DEBUG)
    else()
        target_compile_options(${target} PRIVATE ${FROST_CLANG_WARNINGS} -g)
        target_compile_definitions(${target} PRIVATE $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)
    endif()

    if(kind STREQUAL "PRODUCTION")
        target_compile_options(${target} PRIVATE ${FROST_STARTUP_HYGIENE_WARNINGS})
    endif()
endfunction()
