# SPDX-License-Identifier: Apache-2.0
#
# FrostCompilerOptions.cmake: the per-compiler warning sets, language mode and
# hardening flags from PLAN.md section 2 ("Warnings", "C++23/26 policy").
#
# frost_apply_compile_options(<target> <PRODUCTION|TEST> [NO_SANITIZE]) applies them.
# PRODUCTION code additionally gets -Wglobal-constructors and
# -Wexit-time-destructors (startup hygiene, PLAN.md section 2.1); test code is
# exempt because GoogleTest's TEST() macros register through static objects.
# The sanitizer flags of FROST_SANITIZE (FROST_SANITIZE_COMPILE_OPTIONS and
# FROST_SANITIZE_LINK_OPTIONS, set by the root CMakeLists.txt) are applied here too, per
# target, so that every FrostIDE target agrees on the runtime. NO_SANITIZE leaves them out;
# only frost.exe's Windows launcher uses it (src/app/CMakeLists.txt): it is a dozen loader
# calls that must run before any DLL beside it loads, and an instrumented launcher imports
# the ASan runtime, whose RT_MANIFEST 2 brings back the loader's kill before main on long
# plain starts (ADR 0012).

include_guard(GLOBAL)

# MSVC-style flags for clang-cl. Never -Wall: clang-cl reads it as -Weverything.
set(FROST_CLANGCL_WARNINGS
    /W4 /WX
    -Wconversion -Wshadow -Wthread-safety)

# GCC-style flags for clang on Linux.
set(FROST_CLANG_WARNINGS
    -Wall -Wextra -Wpedantic
    -Wconversion -Wshadow -Wthread-safety
    -Werror)

# Startup hygiene: no code may run before main() or after it returns.
set(FROST_STARTUP_HYGIENE_WARNINGS -Wglobal-constructors -Wexit-time-destructors)

# CMake's MSVC Ninja link rule places LINK_FLAGS after the response-file libraries.
# The runtime import must precede Qt and other user DLLs, whose CRT startup can allocate
# before ASan hooks exist (https://github.com/llvm/llvm-project/issues/61685).
function(frost_prepend_asan_runtime target)
    if(WIN32 AND "address" IN_LIST FROST_SANITIZE)
        get_target_property(target_type ${target} TYPE)
        if(target_type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
            get_property(link_libraries TARGET ${target} PROPERTY LINK_LIBRARIES)
            set_property(TARGET ${target} PROPERTY LINK_LIBRARIES
                "${FROST_ASAN_RUNTIME_LIBRARY};${link_libraries}")
        endif()
    endif()
endfunction()

function(frost_apply_compile_options target kind)
    cmake_parse_arguments(ARG "NO_SANITIZE" "" "" ${ARGN})
    if(NOT kind MATCHES "^(PRODUCTION|TEST)$")
        message(FATAL_ERROR "frost_apply_compile_options: kind must be PRODUCTION or TEST, got '${kind}'")
    endif()
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "frost_apply_compile_options: unexpected arguments '${ARG_UNPARSED_ARGUMENTS}'")
    endif()
    if(NOT ARG_NO_SANITIZE)
        target_compile_options(${target} PRIVATE ${FROST_SANITIZE_COMPILE_OPTIONS})
        target_link_options(${target} PRIVATE ${FROST_SANITIZE_LINK_OPTIONS})
        frost_prepend_asan_runtime(${target})
    endif()

    # C++23 baseline compiled in C++26 mode (/std:c++latest or -std=c++26), extensions off.
    target_compile_features(${target} PUBLIC cxx_std_26)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)

    # Standard-library hardening in Debug and in sanitizer (verification) builds,
    # PLAN.md section 2 ("C++23/26 policy"); tests/toolchain_probe/hardening_test.cpp
    # checks it. A condition for $<...:...>: always true with FROST_SANITIZE set.
    if(FROST_SANITIZE)
        set(hardened 1)
    else()
        set(hardened "$<CONFIG:Debug>")
    endif()

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
            $<${hardened}:_MSVC_STL_HARDENING=1>)
        target_link_options(${target} PRIVATE /DEBUG)
    else()
        target_compile_options(${target} PRIVATE ${FROST_CLANG_WARNINGS} -g)
        target_compile_definitions(${target} PRIVATE $<${hardened}:_GLIBCXX_ASSERTIONS>)
        # A GNU build-id note in every linked binary: freeze reports and tools/symbolize.py
        # identify ELF objects by it (ADR 0022). Static libraries ignore link options.
        target_link_options(${target} PRIVATE -Wl,--build-id=sha1)
    endif()

    if(kind STREQUAL "PRODUCTION")
        target_compile_options(${target} PRIVATE ${FROST_STARTUP_HYGIENE_WARNINGS})
    endif()
endfunction()

# frost_apply_c_options(<target> <OWN|VENDORED>): C11 options for the C targets of frost::syntax
# (ADR 0023). OWN is our C code (the tree-sitter runtime wrapper) with the full warning set as
# errors; VENDORED is upstream C we never edit (the grammars), compiled with warnings off because
# their diagnostics are not ours to fix. Both get FROST_SANITIZE's flags, debug information and
# upstream's feature macros. No /EHsc, /GR or C++ hardening macros: they are C++ only.
function(frost_apply_c_options target kind)
    if(NOT kind MATCHES "^(OWN|VENDORED)$")
        message(FATAL_ERROR "frost_apply_c_options: kind must be OWN or VENDORED, got '${kind}'")
    endif()
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE ${FROST_SANITIZE_COMPILE_OPTIONS})
    target_link_options(${target} PRIVATE ${FROST_SANITIZE_LINK_OPTIONS})
    frost_prepend_asan_runtime(${target})
    if(MSVC)
        if(kind STREQUAL "OWN")
            target_compile_options(${target} PRIVATE /W4 /WX -Wconversion -Wshadow /utf-8)
        else()
            target_compile_options(${target} PRIVATE /w /utf-8)
        endif()
        target_compile_definitions(${target} PRIVATE
            WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00 _CRT_SECURE_NO_WARNINGS)
    else()
        if(kind STREQUAL "OWN")
            target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror -g)
        else()
            target_compile_options(${target} PRIVATE -w -g)
        endif()
        # The feature macros of upstream tree-sitter's own CMakeLists.txt (fdopen, strdup).
        target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE)
    endif()
endfunction()
