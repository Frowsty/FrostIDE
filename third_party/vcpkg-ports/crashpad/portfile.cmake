# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: Microsoft Corporation and vcpkg contributors
#
# vcpkg portfile for crashpad: an overlay of vcpkg's crashpad port at 2026.10.16 (9eec002), same
# crashpad (efdc820b), mini_chromium (e5169551) and linux-syscall-support (9719c1e1) revisions
# (ADR 0022). Changes from the upstream port, all for FrostIDE's rules:
#   - Windows builds with the pinned clang-cl, lld-link and llvm-ml (FROST_LLVM_ROOT; D2, never
#     cl.exe), through mini_chromium's own clang toolchain (mini_chromium_is_clang=true), against
#     the MSVC toolset and Windows SDK that sdk/sdk.lock.json pins: mini-chromium-pinned-msvc.patch
#     makes win_helper.py take VSINSTALLDIR's vcvarsall.bat with -vcvars_ver and the SDK version
#     (never vswhere's "latest"). The x64-windows-clangcl-asan triplet's flags (-fsanitize=address)
#     reach crashpad's objects, and the ASan runtime directory and inputs reach the handler's
#     link. The GN argument strings are escaped on Windows too, because the flags may hold
#     quoted paths with spaces.
#   - frost-pipe-dacl.patch gives the Windows handler's registration pipe a DACL for the current
#     user and SYSTEM only, with a medium no-write-up label (upstream also admits AppContainers
#     and untrusted integrity, and falls back to a descriptor without a DACL).
#   - The Linux handler copy in tools/crashpad is made executable as well (upstream marks only
#     the original in tools/).
#   - frost-getopt.patch replaces the public-domain getopt that the Windows handler links with an
#     Apache-2.0 one written for FrostIDE (the dedication is not on the SPDX allowlist).
#   - crashpad's compat library is installed and linked (crashpadConfig.cmake.in): util uses it.
#   - The copyright file lists every component linked into the libraries or the handler:
#     crashpad (Apache-2.0), mini_chromium (BSD-3-Clause) and, on Linux, lss (BSD-3-Clause).
#   - Only Windows x64 and Linux are supported (FrostIDE's platforms); the Android, macOS and
#     MinGW branches are dropped.
# Delete the overlay when upstream builds with clang-cl against a pinned toolset and ships no
# public-domain code, or revisit it at every vcpkg baseline bump (PLAN.md 2.3).
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://chromium.googlesource.com/crashpad/crashpad
    REF efdc820b087c20eec9e32cb5e5b1a63dcf73a724
    PATCHES
        fix-missing-stdint.patch
        frost-getopt.patch
        # FrostIDE: the handler pipe admits the current user and SYSTEM only.
        frost-pipe-dacl.patch
        # The pinned LLVM ASan dispatcher selects faults before Crashpad's user filter.
        frost-asan-seh-dispatch.patch
)

vcpkg_find_acquire_program(PYTHON3)
x_vcpkg_get_python_packages(OUT_PYTHON_VAR PYTHON3
    PYTHON_EXECUTABLE "${PYTHON3}"
    PYTHON_VERSION "3"
    PACKAGES setuptools
)
vcpkg_replace_string("${SOURCE_PATH}/.gn" "script_executable = \"python3\"" "script_executable = \"${PYTHON3}\"")

# mini_chromium contains the toolchains and build configuration
if(NOT EXISTS "${SOURCE_PATH}/third_party/mini_chromium/mini_chromium/BUILD.gn")
    vcpkg_from_git(OUT_SOURCE_PATH mini_chromium
        URL "https://chromium.googlesource.com/chromium/mini_chromium"
        REF e5169551c51f3a52eee36b3b03f219cefe380237
        PATCHES
            # Honor vcpkg's Linux toolchain, including GCC, and fix ARM Android GN generation.
            mini-chromium-toolchain.patch
            # FrostIDE: the pinned MSVC toolset and Windows SDK through VSINSTALLDIR.
            mini-chromium-pinned-msvc.patch
    )

    file(REMOVE_RECURSE "${SOURCE_PATH}/third_party/mini_chromium/mini_chromium")
    file(RENAME "${mini_chromium}" "${SOURCE_PATH}/third_party/mini_chromium/mini_chromium")
endif()

if(NOT EXISTS "${SOURCE_PATH}/third_party/lss/lss/BUILD.gn" AND VCPKG_TARGET_IS_LINUX)
    vcpkg_from_git(OUT_SOURCE_PATH lss
        URL https://chromium.googlesource.com/linux-syscall-support
        REF 9719c1e1e676814c456b55f5f070eabad6709d31
    )
    file(REMOVE_RECURSE "${SOURCE_PATH}/third_party/lss/lss")
    file(RENAME "${lss}" "${SOURCE_PATH}/third_party/lss/lss")
endif()

# Rename the archives to the vcpkg_crashpad_* names crashpadConfig.cmake expects.
function(set_gn_output_name FILE TARGET_DECL OUTPUT_NAME)
    vcpkg_replace_string("${FILE}" "${TARGET_DECL} {" "${TARGET_DECL} {\n  output_name = \"${OUTPUT_NAME}\"")
endfunction()
set_gn_output_name("${SOURCE_PATH}/client/BUILD.gn" "crashpad_static_library(\"client\")" vcpkg_crashpad_client)
set_gn_output_name("${SOURCE_PATH}/client/BUILD.gn" "static_library(\"common\")" vcpkg_crashpad_client_common)
set_gn_output_name("${SOURCE_PATH}/util/BUILD.gn" "crashpad_static_library(\"util\")" vcpkg_crashpad_util)
set_gn_output_name("${SOURCE_PATH}/compat/BUILD.gn" "crashpad_static_library(target_name)" vcpkg_crashpad_compat)
set_gn_output_name("${SOURCE_PATH}/third_party/mini_chromium/mini_chromium/base/BUILD.gn" "static_library(\"base\")" vcpkg_crashpad_base)

function(replace_gn_dependency INPUT_FILE OUTPUT_FILE LIBRARY_NAMES)
    if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
        unset(_LIBRARY_DEB CACHE)
        find_library(_LIBRARY_DEB NAMES ${LIBRARY_NAMES}
          PATHS "${CURRENT_INSTALLED_DIR}/debug/lib"
          NO_DEFAULT_PATH)

        if(_LIBRARY_DEB MATCHES "-NOTFOUND")
            message(FATAL_ERROR "Could not find debug library with names: ${LIBRARY_NAMES}")
        endif()
    endif()

    unset(_LIBRARY_REL CACHE)
    find_library(_LIBRARY_REL NAMES ${LIBRARY_NAMES}
        PATHS "${CURRENT_INSTALLED_DIR}/lib"
        NO_DEFAULT_PATH)

    if(_LIBRARY_REL MATCHES "-NOTFOUND")
        message(FATAL_ERROR "Could not find release library with names: ${LIBRARY_NAMES}")
    endif()

    if(VCPKG_BUILD_TYPE STREQUAL "release")
        set(_LIBRARY_DEB ${_LIBRARY_REL})
    endif()

    set(_INCLUDE_DIR "${CURRENT_INSTALLED_DIR}/include")

    file(REMOVE "${OUTPUT_FILE}")
    configure_file("${INPUT_FILE}" "${OUTPUT_FILE}" @ONLY)
endfunction()

replace_gn_dependency(
    "${CMAKE_CURRENT_LIST_DIR}/zlib.gn"
    "${SOURCE_PATH}/third_party/zlib/BUILD.gn"
    "z;zs;zlib;zd;zsd;zlibd"
)

set(OPTIONS "target_cpu=\"${VCPKG_TARGET_ARCHITECTURE}\"")
set(OPTIONS_DBG "is_debug=true")
set(OPTIONS_REL "")

vcpkg_cmake_get_vars(cmake_vars_file)
include("${cmake_vars_file}")

if(VCPKG_TARGET_IS_LINUX)
    string(APPEND OPTIONS " target_os=\"linux\"")

    string(APPEND OPTIONS " \
        mini_chromium_cc=\"${VCPKG_DETECTED_CMAKE_C_COMPILER}\" \
        mini_chromium_cxx=\"${VCPKG_DETECTED_CMAKE_CXX_COMPILER}\" \
        mini_chromium_ar=\"${VCPKG_DETECTED_CMAKE_AR}\"")
    if(VCPKG_DETECTED_CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        string(APPEND OPTIONS " mini_chromium_is_clang=false extra_cflags=\"-Wno-error\"")
    endif()

    # libstdc++ 14's C++23 std::bind_front is incompatible with Clang.
    if(VCPKG_DETECTED_CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        string(PREPEND VCPKG_COMBINED_CXX_FLAGS_DEBUG "-std=c++20 ")
        string(PREPEND VCPKG_COMBINED_CXX_FLAGS_RELEASE "-std=c++20 ")
    endif()
    set(link_flags_var EXE)
elseif(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW AND VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    # FrostIDE: the pinned clang-cl (D2). mini_chromium's clang toolchain runs
    # <clang_path>/bin/clang-cl.exe, lld-link.exe (also as the archiver, /lib) and llvm-ml.exe.
    if(NOT DEFINED ENV{FROST_LLVM_ROOT})
        message(FATAL_ERROR "crashpad (FrostIDE overlay): FROST_LLVM_ROOT is not set; run tools/env.ps1")
    endif()
    file(TO_CMAKE_PATH "$ENV{FROST_LLVM_ROOT}" frost_llvm_root)
    if(NOT EXISTS "${frost_llvm_root}/bin/clang-cl.exe")
        message(FATAL_ERROR "crashpad (FrostIDE overlay): no clang-cl.exe under ${frost_llvm_root}")
    endif()
    string(APPEND OPTIONS " target_os=\"win\" mini_chromium_is_clang=true clang_path=\"${frost_llvm_root}\"")

    # The MSVC toolset and SDK that sdk/sdk.lock.json pins (the same values
    # cmake/toolchains/windows-clangcl.cmake reads; this file sits three levels below the
    # repository root). mini_chromium's tools run inside vcvarsall's environment (INCLUDE, LIB,
    # PATH, which clang-cl and lld-link honour), so vcvarsall gets the pin as arguments. The
    # Visual Studio instance is the one that contains the pinned toolset: vcpkg's own
    # VSINSTALLDIR when it does, else the first such instance vswhere enumerates (-all), as the
    # toolchain file chooses it; vswhere's "latest" is never consulted.
    file(READ "${CMAKE_CURRENT_LIST_DIR}/../../../sdk/sdk.lock.json" frost_lock)
    string(JSON frost_vctools_version GET "${frost_lock}" msvc vcToolsVersion)
    string(JSON frost_winsdk_version GET "${frost_lock}" msvc windowsSdkVersion)
    set(frost_vs_candidates "")
    if(DEFINED ENV{VSINSTALLDIR})
        file(TO_CMAKE_PATH "$ENV{VSINSTALLDIR}" frost_vs_env)
        list(APPEND frost_vs_candidates "${frost_vs_env}")
    endif()
    execute_process(
        COMMAND "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe"
            -all -products * -property installationPath
        OUTPUT_VARIABLE frost_vs_paths OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REPLACE "\n" ";" frost_vs_paths "${frost_vs_paths}")
    foreach(frost_vs IN LISTS frost_vs_paths)
        string(STRIP "${frost_vs}" frost_vs)
        file(TO_CMAKE_PATH "${frost_vs}" frost_vs)
        list(APPEND frost_vs_candidates "${frost_vs}")
    endforeach()
    set(frost_vs_install "")
    foreach(frost_vs IN LISTS frost_vs_candidates)
        string(REGEX REPLACE "/$" "" frost_vs "${frost_vs}")
        if(EXISTS "${frost_vs}/VC/Tools/MSVC/${frost_vctools_version}/include"
           AND EXISTS "${frost_vs}/VC/Auxiliary/Build/vcvarsall.bat")
            set(frost_vs_install "${frost_vs}")
            break()
        endif()
    endforeach()
    if(frost_vs_install STREQUAL "")
        message(FATAL_ERROR "crashpad (FrostIDE overlay): MSVC ${frost_vctools_version} not found in any Visual Studio install")
    endif()
    # With a trailing backslash, as vcvarsall.bat appends "VC\" to it.
    file(TO_NATIVE_PATH "${frost_vs_install}/" frost_vs_install_native)
    set(ENV{VSINSTALLDIR} "${frost_vs_install_native}")
    set(ENV{MINI_CHROMIUM_VCVARS_ARGS} "${frost_winsdk_version} -vcvars_ver=${frost_vctools_version}")
    message(STATUS "crashpad: clang-cl from ${frost_llvm_root}, MSVC ${frost_vctools_version}, SDK ${frost_winsdk_version}")

    # The ASan triplet's port flags carry -fsanitize=address, which clang-cl turns into
    # /DEFAULTLIB directives for the runtime; lld-link then needs the runtime directory, and the
    # handler (linked by lld-link directly) the inputs the clang-cl driver would add for /MD.
    if("${VCPKG_C_FLAGS}" MATCHES "-fsanitize=address")
        string(REGEX MATCH "^[0-9]+" frost_llvm_major "${VCPKG_DETECTED_CMAKE_CXX_COMPILER_VERSION}")
        set(frost_asan_dir "${frost_llvm_root}/lib/clang/${frost_llvm_major}/lib/windows")
        if(NOT EXISTS "${frost_asan_dir}/clang_rt.asan_dynamic-x86_64.lib")
            message(FATAL_ERROR "crashpad (FrostIDE overlay): no ASan runtime in ${frost_asan_dir}")
        endif()
        foreach(frost_config IN ITEMS DEBUG RELEASE)
            # Match Crashpad's sanitizer integration: its instrumented vectored heap
            # handler can recurse while processing an exception (crashpad_client_win.cc).
            string(APPEND VCPKG_COMBINED_C_FLAGS_${frost_config} " -DADDRESS_SANITIZER=1")
            string(APPEND VCPKG_COMBINED_CXX_FLAGS_${frost_config} " -DADDRESS_SANITIZER=1")
            string(APPEND VCPKG_COMBINED_EXE_LINKER_FLAGS_${frost_config}
                " \"/LIBPATH:${frost_asan_dir}\" clang_rt.asan_dynamic-x86_64.lib"
                " /INCLUDE:__asan_seh_interceptor"
                " \"/WHOLEARCHIVE:${frost_asan_dir}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib\""
                # Keep empty/suffix literals out of other instrumented globals' redzones.
                # https://github.com/llvm/llvm-project/issues/62078
                " /OPT:NOLLDTAILMERGE")
        endforeach()
    endif()
    set(link_flags_var EXE)
else()
    message(FATAL_ERROR "crashpad (FrostIDE overlay) supports Windows x64 and Linux only")
endif()

# GN strings cannot contain raw backslashes, quotes, or `$`; escape the detected flags.
foreach(flags_var IN ITEMS
        VCPKG_COMBINED_C_FLAGS_DEBUG VCPKG_COMBINED_C_FLAGS_RELEASE
        VCPKG_COMBINED_CXX_FLAGS_DEBUG VCPKG_COMBINED_CXX_FLAGS_RELEASE
        VCPKG_COMBINED_${link_flags_var}_LINKER_FLAGS_DEBUG VCPKG_COMBINED_${link_flags_var}_LINKER_FLAGS_RELEASE
        VCPKG_COMBINED_STATIC_LINKER_FLAGS_DEBUG VCPKG_COMBINED_STATIC_LINKER_FLAGS_RELEASE)
    string(REPLACE "\\" "\\\\" ${flags_var} "${${flags_var}}")
    string(REPLACE "\"" "\\\"" ${flags_var} "${${flags_var}}")
    string(REPLACE "\$" "\\\$" ${flags_var} "${${flags_var}}")
endforeach()

# extra_ldflags only affects the crashpad_handler executable; all libraries are static.
set(OPTIONS_DBG "${OPTIONS_DBG} \
    extra_cflags_c=\"${VCPKG_COMBINED_C_FLAGS_DEBUG}\" \
    extra_cflags_cc=\"${VCPKG_COMBINED_CXX_FLAGS_DEBUG}\" \
    extra_ldflags=\"${VCPKG_COMBINED_${link_flags_var}_LINKER_FLAGS_DEBUG}\" \
    extra_arflags=\"${VCPKG_COMBINED_STATIC_LINKER_FLAGS_DEBUG}\"")

set(OPTIONS_REL "${OPTIONS_REL} \
    extra_cflags_c=\"${VCPKG_COMBINED_C_FLAGS_RELEASE}\" \
    extra_cflags_cc=\"${VCPKG_COMBINED_CXX_FLAGS_RELEASE}\" \
    extra_ldflags=\"${VCPKG_COMBINED_${link_flags_var}_LINKER_FLAGS_RELEASE}\" \
    extra_arflags=\"${VCPKG_COMBINED_STATIC_LINKER_FLAGS_RELEASE}\"")

vcpkg_gn_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS "${OPTIONS}"
    OPTIONS_DEBUG "${OPTIONS_DBG}"
    OPTIONS_RELEASE "${OPTIONS_REL}"
)

vcpkg_gn_install(
    SOURCE_PATH "${SOURCE_PATH}"
    TARGETS client client:common util compat third_party/mini_chromium/mini_chromium/base handler:crashpad_handler
)

message(STATUS "Installing headers...")
set(PACKAGES_INCLUDE_DIR "${CURRENT_PACKAGES_DIR}/include/${PORT}")
function(install_headers DIR)
    file(COPY "${DIR}" DESTINATION "${PACKAGES_INCLUDE_DIR}" FILES_MATCHING PATTERN "*.h")
endfunction()
install_headers("${SOURCE_PATH}/client")
install_headers("${SOURCE_PATH}/util")
install_headers("${SOURCE_PATH}/third_party/mini_chromium/mini_chromium/base")
install_headers("${SOURCE_PATH}/third_party/mini_chromium/mini_chromium/build")

vcpkg_copy_tools(
    TOOL_NAMES crashpad_handler
    SEARCH_DIR "${CURRENT_PACKAGES_DIR}/tools")

if(NOT VCPKG_TARGET_IS_WINDOWS)
    # FrostIDE: also the copy vcpkg_copy_tools made in tools/crashpad, which users run; upstream
    # marks only the original executable, so the copy stayed 0644.
    file(CHMOD "${CURRENT_PACKAGES_DIR}/tools/crashpad_handler"
               "${CURRENT_PACKAGES_DIR}/tools/${PORT}/crashpad_handler" FILE_PERMISSIONS
      OWNER_READ OWNER_WRITE OWNER_EXECUTE
      GROUP_READ GROUP_EXECUTE
      WORLD_READ WORLD_EXECUTE
    )
endif()

# remove empty directories
file(REMOVE_RECURSE
    "${PACKAGES_INCLUDE_DIR}/util/net/testdata"
    "${PACKAGES_INCLUDE_DIR}/build/ios")

configure_file("${CMAKE_CURRENT_LIST_DIR}/crashpadConfig.cmake.in"
        "${CURRENT_PACKAGES_DIR}/share/${PORT}/crashpadConfig.cmake" @ONLY)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/${PORT}/build/config")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/${PORT}/util/mach/__pycache__")

vcpkg_copy_pdbs()
set(copyright_files
    "${SOURCE_PATH}/LICENSE"
    "${SOURCE_PATH}/third_party/mini_chromium/mini_chromium/LICENSE")
if(VCPKG_TARGET_IS_LINUX)
    # lss keeps its BSD-3-Clause notice in the header's first comment block.
    file(READ "${SOURCE_PATH}/third_party/lss/lss/linux_syscall_support.h" lss_header)
    string(FIND "${lss_header}" "*/" lss_notice_end)
    if(lss_notice_end LESS 0)
        message(FATAL_ERROR "crashpad (FrostIDE overlay): no licence comment in linux_syscall_support.h")
    endif()
    math(EXPR lss_notice_end "${lss_notice_end} + 2")
    string(SUBSTRING "${lss_header}" 0 ${lss_notice_end} lss_notice)
    file(WRITE "${CURRENT_BUILDTREES_DIR}/lss-LICENSE" "${lss_notice}\n")
    list(APPEND copyright_files "${CURRENT_BUILDTREES_DIR}/lss-LICENSE")
endif()
vcpkg_install_copyright(FILE_LIST ${copyright_files})
