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
# The MSVC toolset and SDK are pinned with clang-cl's and lld-link's /vctoolsdir,
# /winsdkdir and /winsdkversion, so a newer Visual Studio update cannot silently
# change the STL under us (STL >= 14.52 rejects Clang < 22; PLAN.md section 2).
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

# Pin the STL and SDK for both the compiler and the linker.
set(_frost_cl_pin "/vctoolsdir \"${FROST_VCTOOLS_DIR}\" /winsdkdir \"${FROST_WINSDK_DIR}\" /winsdkversion:${FROST_WINSDK_VERSION}")
set(_frost_link_pin "/vctoolsdir:\"${FROST_VCTOOLS_DIR}\" /winsdkdir:\"${FROST_WINSDK_DIR}\" /winsdkversion:${FROST_WINSDK_VERSION}")
string(APPEND CMAKE_C_FLAGS_INIT " ${_frost_cl_pin}")
string(APPEND CMAKE_CXX_FLAGS_INIT " ${_frost_cl_pin}")
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
