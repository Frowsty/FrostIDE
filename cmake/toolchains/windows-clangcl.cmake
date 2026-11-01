# SPDX-License-Identifier: Apache-2.0
#
# Toolchain for FrostIDE's own code on Windows: clang-cl and lld-link from the
# pinned LLVM, against the pinned MSVC STL and Windows SDK (sdk/sdk.lock.json).
#
# Used two ways:
#   1. By our presets, chainloaded from vcpkg's vcpkg.cmake.
#   2. By vcpkg itself when it builds ports for the x64-windows-clangcl triplet
#      (triplets/x64-windows-clangcl.cmake sets VCPKG_CHAINLOAD_TOOLCHAIN_FILE).
#      In that mode VCPKG_CRT_LINKAGE is defined and vcpkg's standard Windows
#      flags are pulled in so ports build exactly as they would with cl.exe.
#
# The MSVC toolset and SDK are pinned with clang-cl's and lld-link's vctoolsdir,
# winsdkdir and winsdkversion options (spelled as explained at the pin below), so a
# newer Visual Studio update cannot silently change the STL under us (STL >= 14.52
# rejects Clang < 22; PLAN.md section 2).
# (/vctoolsversion is not used: it only applies together with /winsysroot and on
# its own disables clang-cl's toolchain detection.)

include_guard(GLOBAL)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# Locate the repository root from this file so the lock file can be read.
get_filename_component(_frost_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(READ "${_frost_repo}/sdk/sdk.lock.json" _frost_lock)
string(JSON _frost_llvm_version GET "${_frost_lock}" windows llvm version)
string(JSON FROST_VCTOOLS_VERSION GET "${_frost_lock}" msvc vcToolsVersion)
string(JSON FROST_WINSDK_VERSION GET "${_frost_lock}" msvc windowsSdkVersion)

# FROST_LLVM_ROOT (set by tools/env.ps1) wins; otherwise the default SDK location.
if(DEFINED ENV{FROST_LLVM_ROOT})
    file(TO_CMAKE_PATH "$ENV{FROST_LLVM_ROOT}" FROST_LLVM_ROOT)
else()
    set(FROST_LLVM_ROOT "E:/sdk/llvm-${_frost_llvm_version}")
endif()
if(NOT EXISTS "${FROST_LLVM_ROOT}/bin/clang-cl.exe")
    message(FATAL_ERROR "clang-cl not found under ${FROST_LLVM_ROOT}; run tools/bootstrap.ps1")
endif()

set(CMAKE_C_COMPILER "${FROST_LLVM_ROOT}/bin/clang-cl.exe")
set(CMAKE_CXX_COMPILER "${FROST_LLVM_ROOT}/bin/clang-cl.exe")
set(CMAKE_LINKER "${FROST_LLVM_ROOT}/bin/lld-link.exe")
set(CMAKE_AR "${FROST_LLVM_ROOT}/bin/llvm-lib.exe")
set(CMAKE_RC_COMPILER "${FROST_LLVM_ROOT}/bin/llvm-rc.exe")
set(CMAKE_MT "${FROST_LLVM_ROOT}/bin/llvm-mt.exe")
# CMake 4.x selects the linker through CMAKE_LINKER_TYPE; LLD is lld-link here.
set(CMAKE_LINKER_TYPE LLD)

# Find the Visual Studio instance that contains the pinned MSVC toolset.
set(_frost_vswhere "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe")
execute_process(COMMAND "${_frost_vswhere}" -all -products * -property installationPath
                OUTPUT_VARIABLE _frost_vs_paths OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REPLACE "\n" ";" _frost_vs_paths "${_frost_vs_paths}")
set(FROST_VCTOOLS_DIR "")
foreach(_vs IN LISTS _frost_vs_paths)
    string(STRIP "${_vs}" _vs)
    file(TO_CMAKE_PATH "${_vs}" _vs)
    if(EXISTS "${_vs}/VC/Tools/MSVC/${FROST_VCTOOLS_VERSION}/include")
        set(FROST_VCTOOLS_DIR "${_vs}/VC/Tools/MSVC/${FROST_VCTOOLS_VERSION}")
        break()
    endif()
endforeach()
if(NOT FROST_VCTOOLS_DIR)
    message(FATAL_ERROR "MSVC toolset ${FROST_VCTOOLS_VERSION} not found in any Visual Studio install")
endif()

# The Windows 10/11 SDK root from the registry (normally C:/Program Files (x86)/Windows Kits/10).
cmake_host_system_information(RESULT FROST_WINSDK_DIR QUERY WINDOWS_REGISTRY
    "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10)
file(TO_CMAKE_PATH "${FROST_WINSDK_DIR}" FROST_WINSDK_DIR)
string(REGEX REPLACE "/$" "" FROST_WINSDK_DIR "${FROST_WINSDK_DIR}")
if(NOT EXISTS "${FROST_WINSDK_DIR}/Include/${FROST_WINSDK_VERSION}/ucrt")
    message(FATAL_ERROR "Windows SDK ${FROST_WINSDK_VERSION} not found under '${FROST_WINSDK_DIR}'")
endif()

# Pin the STL and SDK for both the compiler and the linker. clang-cl takes these
# options joined to their value with a "-" prefix ("-vctoolsdir<dir>"), for two
# reasons, both checked with clang-cl 23.1.4 and sccache 0.19.0:
#  - sccache treats an unknown "/" option, or an unknown option's separate value,
#    as a second input file and refuses to cache the compile (FrostCompilerCache.cmake);
#  - clang-cl ignores "/winsdkversion:<version>": with a version that does not exist it
#    still picks the newest installed SDK, so that spelling pinned nothing.
# lld-link uses its own "/name:value" syntax.
set(_frost_cl_pin "\"-vctoolsdir${FROST_VCTOOLS_DIR}\" \"-winsdkdir${FROST_WINSDK_DIR}\" -winsdkversion${FROST_WINSDK_VERSION}")
set(_frost_link_pin "/vctoolsdir:\"${FROST_VCTOOLS_DIR}\" /winsdkdir:\"${FROST_WINSDK_DIR}\" /winsdkversion:${FROST_WINSDK_VERSION}")
string(APPEND CMAKE_C_FLAGS_INIT " ${_frost_cl_pin}")
string(APPEND CMAKE_CXX_FLAGS_INIT " ${_frost_cl_pin}")
# The _INIT values seed the cached CMAKE_<LANG>_FLAGS only on a tree's first configure,
# so a tree configured with the old spelling would keep it, and sccache would quietly
# cache nothing there. Rewrite exactly that old pin in the cache, once.
# TODO(M1): remove this migration once every build tree has been reconfigured.
set(_frost_cl_pin_old "/vctoolsdir \"${FROST_VCTOOLS_DIR}\" /winsdkdir \"${FROST_WINSDK_DIR}\" /winsdkversion:${FROST_WINSDK_VERSION}")
foreach(_frost_lang IN ITEMS C CXX)
    if(DEFINED CACHE{CMAKE_${_frost_lang}_FLAGS})
        set(_frost_cached "$CACHE{CMAKE_${_frost_lang}_FLAGS}")
        string(FIND "${_frost_cached}" "${_frost_cl_pin_old}" _frost_old_at)
        if(NOT _frost_old_at EQUAL -1)
            string(REPLACE "${_frost_cl_pin_old}" "${_frost_cl_pin}" _frost_cached "${_frost_cached}")
            get_property(_frost_help CACHE CMAKE_${_frost_lang}_FLAGS PROPERTY HELPSTRING)
            set(CMAKE_${_frost_lang}_FLAGS "${_frost_cached}" CACHE STRING "${_frost_help}" FORCE)
            message(STATUS "windows-clangcl: rewrote the MSVC/SDK pin in the cached CMAKE_${_frost_lang}_FLAGS")
        endif()
    endif()
endforeach()
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " ${_frost_link_pin}")
string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " ${_frost_link_pin}")
string(APPEND CMAKE_MODULE_LINKER_FLAGS_INIT " ${_frost_link_pin}")
# llvm-rc needs the SDK headers for windows.h in resource scripts.
string(APPEND CMAKE_RC_FLAGS_INIT " /I \"${FROST_WINSDK_DIR}/Include/${FROST_WINSDK_VERSION}/um\" /I \"${FROST_WINSDK_DIR}/Include/${FROST_WINSDK_VERSION}/shared\"")

# Inside a vcpkg port build: reuse vcpkg's standard Windows flags (CRT, /utf-8, ...).
if(DEFINED VCPKG_CRT_LINKAGE AND DEFINED ENV{VCPKG_ROOT})
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" _frost_vcpkg_root)
    include("${_frost_vcpkg_root}/scripts/toolchains/windows.cmake")
endif()

# AddressSanitizer link inputs. clang-cl adds these itself only when it drives the
# link; CMake links with lld-link directly, so they are passed explicitly. These
# are the same three inputs `clang-cl -fsanitize=address -###` shows for /MD:
# the runtime import library, the SEH interceptor and the whole-archive thunk.
# For FROST_SANITIZE=address the root CMakeLists.txt puts them into
# FROST_SANITIZE_LINK_OPTIONS, which frost_apply_compile_options adds per target
# (cmake/FrostCompilerOptions.cmake; frost.exe's launcher opts out, ADR 0012); port
# builds for the x64-windows-clangcl-asan triplet (VCPKG_C_FLAGS contains
# -fsanitize=address) get them here, including in CMake's own try_compile checks.
string(REGEX MATCH "^[0-9]+" _frost_llvm_major "${_frost_llvm_version}")
set(FROST_ASAN_RUNTIME_DIR "${FROST_LLVM_ROOT}/lib/clang/${_frost_llvm_major}/lib/windows")
set(FROST_ASAN_LINK_OPTIONS
    "${FROST_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.lib"
    "/INCLUDE:__asan_seh_interceptor"
    "/WHOLEARCHIVE:${FROST_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
if("${VCPKG_C_FLAGS}" MATCHES "-fsanitize=address")
    list(JOIN FROST_ASAN_LINK_OPTIONS "\" \"" _frost_asan_link)
    string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " \"${_frost_asan_link}\"")
    string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " \"${_frost_asan_link}\"")
    string(APPEND CMAKE_MODULE_LINKER_FLAGS_INIT " \"${_frost_asan_link}\"")
endif()
