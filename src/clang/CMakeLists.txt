# SPDX-License-Identifier: Apache-2.0
#
# frost-clang (CLANG-0, PLAN.md Appendix C and ADR 0083): the Qt-free out-of-process engine
# executable that links the pinned LLVM 23.1.4 static libraries (clangdMain, clangDaemon,
# clangDaemonTweaks, clangTooling, clangASTMatchers, clangRewrite). On Windows the LLVM 23.1.4
# SDK archives are compiled with /MT (Release static CRT, _ITERATOR_DEBUG_LEVEL=0, no ASan string
# annotations), whereas FrostIDE's main process uses /MDd or /MD; isolating LLVM inside the
# Qt-free frost-clang helper binary keeps the /MT vs /MD boundary at the process level (JSON-RPC
# stdio for the custom FeatureModule in clangd mode, and CLI / file I/O for LibTooling rewrites).

get_filename_component(_frost_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(FROST_LLVM_SDK_ROOT "${_frost_llvm_bin}/.." ABSOLUTE)

if(NOT EXISTS "${FROST_LLVM_SDK_ROOT}/lib/cmake/llvm/LLVMExports.cmake"
   OR NOT EXISTS "${FROST_LLVM_SDK_ROOT}/lib/cmake/clang/ClangTargets.cmake")
    message(FATAL_ERROR "frost-clang: LLVM 23.1.4 CMake exports not found under ${FROST_LLVM_SDK_ROOT}")
endif()

# LLVMSupport's exported interface lists ZLIB::ZLIB and zstd::libzstd_static (and unused targets in
# LLVMExports.cmake list LibXml2 and DIASDK). frost-clang provides the self-contained
# !LLVM_ENABLE_ZLIB / !LLVM_ENABLE_ZSTD llvm::compression::* and IEEE 802.3 crc32 definitions in
# src/llvm_compression_shim.cpp so that Compression.cpp.obj is never pulled from LLVMSupport and the
# engine links without external compression archives in every preset (including fuzz trees).
foreach(_frost_llvm_ext_target IN ITEMS
        ZLIB::ZLIB
        zstd::libzstd_static
        LibXml2::LibXml2
        LibXml2::LibXml2Static
        DIASDK::Diaguids)
    if(NOT TARGET ${_frost_llvm_ext_target})
        add_library(${_frost_llvm_ext_target} INTERFACE IMPORTED)
    endif()
endforeach()

include("${FROST_LLVM_SDK_ROOT}/lib/cmake/llvm/LLVMExports.cmake")
include("${FROST_LLVM_SDK_ROOT}/lib/cmake/clang/ClangTargets.cmake")

# Clear any inherited directory-level compile/link options (such as -RTC1 in Debug or -fsanitize
# in ASan/fuzz trees) so frost-clang's object files always match the uninstrumented /MT Release
# LLVM 23.1.4 static archives.
set_directory_properties(PROPERTIES COMPILE_OPTIONS "" LINK_OPTIONS "")

if(MSVC)
    # In Ninja Multi-Config trees, CMake's default Debug flags add /Ob0 /Od /RTC1 and _DEBUG, which
    # mismatch the /MT Release (_ITERATOR_DEBUG_LEVEL=0) LLVM 23.1.4 static libraries. Scope the
    # Debug flags in this directory to Release-compatible /Z7 /O2 /Ob1 /DNDEBUG.
    set(CMAKE_CXX_FLAGS_DEBUG "/Z7 /O2 /Ob1 /DNDEBUG")
    set(CMAKE_C_FLAGS_DEBUG "/Z7 /O2 /Ob1 /DNDEBUG")
endif()

add_executable(frost-clang
    src/frost_clang_main.cpp
    src/local_rename_plan.cpp
    src/llvm_compression_shim.cpp
)
if(FROST_BUILD_TESTS)
    target_compile_definitions(frost-clang PRIVATE FROST_CLANG_PLAN_TESTS)
endif()

target_compile_features(frost-clang PRIVATE cxx_std_23)

set_target_properties(frost-clang PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)

target_include_directories(frost-clang PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_include_directories(frost-clang SYSTEM PRIVATE
    "${FROST_LLVM_SDK_ROOT}/include"
)

target_compile_definitions(frost-clang PRIVATE
    CLANG_BUILD_STATIC
    NDEBUG
    _CRT_SECURE_NO_DEPRECATE
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_DEPRECATE
    _CRT_NONSTDC_NO_WARNINGS
    _SCL_SECURE_NO_DEPRECATE
    _SCL_SECURE_NO_WARNINGS
    __STDC_CONSTANT_MACROS
    __STDC_FORMAT_MACROS
    __STDC_LIMIT_MACROS
    $<$<PLATFORM_ID:Windows>:UNICODE>
    $<$<PLATFORM_ID:Windows>:_UNICODE>
    $<$<CXX_COMPILER_ID:MSVC>:_HAS_EXCEPTIONS=0>
    $<$<CXX_COMPILER_ID:MSVC>:_ITERATOR_DEBUG_LEVEL=0>
)

if(MSVC)
    target_compile_options(frost-clang PRIVATE
        /utf-8
        /EHs-c-
        /GR-
        /W4
        /WX
        -Wno-unused-parameter
    )
else()
    target_compile_options(frost-clang PRIVATE
        -fno-exceptions
        -fno-rtti
        -Wall
        -Wextra
        -Werror
        -Wno-unused-parameter
    )
endif()

target_link_libraries(frost-clang PRIVATE
    clangdMain
    clangDaemon
    clangDaemonTweaks
    clangdSupport
    clangTooling
    clangToolingCore
    clangToolingRefactoring
    clangToolingSyntax
    clangASTMatchers
    clangRewrite
    clangFrontend
    clangSerialization
    clangDriver
    clangParse
    clangSema
    clangEdit
    clangAnalysis
    clangAST
    clangLex
    clangBasic
    LLVMSupport
)
