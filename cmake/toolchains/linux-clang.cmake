# SPDX-License-Identifier: Apache-2.0
#
# Toolchain for FrostIDE's own code on Linux: clang and lld from the pinned
# LLVM with libstdc++ 14, the floor from PLAN.md section 1.3 (Ubuntu 24.04).
#
# Like the Windows toolchain it serves both our presets and vcpkg port builds
# (triplets/x64-linux-clang.cmake chainloads it).

include_guard(GLOBAL)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

get_filename_component(_frost_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(READ "${_frost_repo}/sdk/sdk.lock.json" _frost_lock)
string(JSON _frost_llvm_version GET "${_frost_lock}" linux llvm version)

if(DEFINED ENV{FROST_LLVM_ROOT})
    set(FROST_LLVM_ROOT "$ENV{FROST_LLVM_ROOT}")
else()
    set(FROST_LLVM_ROOT "$ENV{HOME}/sdk/llvm-${_frost_llvm_version}")
endif()
if(NOT EXISTS "${FROST_LLVM_ROOT}/bin/clang++")
    message(FATAL_ERROR "clang++ not found under ${FROST_LLVM_ROOT}; run tools/bootstrap-wsl.sh")
endif()

set(CMAKE_C_COMPILER "${FROST_LLVM_ROOT}/bin/clang")
set(CMAKE_CXX_COMPILER "${FROST_LLVM_ROOT}/bin/clang++")
set(CMAKE_AR "${FROST_LLVM_ROOT}/bin/llvm-ar")
set(CMAKE_RANLIB "${FROST_LLVM_ROOT}/bin/llvm-ranlib")
set(CMAKE_LINKER_TYPE LLD)

# Pin libstdc++ to the GCC 14 installation even when newer GCCs are present.
set(FROST_GCC_INSTALL_DIR "/usr/lib/gcc/x86_64-linux-gnu/14" CACHE PATH "GCC installation providing libstdc++")
if(NOT EXISTS "${FROST_GCC_INSTALL_DIR}")
    message(FATAL_ERROR "${FROST_GCC_INSTALL_DIR} missing; install libstdc++-14-dev (bootstrap-wsl.sh --system)")
endif()
string(APPEND CMAKE_C_FLAGS_INIT " --gcc-install-dir=${FROST_GCC_INSTALL_DIR}")
string(APPEND CMAKE_CXX_FLAGS_INIT " --gcc-install-dir=${FROST_GCC_INSTALL_DIR}")
string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " --gcc-install-dir=${FROST_GCC_INSTALL_DIR}")
string(APPEND CMAKE_SHARED_LINKER_FLAGS_INIT " --gcc-install-dir=${FROST_GCC_INSTALL_DIR}")
string(APPEND CMAKE_MODULE_LINKER_FLAGS_INIT " --gcc-install-dir=${FROST_GCC_INSTALL_DIR}")

# Inside a vcpkg port build: reuse vcpkg's standard Linux flags (-fPIC, ...).
# It does not override compilers that are already set for a native build.
if(DEFINED VCPKG_CRT_LINKAGE AND DEFINED ENV{VCPKG_ROOT})
    include("$ENV{VCPKG_ROOT}/scripts/toolchains/linux.cmake")
endif()
